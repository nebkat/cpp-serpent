#pragma once

// A JSON reader shaped like bjdata::view, but a reader rather than a view: BJData values
// *are* the bytes, so a view can borrow them; JSON values must be constructed, so strings
// are always decoded rather than handed back as a borrow that only works when the data
// happens to contain no escapes.
//
// Everything else carries over: the same errc, the same three accessor tiers, the same
// forward iterators holding all the traversal state, and no allocation anywhere except
// where a decoded string is asked for.

#include <nonstd/bjdata/error.hpp>
#include <nonstd/bjdata/fwd.hpp>
#include <nonstd/bjdata/json_scan.hpp>
#include <nonstd/bjdata/serializer.hpp>
#include <nonstd/bjdata/view.hpp>

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

namespace nonstd::bjdata {

class json_reader;
class json_array_iterator;
class json_array_range;
class json_member_iterator;
class json_member_range;

/** @brief A handle to one JSON value inside a text buffer. */
class json_reader {
    std::string_view source;
    const char *first = nullptr;   ///< this value's first character, whitespace already skipped

public:
    constexpr json_reader() = default;
    constexpr json_reader(std::string_view source, const char *first) noexcept: source(source), first(first) {}

    /** Wraps a document, skipping leading whitespace. Performs no deep parsing. */
    [[nodiscard]] static json_reader over(std::string_view text) noexcept {
        json_detail::cursor scan { text, text.data() };
        json_detail::skip_whitespace(scan);
        if (!scan.available(1)) return {};
        return json_reader { text, scan.position };
    }

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
        if (*this->first == '-' || json_detail::is_digit(*this->first)) return this->number_kind();
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
        json_detail::scan_literal(scan, *this->first == 't' ? "true" : "false");
        if (!scan.ok()) return std::nullopt;
        return *this->first == 't';
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> as_int() const noexcept {
        if (this->type() != kind::integer) return std::nullopt;
        const auto text = this->number_text();
        if (text.empty()) return std::nullopt;

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
        if (!this->is_number()) return std::nullopt;
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
        decoded.reserve(json_detail::decoded_length(*text));
        json_detail::decode_string(*text, [&](char value) { decoded.push_back(value); });
        return decoded;
    }

    bool read_string_into(std::string &destination) const {
        const auto text = this->scanned_string();
        if (!text) return false;
        destination.clear();
        destination.reserve(json_detail::decoded_length(*text));
        json_detail::decode_string(*text, [&](char value) { destination.push_back(value); });
        return true;
    }

    /** Decodes into caller storage, for the paths that may not allocate. */
    [[nodiscard]] std::optional<std::size_t> decode_string_into(std::span<char> destination) const noexcept {
        const auto text = this->scanned_string();
        if (!text) return std::nullopt;
        if (json_detail::decoded_length(*text) > destination.size()) return std::nullopt;

        std::size_t written = 0;
        json_detail::decode_string(*text, [&](char value) { destination[written++] = value; });
        return written;
    }

    /** Compares against a string without materialising this one. */
    [[nodiscard]] bool string_is(std::string_view other) const noexcept {
        const auto text = this->scanned_string();
        return text && json_detail::equals(*text, other);
    }

    // ---------------- containers ----------------

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] json_array_range array() const noexcept;
    [[nodiscard]] json_member_range items() const noexcept;

    [[nodiscard]] json_reader operator[](std::size_t index) const noexcept;
    [[nodiscard]] json_reader operator[](std::string_view key) const noexcept;
    [[nodiscard]] json_reader find(std::string_view key) const noexcept { return (*this)[key]; }

    /** The text this value occupies, or nothing if it does not parse. */
    [[nodiscard]] std::optional<std::string_view> extent() const noexcept {
        if (this->first == nullptr) return std::nullopt;
        auto scan = this->scan();
        json_detail::skip_value(scan, 0);
        if (!scan.ok()) return std::nullopt;
        return std::string_view { this->first, static_cast<std::size_t>(scan.position - this->first) };
    }

    // ---------------- checked ----------------

    [[nodiscard]] json_reader at(std::size_t index) const {
        auto result = (*this)[index];
        if (!result.is_valid()) raise(errc::out_of_range, this->offset());
        return result;
    }

    [[nodiscard]] json_reader at(std::string_view key) const {
        auto result = (*this)[key];
        if (!result.is_valid()) raise(errc::out_of_range, this->offset());
        return result;
    }

    [[nodiscard]] std::string string() const {
        auto result = this->as_string();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

    template<typename T>
    [[nodiscard]] std::optional<T> try_get() const noexcept {
        if constexpr (std::same_as<T, bool>) return this->as_bool();
        else if constexpr (std::constructible_from<T, std::string> && !std::is_arithmetic_v<T>) {
            auto text = this->as_string();
            if (!text) return std::nullopt;
            return T { *std::move(text) };
        }
        else if constexpr (std::floating_point<T>) return this->as_float<T>();
        else if constexpr (std::integral<T>) return this->as_int<T>();
        else {
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

    [[nodiscard]] json_detail::cursor scan() const noexcept {
        return json_detail::cursor { this->source, this->first };
    }

    [[nodiscard]] std::string_view number_text() const noexcept {
        auto scan = this->scan();
        const auto text = json_detail::scan_number(scan);
        return scan.ok() ? text : std::string_view {};
    }

    /** A JSON number is an integer unless it carries a fraction or an exponent. */
    [[nodiscard]] kind number_kind() const noexcept {
        const auto text = this->number_text();
        if (text.empty()) return kind::invalid;
        return text.find_first_of(".eE") == std::string_view::npos ? kind::integer : kind::real;
    }

    [[nodiscard]] std::optional<json_detail::string_span> scanned_string() const noexcept {
        if (this->type() != kind::string) return std::nullopt;
        auto scan = this->scan();
        const auto text = json_detail::scan_string(scan);
        if (!scan.ok()) return std::nullopt;
        return text;
    }

    friend class json_array_iterator;
    friend class json_member_iterator;
};

/** A key and its value. The key stays encoded until key_string() or key_is() asks. */
struct json_key_value {
    json_detail::string_span key;
    json_reader value;

    [[nodiscard]] std::string key_string() const {
        std::string decoded;
        decoded.reserve(json_detail::decoded_length(this->key));
        json_detail::decode_string(this->key, [&](char value) { decoded.push_back(value); });
        return decoded;
    }

    [[nodiscard]] bool key_is(std::string_view other) const noexcept {
        return json_detail::equals(this->key, other);
    }
};

/** Forward iterator over the elements of an array. */
class json_array_iterator {
public:
    using value_type        = json_reader;
    using reference         = json_reader;
    using difference_type   = std::ptrdiff_t;
    using iterator_concept  = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::string_view source;
    const char *cursor = nullptr;
    bool exhausted = true;

public:
    json_array_iterator() = default;

    explicit json_array_iterator(const json_reader &container) noexcept: source(container.source) {
        if (container.type() != kind::array) return;
        json_detail::cursor scan { this->source, container.first + 1 };
        json_detail::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == ']') return;
        this->cursor = scan.position;
        this->exhausted = false;
    }

    [[nodiscard]] json_reader operator*() const noexcept {
        if (this->exhausted) return {};
        return json_reader { this->source, this->cursor };
    }

    json_array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        json_detail::cursor scan { this->source, this->cursor };
        json_detail::skip_value(scan, 1);
        json_detail::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            return *this;
        }
        scan.advance(1);
        json_detail::skip_whitespace(scan);
        if (!scan.available(1)) {
            this->exhausted = true;
            return *this;
        }
        this->cursor = scan.position;
        return *this;
    }

    json_array_iterator operator++(int) noexcept {
        auto previous = *this;
        ++(*this);
        return previous;
    }

    [[nodiscard]] friend bool operator==(const json_array_iterator &left, const json_array_iterator &right) noexcept {
        if (left.exhausted || right.exhausted) return left.exhausted == right.exhausted;
        return left.cursor == right.cursor;
    }
};

/** Forward iterator over the key/value pairs of an object. */
class json_member_iterator {
public:
    using value_type        = json_key_value;
    using reference         = json_key_value;
    using difference_type   = std::ptrdiff_t;
    using iterator_concept  = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

private:
    std::string_view source;
    const char *cursor = nullptr;   ///< at the opening quote of the key
    bool exhausted = true;

public:
    json_member_iterator() = default;

    explicit json_member_iterator(const json_reader &container) noexcept: source(container.source) {
        if (container.type() != kind::object) return;
        json_detail::cursor scan { this->source, container.first + 1 };
        json_detail::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() == '}') return;
        this->cursor = scan.position;
        this->exhausted = false;
    }

    [[nodiscard]] json_key_value operator*() const noexcept {
        if (this->exhausted) return {};

        json_detail::cursor scan { this->source, this->cursor };
        const auto key = json_detail::scan_string(scan);
        json_detail::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') return {};
        scan.advance(1);
        json_detail::skip_whitespace(scan);
        if (!scan.available(1)) return {};
        return json_key_value { key, json_reader { this->source, scan.position } };
    }

    json_member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        json_detail::cursor scan { this->source, this->cursor };
        (void) json_detail::scan_string(scan);
        json_detail::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ':') {
            this->exhausted = true;
            return *this;
        }
        scan.advance(1);
        json_detail::skip_value(scan, 1);
        json_detail::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->exhausted = true;
            return *this;
        }
        scan.advance(1);
        json_detail::skip_whitespace(scan);
        if (!scan.available(1)) {
            this->exhausted = true;
            return *this;
        }
        this->cursor = scan.position;
        return *this;
    }

    json_member_iterator operator++(int) noexcept {
        auto previous = *this;
        ++(*this);
        return previous;
    }

    [[nodiscard]] friend bool operator==(const json_member_iterator &left, const json_member_iterator &right) noexcept {
        if (left.exhausted || right.exhausted) return left.exhausted == right.exhausted;
        return left.cursor == right.cursor;
    }
};

class json_array_range : public std::ranges::view_interface<json_array_range> {
    json_array_iterator head;

public:
    json_array_range() = default;
    explicit json_array_range(json_array_iterator head) noexcept: head(head) {}
    [[nodiscard]] json_array_iterator begin() const noexcept { return this->head; }
    [[nodiscard]] json_array_iterator end() const noexcept { return {}; }
};

class json_member_range : public std::ranges::view_interface<json_member_range> {
    json_member_iterator head;

public:
    json_member_range() = default;
    explicit json_member_range(json_member_iterator head) noexcept: head(head) {}
    [[nodiscard]] json_member_iterator begin() const noexcept { return this->head; }
    [[nodiscard]] json_member_iterator end() const noexcept { return {}; }
};

inline json_array_range json_reader::array() const noexcept {
    return json_array_range { json_array_iterator { *this } };
}

inline json_member_range json_reader::items() const noexcept {
    return json_member_range { json_member_iterator { *this } };
}

inline std::size_t json_reader::size() const noexcept {
    std::size_t total = 0;
    if (this->is_object()) {
        for ([[maybe_unused]] const auto entry : this->items()) ++total;
    } else if (this->is_array()) {
        for ([[maybe_unused]] const auto entry : this->array()) ++total;
    }
    return total;
}

inline json_reader json_reader::operator[](std::size_t index) const noexcept {
    std::size_t position = 0;
    for (const auto element : this->array()) {
        if (position++ == index) return element;
    }
    return {};
}

inline json_reader json_reader::operator[](std::string_view key) const noexcept {
    for (const auto entry : this->items()) {
        if (entry.key_is(key)) return entry.value;
    }
    return {};
}

/** Walks the whole document once, checking it is well formed and consumes the whole text. */
[[nodiscard]] inline std::expected<void, error> validate_json(std::string_view text) noexcept {
    json_detail::cursor scan { text, text.data() };
    json_detail::skip_whitespace(scan);
    if (!scan.available(1)) return std::unexpected { error { errc::unexpected_end, 0 } };

    json_detail::skip_value(scan, 0);
    if (!scan.ok()) return std::unexpected { scan.to_error() };

    json_detail::skip_whitespace(scan);
    if (scan.position != scan.limit) {
        return std::unexpected { error { errc::trailing_data,
                                         static_cast<std::size_t>(scan.position - text.data()) } };
    }
    return {};
}

/**
 * Decodes a value from JSON text.
 *
 * A type carrying a bjdata_convert - which BJDATA_DEFINE_TYPE writes for you - reads here and
 * from BJData with one definition, because the visitor never names either reader. A type
 * using from_bjdata names `view`, so it reads BJData only.
 */
template<typename T>
[[nodiscard]] std::optional<T> from_json(std::string_view text) {
    return json_reader::over(text).try_get<T>();
}

}// namespace nonstd::bjdata
