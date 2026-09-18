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

#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/serializer.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
class value {
public:
    using array = std::vector<value>;
    using binary = std::vector<std::byte>;

    /**
     * The members of an object, in the order they were added.
     *
     * Insertion order rather than sorted, because a document built by hand is usually read by a
     * person, and the order the fields were written in is the order they were meant in. Neither
     * format ascribes meaning to key order, so nothing downstream depends on the choice.
     */
    class object {
        std::vector<std::pair<std::string, value>> entries;

    public:
        object() = default;
        object(std::initializer_list<std::pair<const std::string_view, value>> members) {
            this->entries.reserve(members.size());
            for (const auto &[name, held] : members) this->entries.emplace_back(std::string { name }, held);
        }

        [[nodiscard]] auto begin() const noexcept { return this->entries.begin(); }
        [[nodiscard]] auto end() const noexcept { return this->entries.end(); }
        [[nodiscard]] auto begin() noexcept { return this->entries.begin(); }
        [[nodiscard]] auto end() noexcept { return this->entries.end(); }
        [[nodiscard]] std::size_t size() const noexcept { return this->entries.size(); }
        [[nodiscard]] bool empty() const noexcept { return this->entries.empty(); }

        [[nodiscard]] const value *find(std::string_view name) const noexcept;
        [[nodiscard]] value *find(std::string_view name) noexcept;
        [[nodiscard]] bool contains(std::string_view name) const noexcept { return this->find(name) != nullptr; }

        /** The member, adding it as null if it was not there. */
        value &operator[](std::string_view name);

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

    using storage = std::variant<std::monostate,
            bool,
            std::int64_t,
            std::uint64_t,
            double,
            std::string,
            binary,
            array,
            object>;

private:
    storage held {};

public:
    value() = default;
    value(std::nullptr_t) noexcept {}
    value(bool truth) noexcept : held(truth) {}

    // Split by signedness so that a value above int64 range survives, and taken as the widest
    // of each: the writer narrows every integer to the smallest marker that holds it anyway, so
    // keeping the declared width here would buy nothing but alternatives to visit.
    template<std::integral T>
    requires (!std::same_as<T, bool>)
    value(T number) noexcept {
        if constexpr (std::is_signed_v<T>)
            this->held = static_cast<std::int64_t>(number);
        else
            this->held = static_cast<std::uint64_t>(number);
    }

    template<std::floating_point T>
    value(T number) noexcept : held(static_cast<double>(number)) {}

    // Spelled out rather than left to string_view, which would lose to the bool conversion.
    value(const char *text) : held(std::string { text }) {}
    value(std::string_view text) : held(std::string { text }) {}
    value(std::string text) noexcept : held(std::move(text)) {}
    value(std::span<const std::byte> bytes) : held(binary { bytes.begin(), bytes.end() }) {}
    value(binary bytes) noexcept : held(std::move(bytes)) {}
    value(array items) noexcept : held(std::move(items)) {}
    value(object members) noexcept : held(std::move(members)) {}

    /** An object written out at its call site: the braces a nested document is built with. */
    static value of(std::initializer_list<std::pair<const std::string_view, value>> members) {
        return value { object { members } };
    }

    /** An array written out at its call site. */
    static value of(std::initializer_list<value> items) { return value { array { items } }; }

    // ---------------- what it is ----------------

    [[nodiscard]] kind type() const noexcept {
        return std::visit(
                []<typename T>(const T &) {
                    if constexpr (std::same_as<T, std::monostate>) return kind::null;
                    else if constexpr (std::same_as<T, bool>) return kind::boolean;
                    else if constexpr (std::same_as<T, double>) return kind::real;
                    else if constexpr (std::same_as<T, std::string>) return kind::string;
                    else if constexpr (std::same_as<T, object>) return kind::object;
                    else if constexpr (std::same_as<T, array> || std::same_as<T, binary>) return kind::array;
                    else return kind::integer;
                },
                this->held);
    }

    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(this->held); }
    [[nodiscard]] bool is_boolean() const noexcept { return std::holds_alternative<bool>(this->held); }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return std::holds_alternative<double>(this->held); }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(this->held); }
    [[nodiscard]] bool is_binary() const noexcept { return std::holds_alternative<binary>(this->held); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<array>(this->held); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<object>(this->held); }

    [[nodiscard]] const storage &contents() const noexcept { return this->held; }

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

    /** The array or object contents, or nullptr when it is neither. */
    [[nodiscard]] const array *as_array() const noexcept { return std::get_if<array>(&this->held); }
    [[nodiscard]] array *as_array() noexcept { return std::get_if<array>(&this->held); }
    [[nodiscard]] const object *as_object() const noexcept { return std::get_if<object>(&this->held); }
    [[nodiscard]] object *as_object() noexcept { return std::get_if<object>(&this->held); }

    /** How many members or elements, counting a scalar as one and null as none. */
    [[nodiscard]] std::size_t size() const noexcept {
        if (const auto *items = this->as_array()) return items->size();
        if (const auto *members = this->as_object()) return members->size();
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
    value &operator[](std::string_view name) {
        if (this->is_null()) this->held = object {};
        auto *members = this->as_object();
        if (members == nullptr) raise(errc::type_mismatch, 0, name);
        return (*members)[name];
    }

    /** The member, or a null value when there is none. Never adds. */
    [[nodiscard]] const value &operator[](std::string_view name) const noexcept {
        static const value nothing {};
        const auto *members = this->as_object();
        if (members == nullptr) return nothing;
        const auto *found = members->find(name);
        return found != nullptr ? *found : nothing;
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        const auto *members = this->as_object();
        return members != nullptr && members->contains(name);
    }

    /** The member, or an error naming the key. */
    [[nodiscard]] const value &at(std::string_view name) const {
        const auto *members = this->as_object();
        const auto *found = members != nullptr ? members->find(name) : nullptr;
        if (found == nullptr) raise(errc::missing_key, 0, name);
        return *found;
    }

    [[nodiscard]] const value &at(std::size_t index) const {
        const auto *items = this->as_array();
        if (items == nullptr || index >= items->size()) raise(errc::out_of_range, 0);
        return (*items)[index];
    }

    /** Appends, turning a null value into an array first, as operator[] does for objects. */
    value &push_back(value item) {
        if (this->is_null()) this->held = array {};
        auto *items = this->as_array();
        if (items == nullptr) raise(errc::type_mismatch, 0);
        return items->emplace_back(std::move(item));
    }

    friend bool operator==(const value &left, const value &right) = default;

private:
    [[nodiscard]] std::optional<bool> read_bool() const noexcept {
        if (const auto *truth = std::get_if<bool>(&this->held)) return *truth;
        return std::nullopt;
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> read_integer() const noexcept {
        if (const auto *whole = std::get_if<std::int64_t>(&this->held))
            return std::in_range<T>(*whole) ? std::optional<T> { static_cast<T>(*whole) } : std::nullopt;
        if (const auto *whole = std::get_if<std::uint64_t>(&this->held))
            return std::in_range<T>(*whole) ? std::optional<T> { static_cast<T>(*whole) } : std::nullopt;
        return std::nullopt;
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> read_real() const noexcept {
        if (const auto *number = std::get_if<double>(&this->held)) return static_cast<T>(*number);
        if (const auto whole = this->read_integer<std::int64_t>()) return static_cast<T>(*whole);
        if (const auto whole = this->read_integer<std::uint64_t>()) return static_cast<T>(*whole);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> read_text() const noexcept {
        if (const auto *text = std::get_if<std::string>(&this->held)) return std::string_view { *text };
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_binary() const noexcept {
        if (const auto *bytes = std::get_if<binary>(&this->held)) return std::span<const std::byte> { *bytes };
        return std::nullopt;
    }
};

inline const value *value::object::find(std::string_view name) const noexcept {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    return found != this->entries.end() ? &found->second : nullptr;
}

inline value *value::object::find(std::string_view name) noexcept {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    return found != this->entries.end() ? &found->second : nullptr;
}

inline value &value::object::operator[](std::string_view name) {
    if (auto *found = this->find(name)) return *found;
    return this->entries.emplace_back(std::string { name }, value {}).second;
}

inline bool operator==(const value::object &left, const value::object &right) noexcept {
    if (left.size() != right.size()) return false;
    return std::ranges::all_of(left, [&right](const auto &entry) {
        const auto *other = right.find(entry.first);
        return other != nullptr && *other == entry.second;
    });
}

inline bool value::object::erase(std::string_view name) {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    if (found == this->entries.end()) return false;
    this->entries.erase(found);
    return true;
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
            top.held[top.pending_key] = std::move(item);
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

    void begin_array() { this->open.push_back(frame { serpent::value { serpent::value::array {} }, {}, false }); }
    void begin_object() { this->open.push_back(frame { serpent::value { serpent::value::object {} }, {}, true }); }

    void end_container() {
        auto closing = std::move(this->open.back().held);
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
        if (const auto *items = this->target->as_array()) return items->size();
        return std::nullopt;
    }

    [[nodiscard]] value_reader operator[](std::string_view name) const noexcept {
        if (this->target == nullptr) return {};
        const auto *members = this->target->as_object();
        if (members == nullptr) return {};
        const auto *found = members->find(name);
        return found != nullptr ? value_reader { *found } : value_reader {};
    }

    [[nodiscard]] value_reader operator[](std::size_t index) const noexcept {
        if (this->target == nullptr) return {};
        const auto *items = this->target->as_array();
        if (items == nullptr || index >= items->size()) return {};
        return value_reader { (*items)[index] };
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
        using held = std::pair<std::string, value>;
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
        const value::array *items = nullptr;

    public:
        constexpr explicit array_range(const value::array *items) noexcept : items(items) {}
        [[nodiscard]] array_iterator begin() const noexcept {
            return array_iterator { this->items == nullptr ? nullptr : this->items->data() };
        }
        [[nodiscard]] array_iterator end() const noexcept {
            return array_iterator { this->items == nullptr ? nullptr : this->items->data() + this->items->size() };
        }
    };

    class member_range {
        const value::object *members = nullptr;

    public:
        constexpr explicit member_range(const value::object *members) noexcept : members(members) {}
        [[nodiscard]] member_iterator begin() const noexcept {
            return member_iterator { this->members == nullptr ? nullptr : &*this->members->begin() };
        }
        [[nodiscard]] member_iterator end() const noexcept {
            return member_iterator { this->members == nullptr ? nullptr : &*this->members->begin() + this->members->size() };
        }
    };

    [[nodiscard]] array_range array() const noexcept {
        return array_range { this->target == nullptr ? nullptr : this->target->as_array() };
    }

    [[nodiscard]] member_range items() const noexcept {
        return member_range { this->target == nullptr ? nullptr : this->target->as_object() };
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
    return key_value { this->position->first, value_reader { this->position->second } };
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
        std::visit(
                [&out]<typename T>(const T &held) {
                    if constexpr (std::same_as<T, std::monostate>) {
                        out.null();
                    } else if constexpr (std::same_as<T, value::binary>) {
                        out.bytes(held);
                    } else if constexpr (std::same_as<T, value::array>) {
                        out.range(held);
                    } else if constexpr (std::same_as<T, value::object>) {
                        const auto scope = out.object();
                        for (const auto &[name, member] : held) {
                            out.key(name);
                            out.value(member);
                        }
                    } else {
                        out.value(held);
                    }
                },
                item.contents());
    }

    template<typename Source>
    static bool read(const Source &source, value &item) {
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
            value::array items;
            if constexpr (requires { source.size_hint(); }) {
                if (const auto hint = source.size_hint()) items.reserve(*hint);
            }
            for (const auto &element : source.array()) {
                if (!read(element, items.emplace_back())) return false;
            }
            item = value { std::move(items) };
            return true;
        }
        case kind::object: {
            value::object members;
            for (const auto &entry : source.items()) {
                // key_string() rather than the key itself, which a text format leaves encoded.
                if (!read(entry.value, members[entry.key_string()])) return false;
            }
            item = value { std::move(members) };
            return true;
        }
        }
        return false;
    }
};

} // namespace serpent
