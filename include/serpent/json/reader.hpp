#pragma once

// A JSON reader shaped like bjdata::view, but a reader rather than a view: BJData values
// *are* the bytes, so a view can borrow them; JSON values must be constructed, so strings
// are always decoded rather than handed back as a borrow that only works when the data
// happens to contain no escapes.
//
// Everything else carries over: the same errc, the same three accessor tiers, the same
// forward iterators holding all the traversal state, and no allocation anywhere except
// where a decoded string is asked for.

#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
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

namespace serpent::json {

class reader;
class array_iterator;
class array_range;
class member_iterator;
class member_range;

/**
 * @brief How far a traversal has got, and into which value.
 *
 * A forward iterator has to know where the current value ends before it can hand over the next,
 * and the only way to know is to walk it - so a byte would be walked once by the loop that wants
 * it and again by the loop stepping over it, once for every level of nesting above it. The loops
 * already walk those bytes; this is how they tell each other.
 *
 * It describes one value at a time, because that is all a step ever needs: the point reached
 * inside the value being left, and how many containers were open there. Each level takes it over
 * as control comes back, so it is a small struct rather than a stack.
 *
 * A traversal that finds the memo is about some other value simply scans, which is what every
 * traversal did before this existed.
 */
struct walk_memo {
    const char *base = nullptr;    ///< the document these positions are into
    const char *owner = nullptr;   ///< first character of the value being walked
    const char *reached = nullptr; ///< how far into it anything has scanned
    int open = 0;                  ///< containers still open at `reached`, counting from `owner`

    /**
     * Whether this says anything about the value beginning at `first` of this document.
     *
     * The document is part of the question, not only the value. A memo outlives the document it
     * was taken about - the default one lives as long as the thread - so a later document
     * allocated where an earlier one stood would otherwise match a note about the dead one and
     * resume into a position that means nothing. Comparing the buffer as well as the value makes
     * that impossible rather than unlikely.
     */
    [[nodiscard]] constexpr bool describes(const char *document, const char *first) const noexcept {
        return this->base == document && this->owner == first && this->reached != nullptr;
    }

    constexpr void note(const char *document, const char *first, const char *position, int still_open) noexcept {
        this->base = document;
        this->owner = first;
        this->reached = position;
        this->open = still_open;
    }
};

/**
 * The memo a reader uses when it is not given one.
 *
 * One of them, so a reader carries no ownership burden and construction is a constant address
 * rather than a lookup. Two traversals that overlap simply take it from each other, which costs
 * them the note and nothing else: a memo about another value, or another document, is ignored.
 *
 * It is not synchronised. Reading one document from two threads at once wants a memo each -
 * over(text, memo) - because a torn note could be believed. A single reader is unaffected, and
 * decode() already keeps its own.
 */
[[nodiscard]] inline walk_memo &ambient_memo() noexcept {
    static walk_memo memo;
    return memo;
}

/** @brief A handle to one JSON value inside a text buffer. */
class reader {
    std::string_view source;
    const char *first = nullptr; ///< this value's first character, whitespace already skipped
    walk_memo *memo = &ambient_memo();

public:
    constexpr reader() = default;
    reader(std::string_view source, const char *first) noexcept : source(source), first(first) {}
    constexpr reader(std::string_view source, const char *first, walk_memo *memo) noexcept
            : source(source)
            , first(first)
            , memo(memo) {}

    /** Wraps a document, skipping leading whitespace. Performs no deep parsing. */
    [[nodiscard]] static reader over(std::string_view text) noexcept { return over(text, ambient_memo()); }

    /**
     * The same, with a memo of your own.
     *
     * For a traversal that would otherwise contend with another on the same thread - two
     * documents walked in lockstep, say. Every handle taken from this one shares it, which is
     * what lets a nested loop tell the loop above it how far it got.
     */
    [[nodiscard]] static reader over(std::string_view text, walk_memo &memo) noexcept {
        scanner::cursor scan { text, text.data() };
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return {};
        return reader { text, scan.position, &memo };
    }

    [[nodiscard]] constexpr walk_memo *notes() const noexcept { return this->memo; }

    [[nodiscard]] constexpr std::string_view buffer() const noexcept { return this->source; }
    [[nodiscard]] constexpr const char *data() const noexcept { return this->first; }
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
    [[nodiscard]] bool is_null() const noexcept { return this->type() == kind::null; }
    [[nodiscard]] bool is_boolean() const noexcept { return this->type() == kind::boolean; }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return this->type() == kind::real; }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return this->type() == kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return this->type() == kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return this->type() == kind::object; }

    // ---------------- scalars ----------------

    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        if (this->type() != kind::boolean) return std::nullopt;
        auto scan = this->scan();
        scanner::scan_literal(scan, *this->first == 't' ? "true" : "false");
        if (!scan.ok()) return std::nullopt;
        return *this->first == 't';
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> as_int() const noexcept {
        // As above, with the one thing the grammar scan does not settle: a real is a number and
        // would convert, so the text is checked for the point or exponent that makes it one.
        // That is the same walk number_kind() does, done here on text already in hand.
        const auto text = this->number_text();
        if (text.empty() || text.find_first_of(".eE") != std::string_view::npos) return std::nullopt;

        if (text.front() == '-') {
            std::int64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc {} || !std::in_range<T>(value)) return std::nullopt;
            return static_cast<T>(value);
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc {} || !std::in_range<T>(value)) return std::nullopt;
        return static_cast<T>(value);
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> as_float() const noexcept {
        // One grammar scan and one conversion. is_number() would walk the digits to answer a
        // question the conversion answers anyway, and it answers it by telling an integer from
        // a real - which this does not care about and which costs a second walk to decide.
        const auto text = this->number_text();
        if (text.empty()) return std::nullopt;

        double value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        // out_of_range means the literal overflows a double; JSON has no infinity to mean.
        if (parsed.ec != std::errc {}) return std::nullopt;
        return static_cast<T>(value);
    }

    /** Always decodes. There is no borrow-when-unescaped path, deliberately. */
    [[nodiscard]] std::optional<std::string> as_string() const noexcept {
        const auto text = this->scanned_string();
        if (!text) return std::nullopt;

        std::string decoded;
        decoded.reserve(scanner::decoded_length(*text));
        scanner::decode_string(*text, [&](char value) { decoded.push_back(value); });
        return decoded;
    }

    bool read_string_into(std::string &destination) const {
        const auto text = this->scanned_string();
        if (!text) return false;
        destination.clear();
        destination.reserve(scanner::decoded_length(*text));
        scanner::decode_string(*text, [&](char value) { destination.push_back(value); });
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
    [[nodiscard]] array_range array() const noexcept;
    [[nodiscard]] member_range items() const noexcept;

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

    [[nodiscard]] std::string string() const {
        auto result = this->as_string();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

    template<typename T>
    [[nodiscard]] std::optional<T> try_get() const noexcept {
        if constexpr (std::same_as<T, bool>)
            return this->as_bool();
        else if constexpr (std::same_as<T, std::string_view>) {
            static_assert(always_false<T>,
                    "a JSON string has to be decoded, so it cannot be borrowed as a string_view; "
                    "read it into a std::string, or use decode_string_into");
        } else if constexpr (detail::string_like<T> && std::constructible_from<T, std::string>) {
            auto text = this->as_string();
            if (!text) return std::nullopt;
            return T { *std::move(text) };
        } else if constexpr (std::floating_point<T>)
            return this->as_float<T>();
        else if constexpr (std::integral<T>)
            return this->as_int<T>();
        else if constexpr (detail::structurally_readable<T>) {
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
    [[nodiscard]] constexpr const char *limit() const noexcept { return this->source.data() + this->source.size(); }

    [[nodiscard]] scanner::cursor scan() const noexcept { return scanner::cursor { this->source, this->first }; }

    [[nodiscard]] std::string_view number_text() const noexcept {
        auto scan = this->scan();
        const auto text = scanner::scan_number(scan);
        return scan.ok() ? text : std::string_view {};
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
        return text;
    }

    friend class array_iterator;
    friend class member_iterator;
};

/** A key and its value. The key stays encoded until key_string() or key_is() asks. */
struct key_value {
    scanner::string_span key;
    reader value;

    [[nodiscard]] std::string key_string() const {
        std::string decoded;
        decoded.reserve(scanner::decoded_length(this->key));
        scanner::decode_string(this->key, [&](char value) { decoded.push_back(value); });
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
        const char here = scan.peek();
        if (here == '"') {
            // A bracket inside a string is not a bracket, so a string is consumed whole.
            (void)scan_string(scan);
            continue;
        }
        if (here == '[' || here == '{') {
            ++open;
        } else if (here == ']' || here == '}') {
            --open;
        }
        scan.advance(1);
    }
}

} // namespace scanner

/** Forward iterator over the elements of an array. */
class array_iterator {
public:
    using value_type = reader;
    using reference = reader;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::string_view source;
    const char *cursor = nullptr;
    const char *container_at = nullptr;
    walk_memo *memo = nullptr;
    bool exhausted = true;

public:
    array_iterator() = default;

    explicit array_iterator(const reader &container) noexcept
            : source(container.source)
            , container_at(container.first)
            , memo(container.memo) {
        if (container.type() != kind::array) return;
        scanner::cursor scan { this->source, container.first + 1 };
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == ']') return;
        this->cursor = scan.position;
        this->exhausted = false;
        // This value is what is being walked now, one container deep into it.
        if (this->memo != nullptr) this->memo->note(this->source.data(), this->container_at, scan.position, 1);
    }

    [[nodiscard]] reader operator*() const noexcept {
        if (this->exhausted) return {};
        return reader { this->source, this->cursor, this->memo };
    }

    array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        scanner::cursor scan { this->source, this->cursor };
        int open = 0;
        if (this->memo != nullptr && this->memo->describes(this->source.data(), this->cursor)) {
            // Something walked into this element. Finish what is left of it rather than all.
            scan.position = this->memo->reached;
            open = this->memo->open;
        }
        if (open > 0) {
            scanner::close_containers(scan, open);
        } else if (scan.position == this->cursor) {
            scanner::skip_value(scan, 1);
        }
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            if (this->memo != nullptr && scan.ok() && scan.available(1) && scan.peek() == ']') {
                scan.advance(1);
                this->memo->note(this->source.data(), this->container_at, scan.position, 0);
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
        // This level owns the memo again, between elements of its own container.
        if (this->memo != nullptr) this->memo->note(this->source.data(), this->container_at, scan.position, 1);
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
    using reference = key_value;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::string_view source;
    const char *cursor = nullptr; ///< at the opening quote of the key
    const char *container_at = nullptr;
    walk_memo *memo = nullptr;
    bool exhausted = true;

    /** Where this member's value begins, which is what the memo is kept about. */
    [[nodiscard]] const char *value_position() const noexcept {
        scanner::cursor scan { this->source, this->cursor };
        (void)scanner::scan_string(scan);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') return nullptr;
        scan.advance(1);
        scanner::skip_whitespace(scan);
        return scan.available(1) ? scan.position : nullptr;
    }

public:
    member_iterator() = default;

    explicit member_iterator(const reader &container) noexcept
            : source(container.source)
            , container_at(container.first)
            , memo(container.memo) {
        if (container.type() != kind::object) return;
        scanner::cursor scan { this->source, container.first + 1 };
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == '}') return;
        this->cursor = scan.position;
        this->exhausted = false;
    }

    [[nodiscard]] key_value operator*() const noexcept {
        if (this->exhausted) return {};

        scanner::cursor scan { this->source, this->cursor };
        const auto key = scanner::scan_string(scan);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') return {};
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return {};
        return key_value { key, reader { this->source, scan.position, this->memo } };
    }

    member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        scanner::cursor scan { this->source, this->cursor };
        (void)scanner::scan_string(scan);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') {
            this->exhausted = true;
            return *this;
        }
        scan.advance(1);
        scanner::skip_whitespace(scan);

        // The value may have been walked by a loop of its own; finish what is left of it.
        const char *const value_at = scan.available(1) ? scan.position : nullptr;
        int open = 0;
        if (this->memo != nullptr && value_at != nullptr && this->memo->describes(this->source.data(), value_at)) {
            scan.position = this->memo->reached;
            open = this->memo->open;
        }
        if (open > 0) {
            scanner::close_containers(scan, open);
        } else if (scan.position == value_at) {
            // Nothing had been walked, or it was walked to the end: only skip if we are still
            // standing at the start of it.
            scanner::skip_value(scan, 1);
        }
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            if (this->memo != nullptr && scan.ok() && scan.available(1) && scan.peek() == '}') {
                scan.advance(1);
                this->memo->note(this->source.data(), this->container_at, scan.position, 0);
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

inline array_range reader::array() const noexcept { return array_range { array_iterator { *this } }; }

inline member_range reader::items() const noexcept { return member_range { member_iterator { *this } }; }

inline std::size_t reader::size() const noexcept {
    std::size_t total = 0;
    if (this->is_object()) {
        for ([[maybe_unused]] const auto entry : this->items())
            ++total;
    } else if (this->is_array()) {
        for ([[maybe_unused]] const auto entry : this->array())
            ++total;
    }
    return total;
}

inline reader reader::operator[](std::size_t index) const noexcept {
    std::size_t position = 0;
    for (const auto element : this->array()) {
        if (position++ == index) return element;
    }
    return {};
}

inline reader reader::operator[](std::string_view key) const noexcept {
    for (const auto entry : this->items()) {
        if (entry.key_is(key)) return entry.value;
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
