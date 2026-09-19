#pragma once

// A JSON reader shaped like bjdata::reader, without what only bytes allow: BJData values
// *are* the bytes, so that one can lend them; JSON values must be constructed, so strings
// are always decoded rather than handed back as a borrow that only works when the data
// happens to contain no escapes.
//
// Everything else carries over: the same errc, the same three accessor tiers, the same
// forward iterators holding all the traversal state, and no allocation anywhere except
// where a decoded string is asked for.

#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/json/direct.hpp>
#include <serpent/config.hpp>
#include <serpent/json/scan.hpp>
#include <serpent/kind.hpp>
#include <serpent/serializer.hpp>

#include <charconv>
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
#include <tuple>

namespace serpent::json {

class reader;
class array_iterator;
class member_iterator;
template<typename Iterator>
class element_range;
using array_range = element_range<array_iterator>;
using member_range = element_range<member_iterator>;

/** @brief A handle to one JSON value inside a text buffer. */
class reader {
    std::string_view source;
    const char *first = nullptr; ///< this value's first character, whitespace already skipped

    // How far into this value a walk of it has got, for whoever steps over it next.
    //
    // A forward iterator has to know where the current value ends before it can hand over the
    // next, and the only way to know is to walk it - so a byte would be walked once by the loop
    // that wants it and again by the loop stepping over it, once for every level of nesting
    // above it. The loops already walk those bytes; this is how they tell each other: a nested
    // traversal notes here what it found out, and the iterator that owns this handle reads it.
    //
    // Which is why a reader can be moved but not copied. A copy would have a note of its own,
    // and a loop written `for (auto child : ...)` would quietly walk every byte twice; written
    // `for (auto &child : ...)` it walks them once, and the compiler insists on the second.
    mutable const char *reached = nullptr; ///< how far into this value anything has scanned
    mutable int open = 0;                  ///< containers still open at `reached`, counting from `first`

public:
    constexpr reader() = default;
    constexpr reader(std::string_view source, const char *first) noexcept : source(source), first(first) {}
    reader(const reader &) = delete;
    reader &operator=(const reader &) = delete;
    reader(reader &&) = default;
    reader &operator=(reader &&) = default;

    /** Wraps a document, skipping leading whitespace. Performs no deep parsing. */
    [[nodiscard]] static reader over(std::string_view text) noexcept {
        scanner::cursor scan { text, text.data() };
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return {};
        return reader { text, scan.position };
    }

    /**
     * Records where this value ends, so that stepping to the next need not scan it again.
     *
     * A container says this when a walk of it reaches the end. A scalar knows it as soon as it
     * has been read at all - the grammar scan that reads it is the same one an iterator would
     * run to step over it - and saying so is what keeps every scalar in an object from being
     * scanned twice, once to convert and once to find the member after it.
     *
     * An extent is a fact about the grammar, not about the conversion, so it is recorded
     * whenever the scan succeeded - including where the value scanned cleanly and then failed
     * to convert, as a real does when asked for an integer.
     */
    void note_end(const char *end) const noexcept { this->note_progress(end, 0); }

    /** Records how far a walk of this container has got, and how many containers are open there. */
    void note_progress(const char *position, int still_open) const noexcept {
        this->reached = position;
        this->open = still_open;
    }

private:
    /** Points this handle at another value of the same document, with nothing known about it yet. */
    void move_to(const char *position) noexcept {
        this->first = position;
        this->reached = nullptr;
        this->open = 0;
    }

public:

    [[nodiscard]] constexpr std::string_view buffer() const noexcept { return this->source; }
    [[nodiscard]] constexpr const char *data() const noexcept { return this->first; }

    /** The whole document this value sits in, which a reader generated for a type scans itself. */
    [[nodiscard]] constexpr std::string_view document() const noexcept { return this->source; }
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return this->first == nullptr ? 0 : static_cast<std::size_t>(this->first - this->source.data());
    }

    [[nodiscard]] kind type() const noexcept {
        if (this->first == nullptr || this->first >= this->limit()) return kind::invalid;
        switch (*this->first) {
        case 'n': return kind::null;
        case 't':
        case 'f': return kind::boolean;
        case '"': return kind::string;
        case '[': return kind::array;
        case '{': return kind::object;
        default: break;
        }
        if (*this->first == '-' || scanner::is_digit(*this->first)) return this->number_kind();
        return kind::invalid;
    }

    [[nodiscard]] bool is_valid() const noexcept { return this->type() != kind::invalid; }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }
    /** What the first character says, for the kinds one character settles - asked far more
     * often than the full classification, which has to scan a number to say which it is. */
    [[nodiscard]] SERPENT_ALWAYS_INLINE bool begins_with(char opening) const noexcept {
        return this->first != nullptr && this->first < this->limit() && *this->first == opening;
    }

    [[nodiscard]] bool is_null() const noexcept { return this->begins_with('n'); }
    [[nodiscard]] bool is_boolean() const noexcept { return this->type() == kind::boolean; }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return this->type() == kind::real; }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return this->begins_with('"'); }
    [[nodiscard]] bool is_array() const noexcept { return this->begins_with('['); }
    [[nodiscard]] bool is_object() const noexcept { return this->begins_with('{'); }

    // ---------------- scalars ----------------

    bool read_string_into(std::string &destination) const {
        const auto text = this->scanned_string();
        if (!text) return false;
        scanner::decode_string(*text, destination);
        return true;
    }

    /** Decodes into caller storage, for the paths that may not allocate. */
    [[nodiscard]] std::optional<std::size_t> decode_string_into(std::span<char> destination) const noexcept {
        const auto text = this->scanned_string();
        if (!text) return std::nullopt;
        if (scanner::decoded_length(*text) > destination.size()) return std::nullopt;

        std::size_t written = 0;
        scanner::decode_string(*text, [&](char value) { destination[written++] = value; });
        return written;
    }

    /** Compares against a string without materialising this one. */
    [[nodiscard]] bool string_is(std::string_view other) const noexcept {
        const auto text = this->scanned_string();
        return text && scanner::equals(*text, other);
    }

    // ---------------- containers ----------------

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] array_range array() const & noexcept;
    [[nodiscard]] array_range array() && noexcept;
    [[nodiscard]] member_range items() const & noexcept;
    [[nodiscard]] member_range items() && noexcept;

    [[nodiscard]] reader operator[](std::size_t index) const noexcept;
    [[nodiscard]] reader operator[](std::string_view key) const noexcept;
    [[nodiscard]] reader find(std::string_view key) const noexcept { return (*this)[key]; }

    /** The text this value occupies, or nothing if it does not parse. */
    [[nodiscard]] std::optional<std::string_view> extent() const noexcept {
        if (this->first == nullptr) return std::nullopt;
        auto scan = this->scan();
        scanner::skip_value(scan, 0);
        if (!scan.ok()) return std::nullopt;
        return std::string_view { this->first, static_cast<std::size_t>(scan.position - this->first) };
    }

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

    /**
     * The value as a T, or nothing if it is not one: a boolean, an integer that fits, a real
     * (from a real or an integer), text as std::string or anything made from one - always a
     * copy, since a JSON string has to be decoded and one read out of a document owns its bytes
     * whatever the document does next - a container or optional filled from the shape of the
     * document, or a described or converted type.
     */
    template<typename T>
        requires (!std::same_as<T, std::string_view> && !std::same_as<T, std::span<const std::byte>>)
    [[nodiscard]] std::optional<T> as() const noexcept {
        if constexpr (std::same_as<T, bool> || std::floating_point<T> || std::integral<T>)
            return this->read_as<T>();
        else if constexpr (detail::string_like<T> && std::constructible_from<T, std::string>) {
            auto text = this->read_as<std::string>();
            if (!text) return std::nullopt;
            return T { *std::move(text) };
        } else if constexpr (detail::structurally_readable<T>) {
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

    /** JSON text is neither a string_view nor bytes to be lent: a string has to be decoded, and there is no binary. */
    template<typename T>
        requires std::same_as<T, std::string_view> || std::same_as<T, std::span<const std::byte>>
    [[nodiscard]] std::optional<T> as() const noexcept = delete("a JSON string has to be decoded, so it cannot be "
                                                              "borrowed as a string_view; read it as a std::string, "
                                                              "or use decode_string_into");

    /** as<T>(), or errc::type_mismatch thrown. */
    template<typename T>
    [[nodiscard]] T get() const {
        auto result = this->as<T>();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

private:
    [[nodiscard]] constexpr const char *limit() const noexcept { return this->source.data() + this->source.size(); }

    [[nodiscard]] scanner::cursor scan() const noexcept { return scanner::cursor { this->source, this->first }; }

    [[nodiscard]] std::string_view number_text() const noexcept {
        auto scan = this->scan();
        const auto text = scanner::scan_number(scan);
        if (!scan.ok()) return {};
        this->note_end(scan.position);
        return text;
    }

    /** A JSON number is an integer unless it carries a fraction or an exponent. */
    [[nodiscard]] kind number_kind() const noexcept {
        const auto text = this->number_text();
        if (text.empty()) return kind::invalid;
        return text.find_first_of(".eE") == std::string_view::npos ? kind::integer : kind::real;
    }

    [[nodiscard]] std::optional<scanner::string_span> scanned_string() const noexcept {
        if (this->type() != kind::string) return std::nullopt;
        auto scan = this->scan();
        const auto text = scanner::scan_string(scan);
        if (!scan.ok()) return std::nullopt;
        this->note_end(scan.position);
        return text;
    }

    /**
     * Reads this value as a type known in advance, and says where it ended.
     *
     * The conversions themselves are direct::read, shared with the readers generated for
     * described types so that the two cannot come to disagree about what a value is.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> read_as() const {
        if (this->first == nullptr) return std::nullopt;
        auto scan = this->scan();
        T value {};
        if (!direct::read(scan, value)) return std::nullopt;
        this->note_end(scan.position);
        return value;
    }


    friend class array_iterator;
    friend class member_iterator;
    friend void step_over_value(scanner::cursor &scan, const reader &value) noexcept;
};

/** A key and its value. The key stays encoded until key_string() or key_is() asks. */
struct key_value {
    scanner::string_span key;
    reader value;

    [[nodiscard]] std::string key_string() const {
        std::string decoded;
        scanner::decode_string(this->key, decoded);
        return decoded;
    }

    [[nodiscard]] bool key_is(std::string_view other) const noexcept { return scanner::equals(this->key, other); }
};

namespace scanner {

/** Consumes forward until `open` containers have been closed, leaving the cursor just past. */
inline void close_containers(cursor &scan, int open) noexcept {
    while (open > 0) {
        skip_whitespace(scan);
        if (!scan.available(1)) {
            scan.fail(errc::unexpected_end);
            return;
        }
        // What stands here is a closing bracket, a separator, or a whole value - and a value is
        // stepped over as one, at the speed the scanner steps over anything.
        switch (scan.peek()) {
        case ']':
        case '}':
            --open;
            scan.advance(1);
            break;
        case ',':
        case ':': scan.advance(1); break;
        default:
            skip_value(scan, 1);
            if (!scan.ok()) return;
        }
    }
}

} // namespace scanner

/**
 * Moves a cursor standing at the start of `value` to just past it.
 *
 * By scanning it - unless something has already walked the value and noted how far it got, in
 * which case only what that walk left unread is scanned, which for a value read to its end is
 * nothing at all. This is the one place a note is consulted, whoever is stepping.
 */
inline void step_over_value(scanner::cursor &scan, const reader &value) noexcept {
    const char *const start = scan.position;
    if (value.reached != nullptr) {
        scan.position = value.reached;
        if (value.open > 0) {
            scanner::close_containers(scan, value.open);
            return;
        }
    }
    if (scan.position == start) scanner::skip_value(scan, 1);
}

/**
 * Iterator over the elements of an array.
 *
 * Owns the element it stands on and hands out a reference to it, so that a walk of that element
 * leaves its note where the step to the next one reads it; and tells the array being walked,
 * through the reference it was given to it, how far the walk has got. An input iterator, since
 * what it owns cannot be copied.
 */
class array_iterator {
public:
    using value_type = reader;
    using reference = const reader &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    reader current;
    bool exhausted = true;

public:
    array_iterator() = default;

    explicit array_iterator(const reader &container) noexcept
    : container(&container)
    , current(container.source, nullptr) {
        if (container.type() != kind::array) return;
        scanner::cursor scan { container.source, container.first + 1 };
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == ']') return;
        this->current.move_to(scan.position);
        this->exhausted = false;
        // This container is what is being walked now, one container deep into it.
        container.note_progress(scan.position, 1);
    }

    [[nodiscard]] const reader &operator*() const noexcept { return this->current; }

    array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        scanner::cursor scan { this->container->source, this->current.first };
        step_over_value(scan, this->current);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            if (scan.ok() && scan.available(1) && scan.peek() == ']') {
                scan.advance(1);
                this->container->note_end(scan.position);
            }
            return *this;
        }
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) {
            this->exhausted = true;
            return *this;
        }
        this->current.move_to(scan.position);
        this->container->note_progress(scan.position, 1);
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/** Iterator over the key/value pairs of an object; the same arrangement as array_iterator. */
class member_iterator {
public:
    using value_type = key_value;
    using reference = const key_value &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    const char *cursor = nullptr; ///< at the opening quote of the current key
    key_value current;
    bool exhausted = true;

    /** Reads the key at the cursor and sets `current` to it and its value; false if malformed. */
    bool take_entry() noexcept {
        scanner::cursor scan { this->container->source, this->cursor };
        const auto key = scanner::scan_string(scan);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') return false;
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return false;
        this->current.key = key;
        this->current.value.move_to(scan.position);
        return true;
    }

public:
    member_iterator() = default;

    explicit member_iterator(const reader &container) noexcept
    : container(&container)
    , current { {}, reader { container.source, nullptr } } {
        if (container.type() != kind::object) return;
        scanner::cursor scan { container.source, container.first + 1 };
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == '}') return;
        this->cursor = scan.position;
        this->exhausted = !this->take_entry();
        if (!this->exhausted) container.note_progress(this->current.value.first, 1);
    }

    [[nodiscard]] const key_value &operator*() const noexcept { return this->current; }
    [[nodiscard]] const key_value *operator->() const noexcept { return &this->current; }

    member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        scanner::cursor scan { this->container->source, this->current.value.first };
        step_over_value(scan, this->current.value);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            if (scan.ok() && scan.available(1) && scan.peek() == '}') {
                scan.advance(1);
                this->container->note_end(scan.position);
            }
            return *this;
        }
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) {
            this->exhausted = true;
            return *this;
        }
        this->cursor = scan.position;
        this->exhausted = !this->take_entry();
        if (!this->exhausted) this->container->note_progress(this->current.value.first, 1);
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/**
 * The elements of an array, or the members of an object, as a range.
 *
 * Made from a reader that will outlive the loop, it refers to it, and the walk leaves its note
 * there for whatever steps over that reader next. Made from a temporary - `document["a"].array()`
 * - it keeps the reader itself, and the note has nowhere further to go.
 */
template<typename Iterator>
class element_range {
    const reader *container = nullptr;
    std::optional<reader> owned;

public:
    explicit element_range(const reader &container) noexcept : container(&container) {}
    explicit element_range(reader &&container) noexcept : owned(std::move(container)) {}

    [[nodiscard]] Iterator begin() const noexcept { return Iterator { this->owned ? *this->owned : *this->container }; }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};

inline array_range reader::array() const & noexcept { return array_range { *this }; }
inline array_range reader::array() && noexcept { return array_range { std::move(*this) }; }
inline member_range reader::items() const & noexcept { return member_range { *this }; }
inline member_range reader::items() && noexcept { return member_range { std::move(*this) }; }

inline std::size_t reader::size() const noexcept {
    std::size_t total = 0;
    if (this->is_object()) {
        for ([[maybe_unused]] const auto &entry : this->items())
            ++total;
    } else if (this->is_array()) {
        for ([[maybe_unused]] const auto &entry : this->array())
            ++total;
    }
    return total;
}

inline reader reader::operator[](std::size_t index) const noexcept {
    std::size_t position = 0;
    for (const auto &element : this->array()) {
        if (position++ == index) return reader { element.source, element.first };
    }
    return {};
}

inline reader reader::operator[](std::string_view key) const noexcept {
    for (const auto &entry : this->items()) {
        if (entry.key_is(key)) return reader { entry.value.source, entry.value.first };
    }
    return {};
}

/** Walks the whole document once, checking it is well formed and consumes the whole text. */
[[nodiscard]] inline std::expected<void, error> validate(std::string_view text) noexcept {
    scanner::cursor scan { text, text.data() };
    scanner::skip_whitespace(scan);
    if (!scan.available(1)) return std::unexpected { error { errc::unexpected_end, 0 } };

    scanner::skip_value(scan, 0);
    if (!scan.ok()) return std::unexpected { scan.to_error() };

    scanner::skip_whitespace(scan);
    if (scan.position != scan.limit) {
        return std::unexpected { error { errc::trailing_data, static_cast<std::size_t>(scan.position - text.data()) } };
    }
    return {};
}

// decode() and try_decode() are in json/decode.hpp, which can reach the walking reader.

} // namespace serpent::json
