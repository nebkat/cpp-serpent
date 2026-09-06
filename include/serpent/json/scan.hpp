#pragma once

// The JSON text scanner: the primitives everything else in the reader is built on.
//
// Unlike BJData, nothing here is length-prefixed, so skipping a value means reading it. The
// escape-aware string scan is the load-bearing part: mistake a \" for a closing quote and
// the whole walk desynchronises, which is why it is separated out and tested on its own.

#include <serpent/error.hpp>
#include <serpent/limits.hpp>

#include <charconv>
#include <span>
#include <string_view>

#include <cstddef>
#include <cstdint>

namespace serpent::json::scanner {


[[nodiscard]] constexpr bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] constexpr bool is_digit(char value) noexcept { return value >= '0' && value <= '9'; }

[[nodiscard]] constexpr int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/** A bounds-checked cursor over the text, latching the first failure. Mirrors detail::cursor. */
struct cursor {
    const char *origin = nullptr;
    const char *position = nullptr;
    const char *limit = nullptr;

    errc failure = errc::ok;
    std::size_t failure_offset = 0;

    constexpr cursor() = default;
    constexpr cursor(std::string_view text, const char *position) noexcept
        : origin(text.data()), position(position), limit(text.data() + text.size()) {}

    [[nodiscard]] constexpr bool ok() const noexcept { return this->failure == errc::ok; }
    [[nodiscard]] constexpr bool available(std::size_t count) const noexcept {
        return this->ok() && this->position != nullptr
                && static_cast<std::size_t>(this->limit - this->position) >= count;
    }

    constexpr void fail(errc code) noexcept { this->fail(code, this->position); }
    constexpr void fail(errc code, const char *at) noexcept {
        if (!this->ok()) return;
        this->failure = code;
        this->failure_offset = static_cast<std::size_t>(at - this->origin);
    }

    [[nodiscard]] constexpr bool need(std::size_t count) noexcept {
        if (!this->ok()) return false;
        if (!this->available(count)) {
            this->fail(errc::unexpected_end);
            return false;
        }
        return true;
    }

    [[nodiscard]] constexpr char peek() const noexcept { return *this->position; }
    constexpr char take() noexcept { return *this->position++; }
    constexpr void advance(std::size_t count) noexcept { this->position += count; }

    [[nodiscard]] constexpr error to_error() const noexcept { return error { this->failure, this->failure_offset }; }
};

constexpr void skip_whitespace(cursor &scan) noexcept {
    while (scan.available(1) && is_space(scan.peek())) scan.advance(1);
}

/** A scanned string: its contents between the quotes, still escaped. */
struct string_span {
    std::string_view contents;
    bool escaped = false;
};

/**
 * Scans a string, leaving the cursor just past the closing quote.
 *
 * Rejects the things JSON forbids rather than tolerating them: a raw control character, an
 * unknown escape, a short \\u, and a surrogate that is not properly paired.
 */
[[nodiscard]] constexpr string_span scan_string(cursor &scan) noexcept {
    string_span result;
    if (!scan.need(1)) return result;
    if (scan.peek() != '"') {
        scan.fail(errc::unexpected_character);
        return result;
    }
    scan.advance(1);

    const char *const begin = scan.position;
    while (true) {
        if (!scan.need(1)) return result;
        const char value = scan.take();

        if (value == '"') {
            result.contents = std::string_view { begin, static_cast<std::size_t>(scan.position - 1 - begin) };
            return result;
        }
        if (value == '\\') {
            result.escaped = true;
            const char *const escape_at = scan.position - 1;
            if (!scan.need(1)) return result;
            const char kind = scan.take();
            switch (kind) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n': case 'r': case 't':
                    break;
                case 'u': {
                    if (!scan.available(4)) {
                        scan.fail(errc::invalid_escape, escape_at);
                        return result;
                    }
                    std::uint32_t code = 0;
                    for (int index = 0; index < 4; ++index) {
                        const int digit = hex_value(scan.take());
                        if (digit < 0) {
                            scan.fail(errc::invalid_escape, escape_at);
                            return result;
                        }
                        code = (code << 4) | static_cast<std::uint32_t>(digit);
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // A high surrogate must be followed by its low half. Checked without
                        // need(), so running out here is reported as the bad escape it is
                        // rather than as a truncated document.
                        if (!scan.available(6) || scan.position[0] != '\\' || scan.position[1] != 'u') {
                            scan.fail(errc::invalid_escape, escape_at);
                            return result;
                        }
                        std::uint32_t low = 0;
                        for (int index = 0; index < 4; ++index) {
                            const int digit = hex_value(scan.position[2 + index]);
                            if (digit < 0) {
                                scan.fail(errc::invalid_escape, escape_at);
                                return result;
                            }
                            low = (low << 4) | static_cast<std::uint32_t>(digit);
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            scan.fail(errc::invalid_escape, escape_at);
                            return result;
                        }
                        scan.advance(6);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        scan.fail(errc::invalid_escape, escape_at);   // a lone low surrogate
                        return result;
                    }
                    break;
                }
                default:
                    scan.fail(errc::invalid_escape, escape_at);
                    return result;
            }
            continue;
        }
        if (static_cast<unsigned char>(value) < 0x20) {
            scan.fail(errc::invalid_string, scan.position - 1);
            return result;
        }
    }
}

/**
 * Scans a number, leaving the cursor just past it.
 *
 * The grammar is checked here rather than left to from_chars, which is more permissive than
 * JSON: it would accept inf, nan, .5, 5. and read 01 as 1.
 */
[[nodiscard]] constexpr std::string_view scan_number(cursor &scan) noexcept {
    const char *const begin = scan.position;
    const auto reject = [&] {
        scan.fail(errc::invalid_number, begin);
        return std::string_view {};
    };

    if (!scan.need(1)) return {};
    if (scan.peek() == '-') scan.advance(1);

    if (!scan.available(1) || !is_digit(scan.peek())) return reject();
    if (scan.peek() == '0') {
        scan.advance(1);
        if (scan.available(1) && is_digit(scan.peek())) return reject();   // no leading zeros
    } else {
        while (scan.available(1) && is_digit(scan.peek())) scan.advance(1);
    }

    if (scan.available(1) && scan.peek() == '.') {
        scan.advance(1);
        if (!scan.available(1) || !is_digit(scan.peek())) return reject();
        while (scan.available(1) && is_digit(scan.peek())) scan.advance(1);
    }

    if (scan.available(1) && (scan.peek() == 'e' || scan.peek() == 'E')) {
        scan.advance(1);
        if (scan.available(1) && (scan.peek() == '+' || scan.peek() == '-')) scan.advance(1);
        if (!scan.available(1) || !is_digit(scan.peek())) return reject();
        while (scan.available(1) && is_digit(scan.peek())) scan.advance(1);
    }

    return std::string_view { begin, static_cast<std::size_t>(scan.position - begin) };
}

constexpr void scan_literal(cursor &scan, std::string_view word) noexcept {
    if (!scan.ok()) return;

    // Compare what is there before complaining about what is not: "nan" should be reported
    // as an unexpected character, not as a document that ended early.
    const auto have = static_cast<std::size_t>(scan.limit - scan.position);
    const auto comparable = have < word.size() ? have : word.size();
    if (std::string_view { scan.position, comparable } != word.substr(0, comparable)) {
        scan.fail(errc::unexpected_character);
        return;
    }
    if (have < word.size()) {
        scan.fail(errc::unexpected_end);
        return;
    }
    scan.advance(word.size());
}

/** Appends one Unicode code point as UTF-8. */
template<typename Append>
constexpr void append_utf8(std::uint32_t code, Append &append) noexcept {
    if (code < 0x80) {
        append(static_cast<char>(code));
    } else if (code < 0x800) {
        append(static_cast<char>(0xC0 | (code >> 6)));
        append(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        append(static_cast<char>(0xE0 | (code >> 12)));
        append(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        append(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        append(static_cast<char>(0xF0 | (code >> 18)));
        append(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        append(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        append(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

/**
 * Decodes a scanned string, handing each character to an appender.
 *
 * Assumes the span came from scan_string, which has already rejected everything malformed,
 * so this does no validation of its own. The single definition of what an escape means:
 * unescaping, measuring and comparing all run through here.
 */
template<typename Append>
constexpr void decode_string(string_span text, Append append) noexcept {
    if (!text.escaped) {
        for (const char value : text.contents) append(value);
        return;
    }

    const auto read_hex = [&](std::size_t at) {
        std::uint32_t code = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            code = (code << 4) | static_cast<std::uint32_t>(hex_value(text.contents[at + index]));
        }
        return code;
    };

    for (std::size_t index = 0; index < text.contents.size(); ++index) {
        const char value = text.contents[index];
        if (value != '\\') {
            append(value);
            continue;
        }

        const char kind = text.contents[++index];
        switch (kind) {
            case '"':  append('"'); break;
            case '\\': append('\\'); break;
            case '/':  append('/'); break;
            case 'b':  append('\b'); break;
            case 'f':  append('\f'); break;
            case 'n':  append('\n'); break;
            case 'r':  append('\r'); break;
            case 't':  append('\t'); break;
            case 'u': {
                std::uint32_t code = read_hex(index + 1);
                index += 4;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    const std::uint32_t low = read_hex(index + 3);
                    index += 6;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(code, append);
                break;
            }
            default: break;
        }
    }
}

/** How many bytes the decoded string occupies. */
[[nodiscard]] constexpr std::size_t decoded_length(string_span text) noexcept {
    if (!text.escaped) return text.contents.size();
    std::size_t length = 0;
    decode_string(text, [&](char) { ++length; });
    return length;
}

/** Compares a scanned string against a plain one without materialising it. */
[[nodiscard]] constexpr bool equals(string_span text, std::string_view plain) noexcept {
    if (!text.escaped) return text.contents == plain;

    std::size_t index = 0;
    bool matched = true;
    decode_string(text, [&](char value) {
        if (index >= plain.size() || plain[index] != value) matched = false;
        ++index;
    });
    return matched && index == plain.size();
}

void skip_value(cursor &scan, int depth) noexcept;

inline void skip_container(cursor &scan, bool object, int depth) noexcept {
    const char opening = object ? '{' : '[';
    const char closing = object ? '}' : ']';

    if (!scan.need(1) || scan.peek() != opening) {
        scan.fail(errc::unexpected_character);
        return;
    }
    scan.advance(1);

    skip_whitespace(scan);
    if (!scan.available(1)) {
        scan.fail(errc::unterminated_container);
        return;
    }
    if (scan.peek() == closing) {
        scan.advance(1);
        return;
    }

    while (scan.ok()) {
        skip_whitespace(scan);
        if (object) {
            (void) scan_string(scan);
            if (!scan.ok()) return;
            skip_whitespace(scan);
            if (!scan.need(1)) return;
            if (scan.take() != ':') {
                scan.fail(errc::unexpected_character, scan.position - 1);
                return;
            }
        }

        skip_value(scan, depth + 1);
        if (!scan.ok()) return;

        skip_whitespace(scan);
        if (!scan.available(1)) {
            scan.fail(errc::unterminated_container);
            return;
        }
        const char separator = scan.take();
        if (separator == closing) return;
        if (separator != ',') {
            scan.fail(errc::unexpected_character, scan.position - 1);
            return;
        }
    }
}

inline void skip_value(cursor &scan, int depth) noexcept {
    if (depth > max_depth) {
        scan.fail(errc::depth_exceeded);
        return;
    }
    skip_whitespace(scan);
    if (!scan.need(1)) return;

    switch (scan.peek()) {
        case '{': skip_container(scan, true, depth); return;
        case '[': skip_container(scan, false, depth); return;
        case '"': (void) scan_string(scan); return;
        case 't': scan_literal(scan, "true"); return;
        case 'f': scan_literal(scan, "false"); return;
        case 'n': scan_literal(scan, "null"); return;
        default:
            if (scan.peek() == '-' || is_digit(scan.peek())) {
                (void) scan_number(scan);
                return;
            }
            scan.fail(errc::unexpected_character);
            return;
    }
}

}// namespace serpent::json::scanner
