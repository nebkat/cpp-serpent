#pragma once

// An owning document tree, for building a document whose shape is decided as it is written.
//
// Its own header, included by nothing else, because the library is built the other way round:
// bytes are read in place through a reader and written straight from your types, with no tree in
// between. That is still true of everything else here - this is the escape hatch for the case
// the rest of the library cannot serve, where the fields are not known until run time and the
// document is assembled a piece at a time.
//
// Reading is not what this is for. A document you have the bytes of is already a tree, one that
// costs nothing: bjdata::reader::over(bytes) and json::reader::over(text) walk it in place. Build a value when
// you are the one producing the document.

#include <new>
#include <memory>
#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/serializer.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <cstring>
#include <utility>
#include <variant>
#include <vector>

namespace serpent {

/**
 * @brief A document held as a tree: one of the scalar kinds, an array of values, or an object.
 *
 * Every node owns what it holds, so a value can be returned, stored and added to long after the
 * code that started it has gone. That ownership is the whole point of it, and the reason it is
 * not what the rest of the library does.
 */
class value;

/**
 * One member of an object: its name, and what is under it.
 *
 * A std::pair, so that .first and .second mean here what they mean everywhere else and
 * structured bindings, std::get and comparison all come with it. The name is a value rather
 * than a std::string so that the same member serves an object that owns its names and one
 * than a std::string so that a short name, which nearly every name is, costs no allocation.
 *
 * It cannot be called .value: a member of that name in this namespace changes the meaning of
 * serpent::value, which the compiler refuses.
 */
using member = std::pair<value, value>;

namespace detail {

/**
 * A growable run of T in one allocation: the count and the capacity sit in front of the
 * elements, so whatever holds a run holds nothing but the pointer.
 *
 * std::vector would do all of this correctly and cost a second allocation for its own object;
 * a value is sixteen bytes and has room for a pointer, not for a vector. So the bookkeeping
 * moves into the block, and the lifetime of the elements is managed here - in one place, with
 * one test - rather than at every call site.
 */
template<typename T>
class run {
    std::uint32_t used = 0;
    std::uint32_t room = 0;

    /// Where the elements begin, past the header and at T's alignment.
    static constexpr std::size_t origin =
            (sizeof(std::uint32_t) * 2 + alignof(T) - 1) / alignof(T) * alignof(T);

    static_assert(alignof(T) <= alignof(std::max_align_t), "a run cannot over-align its elements");

    static std::size_t bytes_for(std::uint32_t room) noexcept { return origin + std::size_t(room) * sizeof(T); }

public:
    run() = delete;
    ~run() = delete;   ///< never destroyed as an object: release() does it

    [[nodiscard]] std::uint32_t size() const noexcept { return this->used; }
    [[nodiscard]] bool empty() const noexcept { return this->used == 0; }
    [[nodiscard]] std::uint32_t room_for() const noexcept { return this->room; }

    [[nodiscard]] T *data() noexcept {
        return reinterpret_cast<T *>(reinterpret_cast<std::byte *>(this) + origin);
    }
    [[nodiscard]] const T *data() const noexcept {
        return reinterpret_cast<const T *>(reinterpret_cast<const std::byte *>(this) + origin);
    }

    [[nodiscard]] T *begin() noexcept { return this->data(); }
    [[nodiscard]] T *end() noexcept { return this->data() + this->used; }
    [[nodiscard]] const T *begin() const noexcept { return this->data(); }
    [[nodiscard]] const T *end() const noexcept { return this->data() + this->used; }

    [[nodiscard]] T &operator[](std::size_t index) noexcept { return this->data()[index]; }
    [[nodiscard]] const T &operator[](std::size_t index) const noexcept { return this->data()[index]; }

    /**
     * Moves a whole span in at once, leaving the span's elements empty.
     *
     * A value and a member hold nothing that points back at them, so relocating one is copying
     * its bytes; a builder gathers a container's contents and then hands the lot over, which is
     * one copy and one clear rather than a move constructor per element. The clear is what makes
     * the gathered side safe to destroy afterwards: all-zero is a null value, which owns nothing.
     */
    static void place_all(run *block, std::span<T> items) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<T>, "a run relocates its elements");
        if (items.empty()) return;
        std::memcpy(block->data() + block->used, items.data(), items.size() * sizeof(T));
        std::memset(items.data(), 0, items.size() * sizeof(T));
        block->used += static_cast<std::uint32_t>(items.size());
    }

    /// Adds one where the room is known to be there, so the block can never move.
    static void place_back(run *block, T item) noexcept {
        std::construct_at(block->data() + block->used, std::move(item));
        ++block->used;
    }

    /// An empty run with room for as many, or nullptr for none at all.
    [[nodiscard]] static run *reserved(std::uint32_t room) {
        if (room == 0) return nullptr;
        auto *block = static_cast<run *>(::operator new(bytes_for(room)));
        block->used = 0;
        block->room = room;
        return block;
    }

    static void release(run *block) noexcept {
        if (block == nullptr) return;
        std::destroy_n(block->data(), block->used);
        ::operator delete(static_cast<void *>(block));
    }

    /// A copy of one, holding copies of its elements and no more room than it needs.
    [[nodiscard]] static run *copy_of(const run *other) {
        if (other == nullptr || other->used == 0) return nullptr;
        auto *block = reserved(other->used);
        // A throwing copy must leave nothing behind but the block it came from - and where
        // exceptions are off there is nothing to unwind, so the guard goes with them.
#if defined(__cpp_exceptions) && __cpp_exceptions
        try {
            std::uninitialized_copy_n(other->data(), other->used, block->data());
        } catch (...) {
            ::operator delete(static_cast<void *>(block));
            throw;
        }
#else
        std::uninitialized_copy_n(other->data(), other->used, block->data());
#endif
        block->used = other->used;
        return block;
    }

    /**
     * Adds one, growing the block when it is full and answering where the block now is.
     *
     * Doubling, because appending one at a time is how a document is built and anything less
     * makes that quadratic. The old elements move rather than copy, and a move that throws
     * would leave the run half in each block - so only a nothrow-movable T is accepted, which
     * value and member both are.
     */
    [[nodiscard]] static run *appended(run *block, T item) {
        static_assert(std::is_nothrow_move_constructible_v<T>, "a run moves its elements when it grows");
        if (block == nullptr) block = reserved(4);
        else if (block->used == block->room) {
            auto *wider = reserved(block->room * 2);
            std::uninitialized_move_n(block->data(), block->used, wider->data());
            wider->used = block->used;
            release(block);
            block = wider;
        }
        std::construct_at(block->data() + block->used, std::move(item));
        ++block->used;
        return block;
    }

    /// Drops the one at an index, keeping the order of the rest.
    static void erase_at(run *block, std::size_t index) noexcept {
        if (block == nullptr || index >= block->used) return;
        std::move(block->data() + index + 1, block->data() + block->used, block->data() + index);
        std::destroy_at(block->data() + block->used - 1);
        --block->used;
    }

    /**
     * The same block with no room to spare, or the same pointer when it already had none.
     *
     * Appending doubles, so a run built one element at a time carries up to half again as much
     * as it holds. Nothing needs that room once the building is over.
     */
    [[nodiscard]] static run *tightened(run *block) {
        if (block == nullptr || block->used == block->room) return block;
        if (block->used == 0) { release(block); return nullptr; }
        auto *exact = reserved(block->used);
        std::uninitialized_move_n(block->data(), block->used, exact->data());
        exact->used = block->used;
        release(block);
        return exact;
    }

    /// Forgets everything past a count, for a caller that has moved them away itself.
    static void shrink_to(run *block, std::uint32_t kept) noexcept {
        if (block == nullptr || kept >= block->used) return;
        std::destroy_n(block->data() + kept, block->used - kept);
        block->used = kept;
    }
};

}

class value {
public:
    using array = detail::run<value>;
    using binary = std::vector<std::byte>;

    /**
     * The members of an object, in the order they were added.
     *
     * Insertion order rather than sorted, because a document built by hand is usually read by a
     * person, and the order the fields were written in is the order they were meant in. Neither
     * format ascribes meaning to key order, so nothing downstream depends on the choice.
     */
    /**
     * The members of an object, in the order they were added.
     *
     * Insertion order rather than sorted, because a document built by hand is usually read by a
     * person, and the order the fields were written in is the order they were meant in. Neither
     * format ascribes meaning to key order, so nothing downstream depends on the choice.
     *
     * One allocation, like an array: the members sit in a run, and a name is a value like any
     * other - inline where it is short, which nearly every name is, and in a block of its own
     * where a document owns it. That a name is a value is what lets one span describe the
     * members of an owned object and of one inside a document.
     */
    class object {
        detail::run<member> *entries = nullptr;

    public:
        using entry = member;

        object() = default;
        object(std::initializer_list<std::pair<const std::string_view, value>> members);
        ~object();
        object(const object &other);
        object(object &&other) noexcept : entries(std::exchange(other.entries, nullptr)) {}
        object &operator=(const object &other);
        object &operator=(object &&other) noexcept {
            if (this != &other) { this->clear(); this->entries = std::exchange(other.entries, nullptr); }
            return *this;
        }

        [[nodiscard]] const member *begin() const noexcept;
        [[nodiscard]] const member *end() const noexcept;
        [[nodiscard]] member *begin() noexcept;
        [[nodiscard]] member *end() noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] bool empty() const noexcept { return this->size() == 0; }
        [[nodiscard]] std::span<const member> members() const noexcept;

        void clear() noexcept;

        [[nodiscard]] const value *find(std::string_view name) const noexcept;
        [[nodiscard]] value *find(std::string_view name) noexcept;
        [[nodiscard]] bool contains(std::string_view name) const noexcept { return this->find(name) != nullptr; }

        /** The member, adding it as null if it was not there. */
        value &operator[](std::string_view name);

        /**
         * Adds a member without looking for one of that name first, for a builder that adds
         * members one after another and calls coalesce_duplicates() once: looking on every
         * addition costs an object of N members N-squared comparisons.
         */
        void append(std::string_view name, value item);

        /** Room for as many members, so a builder that knows the count grows the run once. */
        void reserve(std::size_t members);

        /** Leaves one member per name - the last one added under it, where the first was. */
        void coalesce_duplicates();

        /** Removes a member, saying whether there was one. */
        bool erase(std::string_view name);

        /**
         * The same members with the same values, whatever order they were added in.
         *
         * Spelled out rather than defaulted, both because neither format ascribes meaning to
         * key order and because a defaulted one would not be found at all: without it, two
         * objects compare by converting each to a value, which compares its object again.
         */
        friend bool operator==(const object &left, const object &right) noexcept;
    };

    // ---------------- how one is held ----------------
    //
    // Sixteen bytes: an eight-byte payload, the seven more that alignment would otherwise
    // waste, and a tag. A string of fifteen characters or fewer lives in those first fifteen
    // bytes and costs no allocation at all - the same inline length std::string manages in
    // thirty-two. A longer string, and binary, keep a pointer in the payload and their length
    // in the wasted bytes beside it. An array or an object is a pointer to one.
    //
    // This was a std::variant, which is as wide as its widest alternative plus a discriminator:
    // a std::string alternative made every node forty bytes, and the node is what an array of
    // values is made of.
private:
    /**
     * The ten things a value can hold, in the low nibble of the tag.
     *
     * The four that own a block are last and contiguous, so "owns something" is one subtract and
     * one compare - which is what release() tests before it calls out to free anything.
     */
    enum class shape : std::uint8_t {
        empty = 0, truth, whole_signed, whole_unsigned, number, text_inline,
        text_block, binary_block, array_block, object_block,
    };

    static constexpr std::size_t inline_capacity = 15;


    union payload {
        bool          truth;
        std::int64_t  whole_signed;
        std::uint64_t whole_unsigned;
        double        number;
        const char      *text;
        const std::byte *bytes;
        array         *items;
        detail::run<member> *members;
        char          head[8];
    };

    payload      slot { .whole_signed = 0 };
    char         tail[7] {};   ///< a short string's remainder, or a block's length
    std::uint8_t tag {};       ///< shape in the low nibble, inline length in the high one

    [[nodiscard]] shape held() const noexcept { return static_cast<shape>(this->tag & 0x0F); }
    void set_shape(shape what) noexcept { this->tag = static_cast<std::uint8_t>(what); }

    /// The fifteen inline bytes are the payload and the bytes after it, which are contiguous.
    [[nodiscard]] const char *inline_data() const noexcept { return this->slot.head; }
    [[nodiscard]] char *inline_data() noexcept { return this->slot.head; }
    [[nodiscard]] std::size_t inline_size() const noexcept { return std::size_t(this->tag >> 4); }

    /// A block's length rides in the bytes alignment would have wasted. Four of them, so the
    /// longest string or binary is as long as anything else this library will hold.
    [[nodiscard]] std::uint32_t block_size() const noexcept {
        std::uint32_t length = 0;
        std::memcpy(&length, this->tail, sizeof(length));
        return length;
    }
    void set_block_size(std::size_t length) noexcept {
        const auto narrowed = static_cast<std::uint32_t>(length);
        std::memcpy(this->tail, &narrowed, sizeof(narrowed));
    }

    void assign_text(std::string_view text) {
        if (text.size() <= inline_capacity) {
            std::memcpy(this->inline_data(), text.data(), text.size());
            this->tag = static_cast<std::uint8_t>((text.size() << 4) | std::uint8_t(shape::text_inline));
            return;
        }
        auto *block = new char[text.size()];
        std::memcpy(block, text.data(), text.size());
        this->slot.text = block;
        this->set_block_size(text.size());
        this->set_shape(shape::text_block);
    }

    void assign_bytes(std::span<const std::byte> source) {
        if (source.empty()) { this->slot.bytes = nullptr; this->set_block_size(0); this->set_shape(shape::binary_block); return; }
        auto *block = new std::byte[source.size()];
        std::memcpy(block, source.data(), source.size());
        this->slot.bytes = block;
        this->set_block_size(source.size());
        this->set_shape(shape::binary_block);
    }

    /// Frees a block this node owns. Out of line: the node that has one is the rare node.
    void release_block() noexcept;

    /**
     * Drops what this holds.
     *
     * Only four of the fourteen shapes own anything, and a builder assigns and destroys far more
     * scalars than blocks, so the test for "owns nothing" is settled here and the freeing is a
     * call the common node never makes.
     */
    SERPENT_ALWAYS_INLINE void release() noexcept {
        const auto code = static_cast<unsigned>(this->tag & 0x0F);
        if (code - static_cast<unsigned>(shape::text_block) < 4) this->release_block();
        this->slot.whole_signed = 0;
        this->tag = 0;
    }

    void copy_from(const value &other);

public:
    value() = default;
    ~value() { this->release(); }

    value(const value &other) { this->copy_from(other); }
    value(value &&other) noexcept
    : slot(other.slot), tag(other.tag) {
        std::memcpy(this->tail, other.tail, sizeof(this->tail));
        other.slot.whole_signed = 0;
        other.tag = 0;
    }
    value &operator=(const value &other) {
        if (this != &other) { this->release(); this->copy_from(other); }
        return *this;
    }
    value &operator=(value &&other) noexcept {
        if (this != &other) {
            this->release();
            this->slot = other.slot;
            std::memcpy(this->tail, other.tail, sizeof(this->tail));
            this->tag = other.tag;
            other.slot.whole_signed = 0;
            other.tag = 0;
        }
        return *this;
    }

    value(std::nullptr_t) noexcept {}
    value(bool truth) noexcept { this->slot.truth = truth; this->set_shape(shape::truth); }

    // Split by signedness so that a value above int64 range survives, and taken as the widest
    // of each: the writer narrows every integer to the smallest marker that holds it anyway, so
    // keeping the declared width here would buy nothing but shapes to switch on.
    template<std::integral T>
    requires (!std::same_as<T, bool>)
    value(T number) noexcept {
        if constexpr (std::is_signed_v<T>) {
            this->slot.whole_signed = static_cast<std::int64_t>(number);
            this->set_shape(shape::whole_signed);
        } else {
            this->slot.whole_unsigned = static_cast<std::uint64_t>(number);
            this->set_shape(shape::whole_unsigned);
        }
    }

    template<std::floating_point T>
    value(T number) noexcept { this->slot.number = static_cast<double>(number); this->set_shape(shape::number); }

    // Spelled out rather than left to string_view, which would lose to the bool conversion.
    value(const char *text) { this->assign_text(text); }
    value(std::string_view text) { this->assign_text(text); }
    value(const std::string &text) { this->assign_text(text); }
    value(std::span<const std::byte> bytes) { this->assign_bytes(bytes); }
    value(binary bytes) { this->assign_bytes(bytes); }
    value(array *items) noexcept { this->slot.items = items; this->set_shape(shape::array_block); }
    value(detail::run<member> *members) noexcept { this->slot.members = members; this->set_shape(shape::object_block); }

    /**
     * Makes this value hold a T built from the arguments, in place - for a builder that fills
     * a tree where it stands rather than assigning a value made elsewhere.
     */

    /// Replaces whatever this held with text, without a temporary the caller has to keep.
    void assign(std::string_view text) { this->release(); this->assign_text(text); }

    /// A value holding an object with no members yet, for a builder to append to.
    [[nodiscard]] static value empty_object() noexcept {
        value held;
        held.slot.members = nullptr;
        held.set_shape(shape::object_block);
        return held;
    }

    /** An object written out at its call site: the braces a nested document is built with. */
    static value of(std::initializer_list<std::pair<const std::string_view, value>> members);

    /** An array written out at its call site. */
    static value of(std::initializer_list<value> items) {
        array *block = nullptr;
        for (const auto &item : items) block = array::appended(block, item);
        return value { block };
    }

    // ---------------- what it is ----------------

    [[nodiscard]] kind type() const noexcept {
        switch (this->held()) {
        case shape::empty:          return kind::null;
        case shape::truth:          return kind::boolean;
        case shape::whole_signed:
        case shape::whole_unsigned: return kind::integer;
        case shape::number:         return kind::real;
        case shape::text_inline:
        case shape::text_block:     return kind::string;
        case shape::binary_block:
        case shape::array_block:    return kind::array;
        case shape::object_block:   return kind::object;
        }
        return kind::null;
    }

    [[nodiscard]] bool is_null() const noexcept { return this->held() == shape::empty; }
    [[nodiscard]] bool is_boolean() const noexcept { return this->held() == shape::truth; }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return this->held() == shape::number; }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept {
        return this->held() == shape::text_inline || this->held() == shape::text_block;
    }
    [[nodiscard]] bool is_binary() const noexcept { return this->held() == shape::binary_block; }
    [[nodiscard]] bool is_array() const noexcept { return this->held() == shape::array_block; }
    [[nodiscard]] bool is_object() const noexcept { return this->held() == shape::object_block; }

    // ---------------- reading it back ----------------

    /**
     * What this holds as a T, or nothing if it is not one: a boolean, an integer that fits, a
     * real (from a real or an integer), text as std::string_view or anything made from one, or
     * binary as std::span<const std::byte>.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> as() const noexcept {
        if constexpr (std::same_as<T, bool>)
            return this->read_bool();
        else if constexpr (std::same_as<T, std::string_view>)
            return this->read_text();
        else if constexpr (std::same_as<T, std::span<const std::byte>>)
            return this->read_binary();
        else if constexpr (detail::string_like<T> && std::constructible_from<T, std::string_view>) {
            const auto text = this->read_text();
            if (!text) return std::nullopt;
            return T { *text };
        } else if constexpr (std::floating_point<T>)
            return this->read_real<T>();
        else
            return this->read_integer<T>();
    }

    /** The elements; empty when this holds no array at all. */
    [[nodiscard]] std::span<const value> as_array() const noexcept {
        if (this->held() != shape::array_block || this->slot.items == nullptr) return {};
        return { this->slot.items->data(), this->slot.items->size() };
    }
    /** The members; empty when this holds no object at all. */
    [[nodiscard]] std::span<const member> as_object() const noexcept;

    /**
     * The elements to change rather than to read.
     *
     * Writable but not growable: growing may move the block, so push_back() on the value does
     * that and hands back a reference into wherever the block now is.
     */
    [[nodiscard]] std::span<value> as_writable_array() noexcept {
        if (this->held() != shape::array_block || this->slot.items == nullptr) return {};
        return { this->slot.items->data(), this->slot.items->size() };
    }

    /**
     * The members to change rather than to read. Writable but not growable: growing may move
     * the run, so operator[] on the value does that.
     */
    [[nodiscard]] std::span<member> as_writable_object() noexcept;

    /// Adds a member without looking for one of that name first - what a builder does.
    void append_member(std::string_view name, value item);

    /// Leaves one member per name - the last added under it, where the first was.
    void coalesce_members();

    /// Removes a member by name, saying whether there was one.
    bool erase_member(std::string_view name);

    /** How many members or elements, counting a scalar as one and null as none. */
    [[nodiscard]] std::size_t size() const noexcept {
        if (this->is_array()) return this->as_array().size();
        if (this->is_object()) return this->as_object().size();
        return this->is_null() ? 0 : 1;
    }

    // ---------------- building it ----------------

    /**
     * The member under this key, which starts as null if it was not there.
     *
     * A null value becomes an object on the way, so a document can be built up from nothing,
     * one branch at a time. Anything else already holding a value is a programming error and
     * says so - it would otherwise silently discard what was there.
     */
    value &operator[](std::string_view name);

    /** The member, or a null value when there is none. Never adds. */
    [[nodiscard]] const value &operator[](std::string_view name) const noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    /** The member, or an error naming the key. */
    [[nodiscard]] const value &at(std::string_view name) const;

    /** The element, or an error when there is no such index. */
    [[nodiscard]] const value &at(std::size_t index) const {
        const auto items = this->as_array();
        if (index >= items.size()) raise(errc::out_of_range, 0);
        return items[index];
    }

    /**
     * Gives back the room a container asked for and did not use, through the whole tree.
     *
     * An array or an object grown a member at a time doubles as it goes, so a tree just built
     * can be holding half again as much memory as it needs. This is std::vector's
     * shrink_to_fit, applied to every container beneath this one as well as to this one: worth
     * it for a tree that is built once and then kept, pointless for one still being added to.
     *
     * Every reference into any container in the tree is invalidated, as with a vector.
     */
    void shrink_to_fit() {
        if (this->held() == shape::array_block) {
            this->slot.items = array::tightened(this->slot.items);
            for (value &item : this->as_writable_array()) item.shrink_to_fit();
        } else if (this->held() == shape::object_block) {
            this->slot.members = detail::run<member>::tightened(this->slot.members);
            for (member &entry : this->as_writable_object()) entry.second.shrink_to_fit();
        }
    }

    /** How much room this container has, counting only itself. Its size when it has no slack. */
    [[nodiscard]] std::size_t capacity() const noexcept {
        if (this->held() == shape::array_block) return this->slot.items == nullptr ? 0 : this->slot.items->room_for();
        if (this->held() == shape::object_block) return this->slot.members == nullptr ? 0 : this->slot.members->room_for();
        return this->size();
    }

    /** Appends, turning a null value into an array first, as operator[] does for objects. */
    value &push_back(value item) {
        if (this->is_null()) { this->slot.items = nullptr; this->set_shape(shape::array_block); }
        if (!this->is_array()) raise(errc::type_mismatch, 0);
        this->slot.items = array::appended(this->slot.items, std::move(item));
        return (*this->slot.items)[this->slot.items->size() - 1];
    }

    /**
     * By what it holds, not by how it is held: a short string and a long one compare as their
     * text, and a signed and an unsigned node may hold the same number.
     */
    friend bool operator==(const value &left, const value &right) noexcept;

private:
    [[nodiscard]] std::optional<bool> read_bool() const noexcept {
        if (this->held() == shape::truth) return this->slot.truth;
        return std::nullopt;
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> read_integer() const noexcept {
        if (this->held() == shape::whole_signed) {
            const auto whole = this->slot.whole_signed;
            return std::in_range<T>(whole) ? std::optional<T> { static_cast<T>(whole) } : std::nullopt;
        }
        if (this->held() == shape::whole_unsigned) {
            const auto whole = this->slot.whole_unsigned;
            return std::in_range<T>(whole) ? std::optional<T> { static_cast<T>(whole) } : std::nullopt;
        }
        return std::nullopt;
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> read_real() const noexcept {
        if (this->held() == shape::number) return static_cast<T>(this->slot.number);
        if (const auto whole = this->read_integer<std::int64_t>()) return static_cast<T>(*whole);
        if (const auto whole = this->read_integer<std::uint64_t>()) return static_cast<T>(*whole);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> read_text() const noexcept {
        if (this->held() == shape::text_inline)
            return std::string_view { this->inline_data(), this->inline_size() };
        if (this->held() == shape::text_block)
            return std::string_view { this->slot.text, this->block_size() };
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_binary() const noexcept {
        if (this->held() == shape::binary_block)
            return std::span<const std::byte> { this->slot.bytes, this->block_size() };
        return std::nullopt;
    }
};

// A pointer's worth of payload, seven bytes of tail and the tag byte: a node the size of two
// pointers on every target, which is what lets a run hold its elements rather than point at them.
static_assert(sizeof(value) == 16);
static_assert(alignof(value) == 8);

/// The name of a member, as text; empty when the name is not text at all.
[[nodiscard]] inline std::string_view name_of(const member &entry) noexcept {
    return entry.first.as<std::string_view>().value_or(std::string_view {});
}

using member_run = detail::run<member>;

inline value::object::object(std::initializer_list<std::pair<const std::string_view, value>> members) {
    this->entries = member_run::reserved(static_cast<std::uint32_t>(members.size()));
    for (const auto &[name, held] : members)
        this->entries = member_run::appended(this->entries, member { value { name }, held });
}

inline value::object::~object() { member_run::release(this->entries); }

inline value::object::object(const object &other) : entries(member_run::copy_of(other.entries)) {}

inline value::object &value::object::operator=(const object &other) {
    if (this != &other) {
        auto *copy = member_run::copy_of(other.entries);
        member_run::release(this->entries);
        this->entries = copy;
    }
    return *this;
}

inline void value::object::clear() noexcept {
    member_run::release(this->entries);
    this->entries = nullptr;
}

inline const member *value::object::begin() const noexcept {
    return this->entries == nullptr ? nullptr : this->entries->begin();
}
inline const member *value::object::end() const noexcept {
    return this->entries == nullptr ? nullptr : this->entries->end();
}
inline member *value::object::begin() noexcept {
    return this->entries == nullptr ? nullptr : this->entries->begin();
}
inline member *value::object::end() noexcept {
    return this->entries == nullptr ? nullptr : this->entries->end();
}
inline std::size_t value::object::size() const noexcept {
    return this->entries == nullptr ? 0 : this->entries->size();
}
inline void value::release_block() noexcept {
    switch (this->held()) {
    case shape::text_block:   delete[] const_cast<char *>(this->slot.text); break;
    case shape::binary_block: delete[] const_cast<std::byte *>(this->slot.bytes); break;
    case shape::array_block:  array::release(this->slot.items); break;
    case shape::object_block: detail::run<member>::release(this->slot.members); break;
    default: break;
    }
}

inline void value::copy_from(const value &other) {
    switch (other.held()) {
    case shape::text_inline:
    case shape::empty: case shape::truth: case shape::whole_signed:
    case shape::whole_unsigned: case shape::number:
        this->slot = other.slot;
        std::memcpy(this->tail, other.tail, sizeof(this->tail));
        this->tag = other.tag;
        break;
    case shape::text_block:   this->assign_text({ other.slot.text, other.block_size() }); break;
    case shape::binary_block: this->assign_bytes({ other.slot.bytes, other.block_size() }); break;
    case shape::array_block:
        this->slot.items = array::copy_of(other.slot.items);
        this->set_shape(shape::array_block);
        break;
    case shape::object_block:
        this->slot.members = detail::run<member>::copy_of(other.slot.members);
        this->set_shape(shape::object_block);
        break;
    default: break;
    }
}

inline value &value::operator[](std::string_view name) {
    if (this->is_null()) { this->slot.members = nullptr; this->set_shape(shape::object_block); }
    if (!this->is_object()) raise(errc::type_mismatch, 0, name);
    for (auto &entry : this->as_writable_object())
        if (name_of(entry) == name) return entry.second;
    this->slot.members = detail::run<member>::appended(
            this->slot.members, member { value { name }, value {} });
    return (*this->slot.members)[this->slot.members->size() - 1].second;
}

inline value value::of(std::initializer_list<std::pair<const std::string_view, value>> members) {
    auto *run = detail::run<member>::reserved(static_cast<std::uint32_t>(members.size()));
    for (const auto &[name, held] : members)
        run = detail::run<member>::appended(run, member { value { name }, held });
    return value { run };
}

inline bool operator==(const value &left, const value &right) noexcept {
    const auto what = left.type();
    if (what != right.type()) return false;
    switch (what) {
    case kind::null:    return true;
    case kind::boolean: return left.slot.truth == right.slot.truth;
    case kind::real:    return left.slot.number == right.slot.number;
    case kind::string:  return left.read_text() == right.read_text();
    case kind::integer:
        if (const auto a = left.read_integer<std::int64_t>()) {
            const auto b = right.read_integer<std::int64_t>();
            return b && *a == *b;
        }
        {
            const auto a = left.read_integer<std::uint64_t>();
            const auto b = right.read_integer<std::uint64_t>();
            return a && b && *a == *b;
        }
    case kind::array: {
        // One kind covers both, so a binary never equals a list of numbers that spells it.
        const auto left_bytes = left.read_binary();
        const auto right_bytes = right.read_binary();
        if (left_bytes.has_value() != right_bytes.has_value()) return false;
        if (left_bytes) return std::ranges::equal(*left_bytes, *right_bytes);
        return std::ranges::equal(left.as_array(), right.as_array());
    }
    case kind::object: {
        const auto ours = left.as_object();
        if (ours.size() != right.as_object().size()) return false;
        // By name rather than in order: neither format ascribes meaning to key order.
        for (const auto &entry : ours) {
            const value &other = right[name_of(entry)];
            if (!(other == entry.second)) return false;
        }
        return true;
    }
    default:            return false;
    }
}

inline const value &value::operator[](std::string_view name) const noexcept {
    static const value nothing {};
    for (const auto &entry : this->as_object())
        if (name_of(entry) == name) return entry.second;
    return nothing;
}

inline bool value::contains(std::string_view name) const noexcept {
    for (const auto &entry : this->as_object())
        if (name_of(entry) == name) return true;
    return false;
}

inline const value &value::at(std::string_view name) const {
    for (const auto &entry : this->as_object())
        if (name_of(entry) == name) return entry.second;
    raise(errc::missing_key, 0, name);
}

/// Adds a member without looking for one of that name first - what a builder does.
inline void value::append_member(std::string_view name, value item) {
    if (this->is_null()) { this->slot.members = nullptr; this->set_shape(shape::object_block); }
    this->slot.members = detail::run<member>::appended(
            this->slot.members, member { value { name }, std::move(item) });
}

inline bool value::erase_member(std::string_view name) {
    const auto members = this->as_writable_object();
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (name_of(members[index]) == name) {
            detail::run<member>::erase_at(this->slot.members, index);
            return true;
        }
    }
    return false;
}

inline std::span<const member> value::as_object() const noexcept {
    if (this->held() != shape::object_block || this->slot.members == nullptr) return {};
    const auto *first = static_cast<const detail::run<member> *>(this->slot.members)->data();
    return { first, this->slot.members->size() };
}

inline std::span<member> value::as_writable_object() noexcept {
    if (this->held() != shape::object_block || this->slot.members == nullptr) return {};
    return { this->slot.members->data(), this->slot.members->size() };
}

inline std::span<const member> value::object::members() const noexcept {
    if (this->entries == nullptr) return {};
    const auto *first = static_cast<const detail::run<member> *>(this->entries)->data();
    return { first, this->entries->size() };
}

inline void value::object::append(std::string_view name, value item) {
    this->entries = member_run::appended(this->entries, member { value { name }, std::move(item) });
}

inline void value::object::reserve(std::size_t members) {
    if (this->entries != nullptr || members == 0) return;
    this->entries = member_run::reserved(static_cast<std::uint32_t>(members));
}

inline const value *value::object::find(std::string_view name) const noexcept {
    for (const auto &entry : this->members())
        if (name_of(entry) == name) return &entry.second;
    return nullptr;
}

inline value *value::object::find(std::string_view name) noexcept {
    for (auto *entry = this->begin(); entry != this->end(); ++entry)
        if (name_of(*entry) == name) return &entry->second;
    return nullptr;
}

inline value &value::object::operator[](std::string_view name) {
    if (auto *found = this->find(name)) return *found;
    this->append(name, value {});
    return this->entries->data()[this->entries->size() - 1].second;
}

namespace detail {

/** Leaves one member per name in a run - the last added under it, where the first was. */
inline void coalesce_run(run<member> *&entries_run) {
    if (entries_run == nullptr) return;
    const std::size_t count = entries_run->size();
    if (count < 2) return;

    // Few enough members that a hash table would cost more than it saves.
    if (count < 16) {
        auto *entries = entries_run->data();
        for (std::size_t index = 0; index < entries_run->size(); ++index) {
            for (std::size_t later = index + 1; later < entries_run->size();) {
                if (name_of(entries[later]) == name_of(entries[index])) {
                    entries[index].second = std::move(entries[later].second);
                    run<member>::erase_at(entries_run, later);
                    entries = entries_run->data();
                } else {
                    ++later;
                }
            }
        }
        return;
    }

    // A flat table of entry indices, open-addressed, at least twice the size it needs: on the
    // stack for any object of ordinary size, so that checking costs no allocation at all.
    std::size_t slots = 32;
    while (slots < count * 2) slots *= 2;
    std::array<std::uint32_t, 256> near {};
    std::vector<std::uint32_t> far;
    std::uint32_t *table = near.data();
    if (slots > near.size()) {
        far.assign(slots, 0);
        table = far.data();
    }

    // Both ends of the name and its length, so that names alike at the front - which keys of a
    // document usually are - still land in different slots. Not a hash anybody should rely on:
    // the table is indexed by it. A collision only costs the comparison that decides anyway.
    const auto hash_of = [](std::string_view name) noexcept {
        std::uint64_t head = 0;
        std::uint64_t tail = 0;
        const std::size_t take = std::min(name.size(), sizeof(head));
        std::memcpy(&head, name.data(), take);
        std::memcpy(&tail, name.data() + name.size() - take, take);
        std::uint64_t mixed = (head ^ (tail * 0x9E3779B97F4A7C15ull)) + name.size();
        mixed ^= mixed >> 32;
        mixed *= 0xD6E8FEB86659FD93ull;
        mixed ^= mixed >> 32;
        return mixed;
    };

    auto *entries = entries_run->data();
    std::uint32_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::string_view name = name_of(entries[index]);
        std::size_t slot = hash_of(name) & (slots - 1);
        while (true) {
            if (table[slot] == 0) {
                table[slot] = kept + 1;
                if (kept != index) entries[kept] = std::move(entries[index]);
                ++kept;
                break;
            }
            auto &earlier = entries[table[slot] - 1];
            if (name_of(earlier) == name) {
                earlier.second = std::move(entries[index].second);
                break;
            }
            slot = (slot + 1) & (slots - 1);
        }
    }
    run<member>::shrink_to(entries_run, kept);
}

} // namespace detail

inline void value::object::coalesce_duplicates() { detail::coalesce_run(this->entries); }

inline void value::coalesce_members() {
    if (this->held() != shape::object_block) return;
    detail::coalesce_run(this->slot.members);
}

inline bool operator==(const value::object &left, const value::object &right) noexcept {
    if (left.size() != right.size()) return false;
    return std::ranges::all_of(left.members(), [&right](const member &entry) {
        const auto *other = right.find(name_of(entry));
        return other != nullptr && *other == entry.second;
    });
}

inline bool value::object::erase(std::string_view name) {
    for (std::size_t index = 0; index < this->size(); ++index) {
        if (name_of(this->entries->data()[index]) == name) {
            member_run::erase_at(this->entries, index);
            return true;
        }
    }
    return false;
}

class value_array_scope;
class value_object_scope;

/**
 * @brief A writer whose destination is a tree rather than bytes.
 *
 * The same protocol every other writer answers, so a type reaches the tree through its own
 * conversion - annotated, tabulated or hand-written - and nothing needs a second definition to
 * be buildable this way. to_value() is the whole of the usual interface to it.
 */
class value_writer {
    // Each open container is held whole and attached to its parent when it closes, so nothing
    // ever points into a container that is still growing.
    struct frame {
        serpent::value held;
        std::string pending_key {};
        bool is_object = false;
    };

    std::vector<frame> open;
    serpent::value finished {};

    void place(serpent::value item) {
        if (this->open.empty()) {
            this->finished = std::move(item);
            return;
        }
        auto &top = this->open.back();
        if (top.is_object) {
            top.held.append_member(top.pending_key, std::move(item));
            top.pending_key.clear();
        } else {
            top.held.push_back(std::move(item));
        }
    }

public:
    friend class value_array_scope;
    friend class value_object_scope;

    void null() { this->place(serpent::value {}); }
    void boolean(bool item) { this->place(serpent::value { item }); }
    void integer(std::int64_t item) { this->place(serpent::value { item }); }
    void integer(std::uint64_t item) { this->place(serpent::value { item }); }
    void real(double item) { this->place(serpent::value { item }); }
    void string(std::string_view text) { this->place(serpent::value { text }); }
    void character(char item) { this->place(serpent::value { std::string_view { &item, 1 } }); }

    /** A number too wide for a double keeps its digits, as it does reading one back. */
    void high_precision(std::string_view digits) { this->place(serpent::value { digits }); }

    void binary(std::span<const std::byte> bytes) { this->place(serpent::value { bytes }); }

    void key(std::string_view name) {
        if (!this->open.empty()) this->open.back().pending_key = std::string { name };
    }

    template<const std::string_view &Name>
    void key_literal() {
        this->key(Name);
    }

    [[nodiscard]] value_array_scope array();
    [[nodiscard]] value_object_scope object();

    template<typename T>
    void value(const T &item) {
        emit_value(*this, item);
    }

    template<detail::byte_range R>
    void bytes(const R &items) {
        if constexpr (std::ranges::contiguous_range<R>) {
            this->binary(std::span<const std::byte> { std::ranges::data(items), std::ranges::size(items) });
        } else {
            this->binary(std::ranges::to<serpent::value::binary>(items));
        }
    }

    template<typename T>
    void emit_custom(const T &item) {
        serializer<std::remove_cvref_t<T>>::write(*this, item);
    }

    template<std::ranges::input_range R>
    void range(const R &items);

    void begin_array() { this->open.push_back(frame { serpent::value { static_cast<serpent::value::array *>(nullptr) }, {}, false }); }
    void begin_object() { this->open.push_back(frame { serpent::value::empty_object(), {}, true }); }

    void end_container() {
        auto closing = std::move(this->open.back().held);
        if (this->open.back().is_object) closing.coalesce_members();
        this->open.pop_back();
        this->place(std::move(closing));
    }

    /** The tree that was written. Empty of meaning until every scope has closed. */
    [[nodiscard]] serpent::value finish() { return std::move(this->finished); }
};

/** Closes its container on destruction, as every other writer's scope does. */
class value_array_scope {
    value_writer *out = nullptr;

public:
    explicit value_array_scope(value_writer &out) : out(&out) { this->out->begin_array(); }
    value_array_scope(const value_array_scope &) = delete;
    value_array_scope &operator=(const value_array_scope &) = delete;
    value_array_scope(value_array_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    value_array_scope &operator=(value_array_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_container();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~value_array_scope() {
        if (this->out != nullptr) this->out->end_container();
    }

    template<typename T>
    void value(const T &item) const {
        this->out->value(item);
    }
};

class value_object_scope {
    value_writer *out = nullptr;

public:
    explicit value_object_scope(value_writer &out) : out(&out) { this->out->begin_object(); }
    value_object_scope(const value_object_scope &) = delete;
    value_object_scope &operator=(const value_object_scope &) = delete;
    value_object_scope(value_object_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    value_object_scope &operator=(value_object_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_container();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~value_object_scope() {
        if (this->out != nullptr) this->out->end_container();
    }

    template<typename T>
    void member(std::string_view name, const T &item) const {
        this->out->key(name);
        this->out->value(item);
    }
};

inline value_array_scope value_writer::array() { return value_array_scope { *this }; }
inline value_object_scope value_writer::object() { return value_object_scope { *this }; }

template<std::ranges::input_range R>
void value_writer::range(const R &items) {
    const auto scope = this->array();
    for (detail::range_element_t<decltype(items)> item : items)
        this->value(item);
}

/**
 * Any serializable value as a tree, through its own conversion.
 *
 * The bridge between the two halves of the library: a type that can be written at all can be
 * written here, so a document may be built as a tree, shaped at run time, and then encoded -
 * without the type knowing a tree exists.
 */
template<typename T>
[[nodiscard]] value to_value(const T &item) {
    value_writer out;
    out.value(item);
    return out.finish();
}

/**
 * @brief A handle to one node of a tree, answering what a reader over bytes answers.
 *
 * The third of the three ways to hold a document, and the same interface as the other two: a
 * reader scans the bytes on every step, an index would record where each value ends, and this one
 * has the values already. Nothing that reads names a reader type, so a type is decoded from any
 * of them by the same code.
 *
 * A handle rather than the value itself because a reader must be able to say "no such member",
 * and a tree node has no absent state - a null pointer here is that state.
 */
class value_reader {
    const value *target = nullptr;

public:
    constexpr value_reader() = default;
    constexpr value_reader(const value &node) noexcept : target(&node) {}

    [[nodiscard]] kind type() const noexcept { return this->target == nullptr ? kind::invalid : this->target->type(); }
    [[nodiscard]] bool is_valid() const noexcept { return this->target != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }

    [[nodiscard]] bool is_null() const noexcept { return this->target != nullptr && this->target->is_null(); }
    [[nodiscard]] bool is_boolean() const noexcept { return this->target != nullptr && this->target->is_boolean(); }
    [[nodiscard]] bool is_integer() const noexcept { return this->target != nullptr && this->target->is_integer(); }
    [[nodiscard]] bool is_real() const noexcept { return this->target != nullptr && this->target->is_real(); }
    [[nodiscard]] bool is_number() const noexcept { return this->target != nullptr && this->target->is_number(); }
    [[nodiscard]] bool is_string() const noexcept { return this->target != nullptr && this->target->is_string(); }
    [[nodiscard]] bool is_array() const noexcept { return this->target != nullptr && this->target->is_array(); }
    [[nodiscard]] bool is_object() const noexcept { return this->target != nullptr && this->target->is_object(); }
    [[nodiscard]] bool is_binary() const noexcept { return this->target != nullptr && this->target->is_binary(); }

    [[nodiscard]] std::size_t size() const noexcept { return this->target == nullptr ? 0 : this->target->size(); }

    /** Already known, where a scanning reader would have to count. */
    [[nodiscard]] std::optional<std::size_t> size_hint() const noexcept {
        if (this->target == nullptr) return std::nullopt;
        if (this->target->is_array()) return this->target->as_array().size();
        return std::nullopt;
    }

    [[nodiscard]] value_reader operator[](std::string_view name) const noexcept {
        if (this->target == nullptr) return {};
        for (const auto &entry : this->target->as_object())
            if (name_of(entry) == name) return value_reader { entry.second };
        return {};
    }

    [[nodiscard]] value_reader operator[](std::size_t index) const noexcept {
        if (this->target == nullptr) return {};
        const auto items = this->target->as_array();
        if (index >= items.size()) return {};
        return value_reader { items[index] };
    }

    struct key_value;

    class array_iterator {
        const value *position = nullptr;

    public:
        using value_type = value_reader;
        using reference = value_reader;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        constexpr array_iterator() = default;
        constexpr explicit array_iterator(const value *position) noexcept : position(position) {}

        [[nodiscard]] value_reader operator*() const noexcept { return value_reader { *this->position }; }
        array_iterator &operator++() noexcept {
            ++this->position;
            return *this;
        }
        array_iterator operator++(int) noexcept {
            auto copy = *this;
            ++*this;
            return copy;
        }
        [[nodiscard]] bool operator==(const array_iterator &) const noexcept = default;
    };

    class member_iterator {
        using held = member;
        const held *position = nullptr;

    public:
        using value_type = key_value;
        using reference = key_value;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        constexpr member_iterator() = default;
        constexpr explicit member_iterator(const held *position) noexcept : position(position) {}

        [[nodiscard]] inline key_value operator*() const noexcept;
        member_iterator &operator++() noexcept {
            ++this->position;
            return *this;
        }
        member_iterator operator++(int) noexcept {
            auto copy = *this;
            ++*this;
            return copy;
        }
        [[nodiscard]] bool operator==(const member_iterator &) const noexcept = default;
    };

    class array_range {
        std::span<const value> items {};

    public:
        constexpr explicit array_range(std::span<const value> items) noexcept : items(items) {}
        [[nodiscard]] array_iterator begin() const noexcept { return array_iterator { this->items.data() }; }
        [[nodiscard]] array_iterator end() const noexcept {
            return array_iterator { this->items.data() + this->items.size() };
        }
    };

    class member_range {
        std::span<const member> members {};

    public:
        constexpr explicit member_range(std::span<const member> members) noexcept : members(members) {}
        [[nodiscard]] member_iterator begin() const noexcept { return member_iterator { this->members.data() }; }
        [[nodiscard]] member_iterator end() const noexcept {
            return member_iterator { this->members.data() + this->members.size() };
        }
    };

    [[nodiscard]] array_range array() const noexcept {
        return array_range { this->target == nullptr ? std::span<const value> {} : this->target->as_array() };
    }

    [[nodiscard]] member_range items() const noexcept {
        return member_range { this->target == nullptr ? std::span<const member> {} : this->target->as_object() };
    }

    /**
     * Whatever this node holds, as one of your types.
     *
     * The same dispatch the other readers make, and for the same reason: a scalar is answered
     * here, a container or an optional is filled from the shape it finds and must not go
     * looking for a customization, and everything else is the user's own conversion.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> as() const {
        if constexpr (std::same_as<T, bool> || std::same_as<T, std::string_view>
                || std::same_as<T, std::span<const std::byte>> || std::floating_point<T> || std::integral<T>
                || (detail::string_like<T> && std::constructible_from<T, std::string_view>)) {
            return this->target == nullptr ? std::nullopt : this->target->template as<T>();
        } else if constexpr (detail::structurally_readable<T>) {
            T item {};
            if (!read_into(*this, item)) return std::nullopt;
            return item;
        } else {
            T item {};
            if (!serializer<T>::read(*this, item)) return std::nullopt;
            return item;
        }
    }
};

/** A key and its value, the shape every reader's items() yields. */
struct value_reader::key_value {
    std::string_view key;
    value_reader value;

    [[nodiscard]] bool key_is(std::string_view other) const noexcept { return this->key == other; }
    [[nodiscard]] std::string key_string() const { return std::string { this->key }; }
};

inline value_reader::key_value value_reader::member_iterator::operator*() const noexcept {
    return key_value { name_of(*this->position), value_reader { this->position->second } };
}


/** Reads a typed value straight out of a tree, as decode() does out of bytes. */
template<typename T>
[[nodiscard]] std::optional<T> from_value(const value &tree) {
    return value_reader { tree }.template as<T>();
}

/**
 * Both directions, so a value goes wherever any other type goes: on its own, as a member of a
 * reflected struct, or as an element of a container.
 */
template<>
struct serializer<value, void> {
    template<typename Writer>
    static void write(Writer &out, const value &item) {
        switch (item.type()) {
        case kind::null:    out.null(); return;
        case kind::boolean: out.value(*item.template as<bool>()); return;
        case kind::real:    out.value(*item.template as<double>()); return;
        case kind::string:  out.value(*item.template as<std::string_view>()); return;
        case kind::integer:
            if (const auto whole = item.template as<std::int64_t>()) out.value(*whole);
            else out.value(*item.template as<std::uint64_t>());
            return;
        case kind::array:
            if (const auto bytes = item.template as<std::span<const std::byte>>()) out.bytes(*bytes);
            else out.range(item.as_array());
            return;
        case kind::object: {
            const auto scope = out.object();
            for (const auto &entry : item.as_object()) {
                out.key(name_of(entry));
                out.value(entry.second);
            }
            return;
        }
        default: out.null(); return;
        }
    }

    template<typename Source>
    static bool read(const Source &source, value &item) {
        // A format may build the tree in one pass over its own text. Found by lookup on the
        // source, as read_reflected is; one that offers nothing takes the walk below.
        if constexpr (requires { read_tree(source, item); }) {
            return read_tree(source, item);
        }
        switch (source.type()) {
        case kind::invalid: return false;
        case kind::null: item = value {}; return true;
        case kind::boolean: item = value { *source.template as<bool>() }; return true;
        case kind::integer:
            if (const auto whole = source.template as<std::int64_t>()) item = value { *whole };
            else if (const auto unsigned_whole = source.template as<std::uint64_t>()) item = value { *unsigned_whole };
            else return false;
            return true;
        case kind::real: {
            const auto number = source.template as<double>();
            if (!number) return false;
            item = value { *number };
            return true;
        }
        case kind::string: {
            const auto text = detail::text_of(source);
            if (!text) return false;
            item = value { std::string { *text } };
            return true;
        }
        case kind::array: {
            // A format with a binary type of its own keeps it binary; one without carries it as
            // an array of numbers and reads back as exactly that, which is all it ever was.
            if constexpr (requires { source.template as<std::span<const std::byte>>(); }) {
                if (const auto bytes = source.template as<std::span<const std::byte>>()) {
                    item = value { *bytes };
                    return true;
                }
            }
            value::array *items = nullptr;
            if constexpr (requires { source.size_hint(); }) {
                if (const auto hint = source.size_hint())
                    items = value::array::reserved(static_cast<std::uint32_t>(*hint));
            }
            for (const auto &element : source.array()) {
                value element_value;
                if (!read(element, element_value)) { value::array::release(items); return false; }
                items = value::array::appended(items, std::move(element_value));
            }
            item = value { std::move(items) };
            return true;
        }
        case kind::object: {
            value built = value::empty_object();
            for (const auto &entry : source.items()) {
                // key_string() rather than the key itself, which a text format leaves encoded.
                value held;
                if (!read(entry.value, held)) return false;
                built.append_member(entry.key_string(), std::move(held));
            }
            built.coalesce_members();
            item = std::move(built);
            return true;
        }
        }
        return false;
    }
};

} // namespace serpent

#include <serpent/json/tree.hpp>
