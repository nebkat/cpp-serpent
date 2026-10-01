#pragma once

// A MessagePack reader shaped like bjdata::reader: a handle to one value in a buffer, holding
// nothing of its own, with the same accessors, the same iterators and the same refusal to be
// copied. Like BJData's, it lends - text and binary come back as views of the document's bytes.
//
// MessagePack holds more than JSON does, and the JSON-shaped questions answer for what fits:
//
//   bin          an array of bytes, as BJData's [$B is - as<std::span<const std::byte>>() views
//                it whole, and array() walks it a byte at a time
//   ext          no kind at all: kind::invalid, is_extension(), and extension() for its type
//                and bytes. Stepped over like any other value, never refused.
//   a map key    has to be a str to be a member. A map keyed by anything else is a valid
//   not a str    document - validate() passes it - but items() stops at the first such key,
//                since it has no name to give it.

#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/kind.hpp>
#include <serpent/limits.hpp>
#include <serpent/msgpack/format.hpp>
#include <serpent/serializer.hpp>
#include <nonstd/unaligned.hpp>

#include <bit>
#include <concepts>
#include <expected>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace serpent::msgpack {

namespace detail {

/** Where a value's contents begin, and how far they reach: bytes for a scalar, text or binary, elements for a container. */
struct header {
    family type = family::invalid;
    std::uint64_t length = 0;
    const std::byte *body = nullptr; ///< null when the header itself does not fit, or the format byte means nothing
    std::int8_t extension_type = 0;

    [[nodiscard]] constexpr bool ok() const noexcept { return this->body != nullptr; }
};

/**
 * Reads what follows a format byte, up to where the value's contents begin, and checks that
 * they fit: all of a scalar's, text's or binary's bytes, and for a container at least a byte for
 * each element still to come, so that a count no buffer could hold is refused before anything
 * is walked.
 */
[[nodiscard]] inline header read_header(std::uint8_t head, const std::byte *payload, const std::byte *limit) noexcept {
    const auto described = shape_of(head);
    header out { described.type, described.inline_length, nullptr, 0 };
    if (described.type == family::invalid || payload == nullptr || payload > limit) return out;
    auto available = static_cast<std::size_t>(limit - payload);

    const std::byte *at = payload;
    if (described.length_bytes != 0) {
        if (available < described.length_bytes) return out;
        out.length = load_big(at, described.length_bytes);
        at += described.length_bytes;
        available -= described.length_bytes;
    }

    switch (described.type) {
    case family::extension:
        if (available < 1) return out;
        out.extension_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(*at));
        ++at;
        --available;
        if (out.length > available) return out;
        break;
    case family::string:
    case family::binary:
        if (out.length > available) return out;
        break;
    case family::array:
        if (out.length > available) return out;
        break;
    case family::map:
        if (out.length > available / 2) return out;
        break;
    default:
        // A scalar carries a fixed number of bytes, or none.
        out.length = described.fixed_bytes;
        if (out.length > available) return out;
        break;
    }
    out.body = at;
    return out;
}

/** A position in a buffer that remembers the first thing wrong with it. */
struct cursor {
    const std::byte *base = nullptr;
    const std::byte *position = nullptr;
    const std::byte *limit = nullptr;
    errc failure = errc::ok;
    const std::byte *failed_at = nullptr;

    [[nodiscard]] bool ok() const noexcept { return this->failure == errc::ok; }

    void fail(errc code, const std::byte *at) noexcept {
        if (this->ok()) {
            this->failure = code;
            this->failed_at = at;
        }
    }

    [[nodiscard]] std::size_t offset_of(const std::byte *at) const noexcept {
        return static_cast<std::size_t>(at - this->base);
    }

    [[nodiscard]] error to_error() const noexcept {
        return error { this->failure, this->offset_of(this->failed_at != nullptr ? this->failed_at : this->position) };
    }
};

/**
 * Steps over a value whose format byte has been read and whose payload begins at the cursor.
 * Containers are walked; everything else is stepped over by its header.
 */
inline void skip_value(cursor &scan, std::uint8_t head, int depth) noexcept {
    if (head == format::never_used) {
        scan.fail(errc::invalid_marker, scan.position - 1);
        return;
    }
    const auto info = read_header(head, scan.position, scan.limit);
    if (!info.ok()) {
        scan.fail(errc::unexpected_end, scan.position);
        return;
    }
    if (info.type != family::array && info.type != family::map) {
        scan.position = info.body + info.length;
        return;
    }
    if (depth >= max_depth) {
        scan.fail(errc::depth_exceeded, scan.position - 1);
        return;
    }
    scan.position = info.body;
    // A map is keys and values alternating, and either may be any value.
    const std::uint64_t values = info.type == family::map ? info.length * 2 : info.length;
    for (std::uint64_t index = 0; index < values; ++index) {
        if (scan.position >= scan.limit) {
            scan.fail(errc::unexpected_end, scan.position);
            return;
        }
        const auto next = std::to_integer<std::uint8_t>(*scan.position);
        ++scan.position;
        skip_value(scan, next, depth + 1);
        if (!scan.ok()) return;
    }
}

} // namespace detail

class array_iterator;
class member_iterator;
template<typename Iterator>
class element_range;
using array_range = element_range<array_iterator>;
using member_range = element_range<member_iterator>;

/** An extension value as it stands in the document: its type and its bytes. */
struct extension_view {
    std::int8_t type = 0;
    std::span<const std::byte> data {};
};

/**
 * @brief A handle to one MessagePack value inside a buffer, holding no storage of its own.
 *
 * Every accessor is total: a malformed document, a missing key or a value of the wrong type
 * yields an invalid reader or an empty optional rather than throwing. at() and get() raise
 * instead, over the same bytes.
 */
class reader {
    std::span<const std::byte> source {}; ///< the whole document, so bounds and offsets are absolute
    const std::byte *payload = nullptr;   ///< the first byte after this value's format byte
    std::uint8_t head = format::never_used;
    bool in_binary = false; ///< one byte of a bin, handed out as a uint8 standing where it is

    // One past the end of this value, once a walk of it has been completed - for the iterator
    // that owns this handle, which then steps there rather than walking the value a second time.
    // Why a reader can be moved and not copied, as BJData's and JSON's cannot.
    mutable const std::byte *reached = nullptr;

    friend class array_iterator;
    friend class member_iterator;

    void point(std::uint8_t format_byte, const std::byte *at, bool byte_of_binary = false) noexcept {
        this->head = format_byte;
        this->payload = at;
        this->in_binary = byte_of_binary;
        this->reached = nullptr;
    }

    [[nodiscard]] const std::byte *limit() const noexcept { return this->source.data() + this->source.size(); }

    [[nodiscard]] std::size_t available() const noexcept {
        return this->payload != nullptr && this->payload <= this->limit() ? static_cast<std::size_t>(this->limit() - this->payload) : 0;
    }

    [[nodiscard]] detail::header contents() const noexcept {
        if (this->in_binary) return { family::unsigned_integer, 1, this->payload, 0 };
        return detail::read_header(this->head, this->payload, this->limit());
    }

public:
    constexpr reader() = default;
    reader(std::uint8_t head, std::span<const std::byte> source, const std::byte *payload) noexcept
    : source(source)
    , payload(payload)
    , head(head) {}
    reader(const reader &) = delete;
    reader &operator=(const reader &) = delete;
    reader(reader &&) = default;
    reader &operator=(reader &&) = default;

    /** Wraps a buffer, reading its leading format byte. Performs no deep parsing. */
    [[nodiscard]] static reader over(std::span<const std::byte> buffer) noexcept {
        if (buffer.empty()) return {};
        return reader { std::to_integer<std::uint8_t>(buffer.front()), buffer, buffer.data() + 1 };
    }

    /** Records where this value ends, so that stepping to the next need not walk it again. */
    void note_end(const std::byte *end) const noexcept { this->reached = end; }

    /** Another handle on the same value, with nothing known about it yet. */
    [[nodiscard]] reader clone() const noexcept {
        reader copy { this->head, this->source, this->payload };
        copy.in_binary = this->in_binary;
        return copy;
    }

    [[nodiscard]] constexpr std::uint8_t format_byte() const noexcept { return this->head; }
    [[nodiscard]] constexpr const std::byte *data() const noexcept { return this->payload; }
    [[nodiscard]] constexpr std::span<const std::byte> buffer() const noexcept { return this->source; }
    [[nodiscard]] std::size_t offset() const noexcept {
        return this->payload == nullptr ? 0 : static_cast<std::size_t>(this->payload - this->source.data());
    }

    /** What the format byte announces. Nothing is read past it. */
    [[nodiscard]] family format_family() const noexcept {
        if (this->payload == nullptr) return family::invalid;
        return shape_of(this->head).type;
    }

    [[nodiscard]] kind type() const noexcept { return kind_of(this->format_family()); }

    [[nodiscard]] bool is_valid() const noexcept { return this->type() != kind::invalid; }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }
    [[nodiscard]] bool is_null() const noexcept { return this->type() == kind::null; }
    [[nodiscard]] bool is_boolean() const noexcept { return this->type() == kind::boolean; }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return this->type() == kind::real; }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return this->type() == kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return this->type() == kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return this->type() == kind::object; }
    [[nodiscard]] bool is_binary() const noexcept { return this->format_family() == family::binary; }
    [[nodiscard]] bool is_extension() const noexcept { return this->format_family() == family::extension; }

    /**
     * The whole value as it stands in the document, format byte and all: what writer::encoded()
     * takes to splice it into another document unchanged. Nothing for a byte of a bin, which
     * has no format byte of its own, or for a value that does not parse.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> encoded() const noexcept {
        if (this->payload == nullptr || this->in_binary) return std::nullopt;
        detail::cursor scan { this->source.data(), this->payload, this->limit() };
        detail::skip_value(scan, this->head, 0);
        if (!scan.ok()) return std::nullopt;
        return std::span<const std::byte> { this->payload - 1, static_cast<std::size_t>(scan.position - (this->payload - 1)) };
    }

    // ---------------- containers ----------------

    /** Elements of an array or bytes of a bin, members of a map; zero for anything else. Never walks. */
    [[nodiscard]] std::size_t size() const noexcept {
        const auto holds = this->format_family();
        if (holds != family::array && holds != family::map && holds != family::binary) return 0;
        const auto info = this->contents();
        return info.ok() ? static_cast<std::size_t>(info.length) : 0;
    }

    /** Every MessagePack container states its count, so this is size() for any that parses. */
    [[nodiscard]] std::optional<std::size_t> size_hint() const noexcept {
        const auto holds = this->format_family();
        if (holds != family::array && holds != family::map && holds != family::binary) return std::nullopt;
        const auto info = this->contents();
        if (!info.ok()) return std::nullopt;
        return static_cast<std::size_t>(info.length);
    }

    [[nodiscard]] array_range array() const & noexcept;
    [[nodiscard]] array_range array() && noexcept;
    [[nodiscard]] member_range items() const & noexcept;
    [[nodiscard]] member_range items() && noexcept;

    [[nodiscard]] reader operator[](std::size_t index) const noexcept;
    [[nodiscard]] reader operator[](std::string_view key) const noexcept;
    [[nodiscard]] reader find(std::string_view key) const noexcept { return (*this)[key]; }

    // ---------------- checked ----------------

    [[nodiscard]] reader at(std::size_t index) const {
        auto result = (*this)[index];
        if (!result.is_valid()) raise(errc::out_of_range, this->offset());
        return result;
    }

    [[nodiscard]] reader at(std::string_view key) const {
        auto result = (*this)[key];
        if (!result.is_valid()) raise(errc::missing_key, this->offset(), key);
        return result;
    }

    // ---------------- values ----------------

    /** An extension's type and bytes, viewed in place. */
    [[nodiscard]] std::optional<extension_view> extension() const noexcept {
        if (!this->is_extension()) return std::nullopt;
        const auto info = this->contents();
        if (!info.ok()) return std::nullopt;
        return extension_view { info.extension_type, { info.body, static_cast<std::size_t>(info.length) } };
    }

    /**
     * The value as a T, or nothing if it is not one: a boolean, an integer that fits, a real
     * (from a float, a double or an integer), a str as std::string_view (never a copy) or as
     * anything made from one, a one-byte str as a char, a bin as std::span<const std::byte>, a
     * container or optional filled from the shape of the document, or a described or converted
     * type.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> as() const noexcept {
        if constexpr (std::same_as<T, bool>) {
            if (this->format_family() != family::boolean) return std::nullopt;
            return this->head == format::boolean_true;
        } else if constexpr (std::same_as<T, char>) {
            const auto text = this->read_text();
            if (!text || text->size() != 1) return std::nullopt;
            return text->front();
        } else if constexpr (std::same_as<T, std::string_view>) {
            return this->read_text();
        } else if constexpr (std::same_as<T, std::span<const std::byte>>) {
            return this->read_binary();
        } else if constexpr (std::same_as<T, extension_view>) {
            return this->extension();
        } else if constexpr (serpent::detail::string_like<T> && std::constructible_from<T, std::string_view>) {
            const auto text = this->read_text();
            if (!text) return std::nullopt;
            return T { *text };
        } else if constexpr (std::floating_point<T>) {
            return this->read_real<T>();
        } else if constexpr (std::integral<T>) {
            return this->read_integer<T>();
        } else if constexpr (serpent::detail::structurally_readable<T>) {
            T value {};
            if (!read_into(*this, value)) return std::nullopt;
            return value;
        } else {
            T value {};
            if (!serializer<T>::read(*this, value)) return std::nullopt;
            return value;
        }
    }

    /** as<T>(), or errc::type_mismatch thrown. */
    template<typename T>
    [[nodiscard]] T get() const {
        auto result = this->as<T>();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

private:
    [[nodiscard]] std::optional<std::string_view> read_text() const noexcept {
        if (this->format_family() != family::string) return std::nullopt;
        const auto info = this->contents();
        if (!info.ok()) return std::nullopt;
        return std::string_view { reinterpret_cast<const char *>(info.body), static_cast<std::size_t>(info.length) };
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_binary() const noexcept {
        if (this->format_family() != family::binary) return std::nullopt;
        const auto info = this->contents();
        if (!info.ok()) return std::nullopt;
        return std::span<const std::byte> { info.body, static_cast<std::size_t>(info.length) };
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> read_integer() const noexcept {
        if (this->payload == nullptr) return std::nullopt;
        integer_value found {};
        if (!load_integer(this->head, this->payload, this->available(), found)) return std::nullopt;
        if (found.negative) {
            if constexpr (std::is_unsigned_v<T>) {
                return std::nullopt;
            } else {
                const auto value = static_cast<std::int64_t>(found.bits);
                if (value < static_cast<std::int64_t>(std::numeric_limits<T>::min())) return std::nullopt;
                return static_cast<T>(value);
            }
        }
        if (found.bits > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return std::nullopt;
        return static_cast<T>(found.bits);
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> read_real() const noexcept {
        if (this->payload == nullptr) return std::nullopt;
        if (this->head == format::float32 && this->available() >= 4)
            return static_cast<T>(std::bit_cast<float>(static_cast<std::uint32_t>(load_big(this->payload, 4))));
        if (this->head == format::float64 && this->available() >= 8)
            return static_cast<T>(std::bit_cast<double>(load_big(this->payload, 8)));
        integer_value found {};
        if (!load_integer(this->head, this->payload, this->available(), found)) return std::nullopt;
        if (found.negative) return static_cast<T>(static_cast<std::int64_t>(found.bits));
        return static_cast<T>(found.bits);
    }
};

/** A key and its value, for structured bindings over items(). */
struct key_value {
    std::string_view key;
    reader value;

    /** Present so this and the other formats' entries offer the same interface to generic code. */
    [[nodiscard]] constexpr bool key_is(std::string_view other) const noexcept { return this->key == other; }
    [[nodiscard]] std::string key_string() const { return std::string { this->key }; }
};

/**
 * Iterator over the elements of an array, or the bytes of a bin.
 *
 * All the traversal state lives here - a cursor and the count still owed - and the element it
 * stands on, handed out by reference so that a walk of it leaves its note where the step to the
 * next reads it. An input iterator, since what it owns cannot be copied.
 */
class array_iterator {
public:
    using value_type = reader;
    using reference = const reader &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    const std::byte *cursor = nullptr;
    std::uint64_t remaining = 0;
    bool bytes = false;
    bool exhausted = true;
    reader current;

    [[nodiscard]] const std::byte *limit() const noexcept { return this->container->limit(); }

    void normalise() noexcept {
        if (this->remaining == 0) {
            this->exhausted = true;
            this->container->note_end(this->cursor);
            return;
        }
        if (this->cursor >= this->limit()) {
            this->exhausted = true;
            return;
        }
        if (this->bytes) {
            this->current.point(format::uint8, this->cursor, true);
            return;
        }
        this->current.point(std::to_integer<std::uint8_t>(*this->cursor), this->cursor + 1);
    }

public:
    array_iterator() = default;

    explicit array_iterator(const reader &container) noexcept : container(&container) {
        this->current.source = container.source;
        const auto holds = container.format_family();
        if (holds != family::array && holds != family::binary) return;
        const auto info = container.contents();
        if (!info.ok()) return;
        this->cursor = info.body;
        this->remaining = info.length;
        this->bytes = holds == family::binary;
        this->exhausted = false;
        this->normalise();
    }

    [[nodiscard]] const reader &operator*() const noexcept { return this->current; }

    array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;
        if (this->bytes) {
            ++this->cursor;
        } else if (this->current.reached != nullptr) {
            // Something already walked this element to its end; step to where it finished.
            this->cursor = this->current.reached;
        } else {
            detail::cursor scan { this->container->source.data(), this->current.payload, this->limit() };
            detail::skip_value(scan, this->current.head, 1);
            if (!scan.ok()) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = scan.position;
        }
        --this->remaining;
        this->normalise();
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/** Iterator over the members of a map; the same arrangement as array_iterator. */
class member_iterator {
public:
    using value_type = key_value;
    using reference = const key_value &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    const std::byte *cursor = nullptr;
    std::uint64_t remaining = 0;
    bool exhausted = true;
    key_value current;

    [[nodiscard]] const std::byte *limit() const noexcept { return this->container->limit(); }

    /** Reads the key at the cursor and points the value after it. False for a key that is not a str. */
    [[nodiscard]] bool parse_here() noexcept {
        const auto key_head = std::to_integer<std::uint8_t>(*this->cursor);
        if (shape_of(key_head).type != family::string) return false;
        const auto key = detail::read_header(key_head, this->cursor + 1, this->limit());
        if (!key.ok()) return false;
        const std::byte *const value = key.body + key.length;
        if (value >= this->limit()) return false;
        this->current.key = std::string_view { reinterpret_cast<const char *>(key.body), static_cast<std::size_t>(key.length) };
        this->current.value.point(std::to_integer<std::uint8_t>(*value), value + 1);
        return true;
    }

    void normalise() noexcept {
        if (this->remaining == 0) {
            this->exhausted = true;
            this->container->note_end(this->cursor);
            return;
        }
        this->exhausted = this->cursor >= this->limit() || !this->parse_here();
    }

public:
    member_iterator() = default;

    explicit member_iterator(const reader &container) noexcept : container(&container) {
        this->current.value.source = container.source;
        if (container.format_family() != family::map) return;
        const auto info = container.contents();
        if (!info.ok()) return;
        this->cursor = info.body;
        this->remaining = info.length;
        this->exhausted = false;
        this->normalise();
    }

    [[nodiscard]] const key_value &operator*() const noexcept { return this->current; }
    [[nodiscard]] const key_value *operator->() const noexcept { return &this->current; }

    member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;
        if (this->current.value.reached != nullptr) {
            this->cursor = this->current.value.reached;
        } else {
            detail::cursor scan { this->container->source.data(), this->current.value.payload, this->limit() };
            detail::skip_value(scan, this->current.value.head, 1);
            if (!scan.ok()) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = scan.position;
        }
        --this->remaining;
        this->normalise();
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/**
 * The elements of an array, or the members of a map, as a range.
 *
 * Made from a reader that will outlive the loop, it refers to it, and the walk leaves its note
 * there; made from a temporary - `document["a"].array()` - it keeps the reader itself.
 */
template<typename Iterator>
class element_range {
    const reader *container = nullptr;
    std::optional<reader> owned;

    [[nodiscard]] const reader &of() const noexcept { return this->owned ? *this->owned : *this->container; }

public:
    explicit element_range(const reader &container) noexcept : container(&container) {}
    explicit element_range(reader &&container) noexcept : owned(std::move(container)) {}

    [[nodiscard]] Iterator begin() const noexcept { return Iterator { this->of() }; }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};

inline array_range reader::array() const & noexcept { return array_range { *this }; }
inline array_range reader::array() && noexcept { return array_range { std::move(*this) }; }
inline member_range reader::items() const & noexcept { return member_range { *this }; }
inline member_range reader::items() && noexcept { return member_range { std::move(*this) }; }

inline reader reader::operator[](std::size_t index) const noexcept {
    const auto holds = this->format_family();
    if (holds == family::binary) {
        // A fixed stride of one, so indexing is arithmetic rather than a walk.
        const auto info = this->contents();
        if (!info.ok() || index >= info.length) return {};
        reader element { format::uint8, this->source, info.body + index };
        element.in_binary = true;
        return element;
    }
    if (holds != family::array) return {};
    std::size_t position = 0;
    for (const auto &value : this->array()) {
        if (position++ == index) return value.clone();
    }
    return {};
}

inline reader reader::operator[](std::string_view key) const noexcept {
    if (this->format_family() != family::map) return {};
    for (const auto &entry : this->items()) {
        if (entry.key == key) return entry.value.clone();
    }
    return {};
}

/**
 * Walks the whole document once, checking that every value is well formed and in bounds, and
 * that the root consumes the entire buffer. Traversal afterwards cannot fail - though a map keyed
 * by something other than text still has no members to give items().
 */
[[nodiscard]] inline std::expected<void, error> validate(std::span<const std::byte> buffer) noexcept {
    detail::cursor scan { buffer.data(), buffer.data(), buffer.data() + buffer.size() };
    if (buffer.empty()) return std::unexpected { error { errc::unexpected_end, 0 } };

    const auto root = std::to_integer<std::uint8_t>(buffer.front());
    ++scan.position;
    detail::skip_value(scan, root, 0);
    if (!scan.ok()) return std::unexpected { scan.to_error() };

    if (scan.position != scan.limit)
        return std::unexpected { error { errc::trailing_data, scan.offset_of(scan.position) } };
    return {};
}

} // namespace serpent::msgpack
