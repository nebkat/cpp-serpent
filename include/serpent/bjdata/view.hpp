#pragma once

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/direct.hpp>
#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/fwd.hpp>
#include <serpent/bjdata/marker.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
#include <concepts>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace serpent::bjdata {

class array_iterator;
class array_range;
class member_iterator;
class member_range;

/**
 * @brief A non-owning handle to one BJData value, holding no storage of its own.
 *
 * Never advances: copying one and indexing it leave it referring to the same value, which is
 * span/string_view semantics rather than those of a cursor. The types that do advance are
 * array_iterator and member_iterator, and detail::cursor internally.
 *
 * Every accessor is total: a malformed document, a missing key or a value of the wrong type
 * yields an invalid view or an empty optional rather than throwing. The checked accessors
 * (at, get, string, binary, span) wrap those and raise instead, so both styles run over the
 * same bytes and the same single parsing implementation.
 */
/**
 * How far a traversal got into which value of which document.
 *
 * A forward iterator has to walk a value to find its sibling, so a container read element by
 * element is scanned once to read each element and again to step over it - every byte twice, and
 * once more for each level above it. When something has already walked an element to its end,
 * this is where it says so, and the iterator steps to that instead of scanning.
 *
 * Only a completed walk is recorded. A traversal that stopped in the middle of a value leaves
 * nothing, and the iterator scans as it always did: knowing where a walk paused would not be
 * enough to resume, because a counted container's remaining count is not recoverable from a
 * position alone.
 */
struct walk_memo {
    const std::byte *base = nullptr;    ///< the document these positions are into
    const std::byte *owner = nullptr;   ///< first byte after the marker of the value walked
    const std::byte *reached = nullptr; ///< one past the end of that value

    /**
     * Whether this says anything about the value beginning at `first` of this document.
     *
     * The document is part of the question, not only the value. A memo outlives the document it
     * was taken about, so a later document allocated where an earlier one stood would otherwise
     * match a note about the dead one and resume into a position that means nothing.
     */
    [[nodiscard]] constexpr bool describes(const std::byte *document, const std::byte *first) const noexcept {
        return this->base == document && this->owner == first && this->reached != nullptr;
    }

    constexpr void note(const std::byte *document, const std::byte *first, const std::byte *end) noexcept {
        this->base = document;
        this->owner = first;
        this->reached = end;
    }

    constexpr void forget() noexcept { this->reached = nullptr; }
};

/** The memo a view uses when it was not given one. Not per-thread: this library has no threads. */
[[nodiscard]] inline walk_memo &ambient_memo() noexcept {
    static walk_memo memo;
    return memo;
}

class view {
    marker element = marker::invalid;
    std::span<const std::byte> source {}; ///< the whole document, so bounds and offsets are absolute
    const std::byte *payload = nullptr; ///< first byte after this value's marker

public:
    constexpr view() = default;
    constexpr view(marker element, std::span<const std::byte> source, const std::byte *payload) noexcept
    : element(element)
    , source(source)
    , payload(payload) {}

    /** Wraps a buffer, reading its leading marker. Performs no deep parsing. */
    [[nodiscard]] static view over(std::span<const std::byte> buffer) noexcept {
        if (buffer.empty()) return {};
        const auto kind = to_marker(buffer.front());
        if (!is_value(kind)) return {};
        return view { kind, buffer, buffer.data() + 1 };
    }

    [[nodiscard]] constexpr marker type_marker() const noexcept { return this->element; }
    [[nodiscard]] constexpr const std::byte *data() const noexcept { return this->payload; }
    /** The whole document these bytes came from. */
    [[nodiscard]] constexpr std::span<const std::byte> buffer() const noexcept { return this->source; }
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return this->payload == nullptr ? 0 : static_cast<std::size_t>(this->payload - this->source.data());
    }

    [[nodiscard]] constexpr kind type() const noexcept {
        switch (this->element) {
        case marker::null: return kind::null;
        case marker::boolean_true:
        case marker::boolean_false: return kind::boolean;
        case marker::uint8:
        case marker::int8:
        case marker::uint16:
        case marker::int16:
        case marker::uint32:
        case marker::int32:
        case marker::uint64:
        case marker::int64:
        case marker::byte: return kind::integer;
        case marker::float16:
        case marker::float32:
        case marker::float64: return kind::real;
        case marker::character:
        case marker::string:
        case marker::high_precision: return kind::string;
        case marker::array_begin: return kind::array;
        case marker::object_begin: return kind::object;
        default: return kind::invalid;
        }
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return this->type() != kind::invalid; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return this->is_valid(); }
    [[nodiscard]] constexpr bool is_null() const noexcept { return this->type() == kind::null; }
    [[nodiscard]] constexpr bool is_boolean() const noexcept { return this->type() == kind::boolean; }
    [[nodiscard]] constexpr bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] constexpr bool is_real() const noexcept { return this->type() == kind::real; }
    [[nodiscard]] constexpr bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] constexpr bool is_string() const noexcept { return this->type() == kind::string; }
    [[nodiscard]] constexpr bool is_array() const noexcept { return this->type() == kind::array; }
    [[nodiscard]] constexpr bool is_object() const noexcept { return this->type() == kind::object; }
    [[nodiscard]] bool is_binary() const noexcept { return this->as_binary().has_value(); }

    // ---------------- scalars ----------------

    [[nodiscard]] constexpr std::optional<bool> as_bool() const noexcept {
        return direct::boolean_under(this->element);
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> as_int() const noexcept {
        T value {};
        if (!direct::load_integer(this->element, this->payload, this->available(), value)) return std::nullopt;
        return value;
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> as_float() const noexcept {
        T value {};
        if (!direct::load_real(this->element, this->payload, this->available(), value)) return std::nullopt;
        return value;
    }

    /** S and H yield their raw bytes; C yields a one-character view. Never copies. */
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept {
        if (this->element == marker::character) {
            if (this->available() < 1) return std::nullopt;
            return std::string_view { reinterpret_cast<const char *>(this->payload), 1 };
        }
        if (this->element != marker::string && this->element != marker::high_precision) return std::nullopt;

        auto scanner = this->scan();
        const auto text = detail::read_key(scanner);
        if (!scanner.ok()) return std::nullopt;
        return text;
    }

    /** A [$B# or [$U# array, viewed as its raw bytes. */
    [[nodiscard]] std::optional<std::span<const std::byte>> as_binary() const noexcept {
        const auto info = this->container_header();
        if (this->element != marker::array_begin || !info.typed()) return std::nullopt;
        if (info.element != marker::byte && info.element != marker::uint8) return std::nullopt;
        if (info.body == nullptr || info.count > static_cast<std::uint64_t>(this->limit() - info.body))
            return std::nullopt;
        return std::span<const std::byte> { info.body, static_cast<std::size_t>(info.count) };
    }

    /**
     * A typed array whose element marker is exactly the one T packs as, viewed in place.
     * Zero copy demands an exact layout match, so a widening read goes through the ordinary
     * element iterator instead.
     */
    template<typename T>
    [[nodiscard]] std::optional<nonstd::unaligned_little_span<const T>> as_span() const noexcept {
        static_assert(strong_type_for<T>() != marker::invalid, "T does not correspond to a BJData strong type");

        const auto info = this->container_header();
        if (this->element != marker::array_begin || !info.typed()) return std::nullopt;
        if (info.element != strong_type_for<T>()) return std::nullopt;

        const auto width = payload_width(info.element);
        if (info.body == nullptr || info.count > static_cast<std::uint64_t>(this->limit() - info.body) / width)
            return std::nullopt;
        return nonstd::unaligned_little_span<const T> { info.body, static_cast<std::size_t>(info.count) };
    }

    // ---------------- containers ----------------

    /**
     * This value's payload, everything after its marker. Empty optional if it does not parse.
     *
     * With type_marker() this is enough to splice the value into another document with no
     * re-encoding: write the marker, then copy these bytes. The marker is not included
     * because an element of a typed container has none of its own in the stream.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> payload_bytes() const noexcept {
        if (!this->is_valid()) return std::nullopt;
        auto scanner = this->scan();
        detail::skip_value(scanner, this->element, 0);
        if (!scanner.ok()) return std::nullopt;
        return std::span<const std::byte> { this->payload, static_cast<std::size_t>(scanner.position - this->payload) };
    }

    [[nodiscard]] detail::header container_header() const noexcept {
        if (this->element != marker::array_begin && this->element != marker::object_begin) return {};
        auto scanner = this->scan();
        const auto info = detail::parse_header(scanner, this->element == marker::object_begin);
        if (!scanner.ok()) return {};
        return info;
    }

    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * How many elements are coming, when the document says so without being walked.
     *
     * Nothing for an unbounded container: size() would have to count them, and a caller asking
     * for a hint wants to avoid exactly that.
     */
    [[nodiscard]] std::optional<std::size_t> size_hint() const noexcept;

    [[nodiscard]] array_range array() const noexcept;
    [[nodiscard]] member_range items() const noexcept;

    [[nodiscard]] view operator[](std::size_t index) const noexcept;
    [[nodiscard]] view operator[](std::string_view key) const noexcept;
    [[nodiscard]] view find(std::string_view key) const noexcept { return (*this)[key]; }

    // ---------------- checked ----------------

    [[nodiscard]] view at(std::size_t index) const {
        auto result = (*this)[index];
        if (!result.is_valid()) raise(errc::out_of_range, this->offset());
        return result;
    }

    [[nodiscard]] view at(std::string_view key) const {
        auto result = (*this)[key];
        if (!result.is_valid()) raise(errc::missing_key, this->offset(), key);
        return result;
    }

    [[nodiscard]] std::string_view string() const {
        const auto result = this->as_string();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *result;
    }

    [[nodiscard]] std::span<const std::byte> binary() const {
        const auto result = this->as_binary();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *result;
    }

    template<typename T>
    [[nodiscard]] nonstd::unaligned_little_span<const T> span() const {
        const auto result = this->as_span<T>();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *result;
    }

    template<typename T>
    [[nodiscard]] std::optional<T> try_get() const noexcept {
        if constexpr (std::same_as<T, bool>)
            return this->as_bool();
        else if constexpr (std::same_as<T, std::string_view>)
            return this->as_string();
        else if constexpr (serpent::detail::string_like<T> && std::constructible_from<T, std::string_view>) {
            const auto text = this->as_string();
            if (!text) return std::nullopt;
            return T { *text };
        } else if constexpr (std::floating_point<T>)
            return this->as_float<T>();
        else if constexpr (std::integral<T>)
            return this->as_int<T>();
        else if constexpr (serpent::detail::structurally_readable<T>) {
            // Containers and optionals are filled from the shape of the document, so they
            // need no customization and must not go looking for one.
            T value {};
            if (!read_into(*this, value)) return std::nullopt;
            return value;
        } else {
            T value {};
            if (!serializer<T>::read(*this, value)) return std::nullopt;
            return value;
        }
    }

    template<typename T>
    [[nodiscard]] T get() const {
        auto result = this->try_get<T>();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

private:
    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->source.data() + this->source.size();
    }

    [[nodiscard]] constexpr std::size_t available() const noexcept {
        return this->payload != nullptr && this->payload < this->limit()
                ? static_cast<std::size_t>(this->limit() - this->payload)
                : 0;
    }

    [[nodiscard]] constexpr detail::cursor scan() const noexcept {
        return detail::cursor { this->source, this->payload };
    }

    friend class array_iterator;
    friend class member_iterator;
};

/** A key and its value, for structured bindings over items(). */
struct key_value {
    std::string_view key;
    view value;

    /** Present so this and json_key_value offer the same interface to generic code. */
    [[nodiscard]] constexpr bool key_is(std::string_view other) const noexcept { return this->key == other; }
    [[nodiscard]] std::string key_string() const { return std::string { this->key }; }
};

/**
 * @brief Forward iterator over the elements of an array.
 *
 * All the traversal state lives here - a cursor, the count still owed, and the strong type
 * when there is one - so nothing is ever materialised for the container itself.
 */
class array_iterator {
public:
    using value_type = view;
    using reference = view;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::span<const std::byte> source {};
    const std::byte *cursor = nullptr;
    marker element = marker::invalid;
    std::uint64_t remaining = 0;
    bool counted = false;
    bool exhausted = true;
    key_value current {};

    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->source.data() + this->source.size();
    }

    void normalise() noexcept {
        if (this->cursor == nullptr) {
            this->exhausted = true;
            return;
        }
        if (this->element == marker::invalid) {
            while (this->cursor < this->limit() && to_marker(*this->cursor) == marker::noop)
                ++this->cursor;
        }
        if (this->counted) {
            this->exhausted = this->remaining == 0 || this->cursor >= this->limit();
            return;
        }
        this->exhausted = this->cursor >= this->limit() || to_marker(*this->cursor) == marker::array_end;
    }

public:
    array_iterator() = default;

    array_iterator(std::span<const std::byte> source, const detail::header &info) noexcept
    : source(source)
    , cursor(info.body)
    , element(info.element)
    , remaining(info.count)
    , counted(!info.unbounded) {
        this->exhausted = info.body == nullptr;
        this->normalise();
    }

    [[nodiscard]] view operator*() const noexcept {
        if (this->exhausted) return {};
        if (this->element != marker::invalid) return view { this->element, this->source, this->cursor };

        const auto kind = to_marker(*this->cursor);
        if (!is_value(kind)) return {};
        return view { kind, this->source, this->cursor + 1 };
    }

    array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        if (this->element != marker::invalid) {
            const auto width = payload_width(this->element);
            if (static_cast<std::uint64_t>(this->limit() - this->cursor) < width) {
                this->exhausted = true;
                return *this;
            }
            this->cursor += width;
        } else {
            detail::cursor scanner { this->source, this->cursor };
            if (!scanner.need(1)) {
                this->exhausted = true;
                return *this;
            }
            const auto kind = to_marker(scanner.peek());
            if (!is_value(kind)) {
                this->exhausted = true;
                return *this;
            }
            scanner.advance(1);
            // Something already walked this element to its end, so step to where it finished
            // rather than scanning the same bytes a second time to find the same place.
            if (auto &memo = ambient_memo(); memo.describes(this->source.data(), scanner.position)) {
                scanner.position = memo.reached;
                memo.forget();
            } else {
                detail::skip_value(scanner, kind, 1);
            }
            if (!scanner.ok()) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = scanner.position;
        }

        if (this->counted && this->remaining > 0) --this->remaining;
        this->normalise();
        return *this;
    }

    array_iterator operator++(int) noexcept {
        auto previous = *this;
        ++(*this);
        return previous;
    }

    [[nodiscard]] friend bool operator==(const array_iterator &left, const array_iterator &right) noexcept {
        if (left.exhausted || right.exhausted) return left.exhausted == right.exhausted;
        return left.cursor == right.cursor;
    }
};

/** Forward iterator over the key/value pairs of an object. */
class member_iterator {
public:
    using value_type = key_value;
    // The entry is parsed once, on arrival, so dereferencing hands it back rather than
    // rebuilding it - and a forward iterator is allowed to say so.
    using reference = const key_value &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::span<const std::byte> source {};
    const std::byte *cursor = nullptr;
    marker element = marker::invalid;
    std::uint64_t remaining = 0;
    bool counted = false;
    bool exhausted = true;
    key_value current {};
    const std::byte *owner = nullptr; ///< the object being walked, for the memo

    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->source.data() + this->source.size();
    }

    /**
     * Says where this object ended, once it has been walked all the way.
     *
     * Only on exhaustion, and only for a counted object: an unbounded one ends at a terminator
     * the cursor has not stepped over, so the position here is not yet past the value.
     */
    void publish() const noexcept {
        if (this->owner == nullptr || !this->exhausted) return;

        const std::byte *end = nullptr;
        if (this->counted) {
            // Every member owed has been read, so the cursor is already past the last of them.
            if (this->remaining != 0) return;
            end = this->cursor;
        } else {
            // An unbounded object ends at a terminator the cursor stops on rather than steps
            // over, so the value ends one byte further on than the walk reached.
            if (this->cursor == nullptr || this->cursor >= this->limit()) return;
            if (to_marker(*this->cursor) != marker::object_end) return;
            end = this->cursor + 1;
        }
        ambient_memo().note(this->source.data(), this->owner, end);
    }

    /** Parses the entry at the cursor. Called once per position, by normalise(). */
    [[nodiscard]] key_value parse_here() const noexcept {
        detail::cursor scanner { this->source, this->cursor };
        const auto key = detail::read_key(scanner);
        if (!scanner.ok()) return {};

        if (this->element != marker::invalid) {
            return key_value { key, view { this->element, this->source, scanner.position } };
        }
        if (!scanner.need(1)) return {};

        const auto kind = to_marker(scanner.peek());
        if (!is_value(kind)) return {};
        return key_value { key, view { kind, this->source, scanner.position + 1 } };
    }

    void normalise() noexcept {
        if (this->cursor == nullptr) {
            this->exhausted = true;
            this->current = {};
            return;
        }
        // Unlike a typed array, an object skips noops whether or not it has a strong type.
        while (this->cursor < this->limit() && to_marker(*this->cursor) == marker::noop)
            ++this->cursor;

        if (this->counted) {
            this->exhausted = this->remaining == 0 || this->cursor >= this->limit();
        } else {
            this->exhausted = this->cursor >= this->limit() || to_marker(*this->cursor) == marker::object_end;
        }

        // The key and the value marker are read here rather than in operator*, so that
        // advancing can resume from the value instead of parsing the key a second time.
        this->current = this->exhausted ? key_value {} : this->parse_here();
    }

public:
    member_iterator() = default;

    member_iterator(std::span<const std::byte> source, const detail::header &info,
            const std::byte *owner = nullptr) noexcept
    : source(source)
    , cursor(info.body)
    , element(info.element)
    , remaining(info.count)
    , counted(!info.unbounded)
    , owner(owner) {
        this->exhausted = info.body == nullptr;
        this->normalise();
    }

    [[nodiscard]] const key_value &operator*() const noexcept { return this->current; }

    member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        // The key and the value's marker were consumed by normalise(), so resume at the value.
        if (!this->current.value.is_valid()) {
            this->exhausted = true;
            return *this;
        }

        detail::cursor scanner { this->source, this->current.value.data() };
        if (this->element != marker::invalid) {
            if (!scanner.need(payload_width(this->element))) {
                this->exhausted = true;
                return *this;
            }
            scanner.advance(payload_width(this->element));
        } else {
            detail::skip_value(scanner, this->current.value.type_marker(), 1);
            if (!scanner.ok()) {
                this->exhausted = true;
                return *this;
            }
        }

        this->cursor = scanner.position;
        if (this->counted && this->remaining > 0) --this->remaining;
        this->normalise();
        this->publish();
        return *this;
    }

    member_iterator operator++(int) noexcept {
        auto previous = *this;
        ++(*this);
        return previous;
    }

    [[nodiscard]] friend bool operator==(const member_iterator &left, const member_iterator &right) noexcept {
        if (left.exhausted || right.exhausted) return left.exhausted == right.exhausted;
        return left.cursor == right.cursor;
    }
};

class array_range : public std::ranges::view_interface<array_range> {
    array_iterator head;

public:
    array_range() = default;
    explicit array_range(array_iterator head) noexcept : head(head) {}

    [[nodiscard]] array_iterator begin() const noexcept { return this->head; }
    [[nodiscard]] array_iterator end() const noexcept { return {}; }
};

class member_range : public std::ranges::view_interface<member_range> {
    member_iterator head;

public:
    member_range() = default;
    explicit member_range(member_iterator head) noexcept : head(head) {}

    [[nodiscard]] member_iterator begin() const noexcept { return this->head; }
    [[nodiscard]] member_iterator end() const noexcept { return {}; }
};

inline array_range view::array() const noexcept {
    if (this->element != marker::array_begin) return {};
    return array_range { array_iterator { this->source, this->container_header() } };
}

inline member_range view::items() const noexcept {
    if (this->element != marker::object_begin) return {};
    return member_range { member_iterator { this->source, this->container_header(), this->payload } };
}

inline std::optional<std::size_t> view::size_hint() const noexcept {
    const auto info = this->container_header();
    if (info.body == nullptr || info.unbounded) return std::nullopt;
    return static_cast<std::size_t>(info.count);
}

inline std::size_t view::size() const noexcept {
    const auto info = this->container_header();
    if (info.body == nullptr) return 0;
    if (!info.unbounded) return static_cast<std::size_t>(info.count);

    std::size_t total = 0;
    if (this->element == marker::object_begin) {
        for ([[maybe_unused]] auto entry : this->items())
            ++total;
    } else {
        for ([[maybe_unused]] auto entry : this->array())
            ++total;
    }
    return total;
}

inline view view::operator[](std::size_t index) const noexcept {
    if (this->element != marker::array_begin) return {};

    const auto info = this->container_header();
    if (info.body == nullptr) return {};

    // A typed array has a fixed stride, so indexing is arithmetic rather than a walk.
    if (info.typed() && !info.unbounded) {
        if (index >= info.count) return {};
        const auto width = payload_width(info.element);
        // Check before forming the pointer, so an out-of-range index never computes one.
        if (static_cast<std::uint64_t>(this->limit() - info.body) < (index + 1) * width) return {};
        return view { info.element, this->source, info.body + index * width };
    }

    std::size_t position = 0;
    for (auto value : this->array()) {
        if (position++ == index) return value;
    }
    return {};
}

inline view view::operator[](std::string_view key) const noexcept {
    if (this->element != marker::object_begin) return {};
    for (auto entry : this->items()) {
        if (entry.key == key) return entry.value;
    }
    return {};
}

/**
 * Walks the whole document once, checking that every value is well formed, in bounds, and
 * that the root consumes the entire buffer. Traversal afterwards cannot fail.
 */
[[nodiscard]] inline std::expected<void, error> validate(std::span<const std::byte> buffer) noexcept {
    detail::cursor source { buffer.data(), buffer.data(), buffer.data() + buffer.size() };

    if (!source.need(1)) return std::unexpected { source.to_error() };

    const auto root = to_marker(source.peek());
    if (root == marker::extension) return std::unexpected { error { errc::extension_unsupported, 0 } };
    if (!is_value(root)) return std::unexpected { error { errc::unexpected_marker, 0 } };
    source.advance(1);

    detail::skip_value(source, root, 0);
    if (!source.ok()) return std::unexpected { source.to_error() };

    if (source.position != source.limit) {
        return std::unexpected { error { errc::trailing_data, source.offset_of(source.position) } };
    }
    return {};
}

/**
 * Fills a contiguous container of numbers from a typed array of the same numbers, in one copy.
 *
 * Found by ordinary lookup from the general container read, which otherwise reads an element at
 * a time through the iterator. Answers nothing for any other shape - an untyped array, a typed
 * one of another width - and leaves those to it.
 */
template<std::ranges::contiguous_range C, typename T = std::ranges::range_value_t<C>>
    requires (strong_type_for<T>() != marker::invalid) && std::is_arithmetic_v<T> && requires(C &out) { out.resize(std::size_t {}); }
std::optional<bool> read_sequence(const view &source, C &out) {
    const auto values = source.as_span<T>();
    if (!values) return std::nullopt;

    out.resize(values->size());
    std::ranges::copy(*values, std::ranges::begin(out));
    return true;
}

} // namespace serpent::bjdata
