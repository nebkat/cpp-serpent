#pragma once

// The JSON text scanner: the primitives everything else in the reader is built on.
//
// Unlike BJData, nothing here is length-prefixed, so skipping a value means reading it. The
// escape-aware string scan is the load-bearing part: mistake a \" for a closing quote and
// the whole walk desynchronises, which is why it is separated out and tested on its own.

#include <serpent/config.hpp>
#include <serpent/error.hpp>
#include <serpent/limits.hpp>

#include <charconv>
#include <span>
#include <array>
#include <bit>
#include <string>
#include <string_view>

#include <cstddef>
#include <initializer_list>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <tuple>

namespace serpent::json::scanner {

[[nodiscard]] constexpr bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] constexpr bool is_digit(char value) noexcept { return value >= '0' && value <= '9'; }

// ---------------- scanning runs of one kind of character ----------------

inline constexpr std::uint8_t class_space = 1 << 0;       ///< space, tab, newline, carriage return
inline constexpr std::uint8_t class_digit = 1 << 1;       ///< 0-9
inline constexpr std::uint8_t class_string_body = 1 << 2; ///< anything a string may hold as itself

/**
 * What each byte is, as bits, so a test is a load and an and rather than a run of comparisons.
 *
 * 256 bytes of constants, which is the whole cost.
 */
inline constexpr auto character_class = [] {
    std::array<std::uint8_t, 256> table {};
    for (std::size_t value = 0; value < table.size(); ++value) {
        std::uint8_t flags = 0;
        if (value == ' ' || value == '\t' || value == '\n' || value == '\r') flags |= class_space;
        if (value >= '0' && value <= '9') flags |= class_digit;
        // A string holds anything but its own quote, a backslash, and the control characters
        // JSON insists are escaped. Everything from 0x80 up is UTF-8 and passes through.
        if (value != '"' && value != '\\' && value >= 0x20) flags |= class_string_body;
        table[value] = flags;
    }
    return table;
}();

/** Whether text can stand between quotes exactly as it is, with nothing in it to escape. */
[[nodiscard]] constexpr bool is_plain_text(std::string_view text) noexcept {
    for (const char value : text)
        if (value == '"' || value == '\\' || static_cast<unsigned char>(value) < 0x20) return false;
    return true;
}

/**
 * The first byte from `position` that is not of the wanted class.
 *
 * One compare for the end and one table lookup for the byte, and nothing else: what this
 * replaces asked the cursor whether it had failed and whether it was null on every character,
 * to answer a question that cannot change during a run.
 *
 * Deliberately not widened to read several bytes at once. The runs here are short - the
 * whitespace between two tokens is usually one byte and a number is eight or so - and scanning
 * sixteen to find a run of one costs far more than it saves. Measured: a sixteen-wide version
 * of this made a number-heavy document four times slower.
 */
template<bool Terminated = false>
[[nodiscard]] constexpr const char *advance_while(
        const char *position, const char *limit, std::uint8_t wanted) noexcept {
    if constexpr (Terminated) {
        // The terminator is in no class, so it ends the run by itself: one test per byte where
        // the bounded loop makes two. `limit` still marks where the text ends.
        while ((character_class[static_cast<unsigned char>(*position)] & wanted) != 0) ++position;
    } else {
        while (position < limit && (character_class[static_cast<unsigned char>(*position)] & wanted) != 0) ++position;
    }
    return position;
}

/**
 * Whether any of the eight bytes in `word` is one a string cannot hold as itself: a quote, a
 * backslash, or a control character.
 *
 * Each test is the usual one for "is any byte of this word zero", or "less than n": subtracting
 * from every byte at once borrows out of exactly the bytes that were too small, and the borrow
 * shows in the top bit of a byte whose own top bit was clear. It can spill into the byte above a
 * byte that matched, so this says whether there is such a byte, not which - and which is found
 * by looking at the eight one at a time, which happens once per escape rather than once per byte.
 * A byte of 0x80 or more, which is part of a character outside ASCII, never matches.
 */
[[nodiscard]] constexpr bool holds_byte_to_escape(std::uint64_t word) noexcept {
    constexpr std::uint64_t every_byte = 0x0101010101010101;
    constexpr std::uint64_t top_bits = 0x8080808080808080;

    const auto any_byte_below = [](std::uint64_t bytes, std::uint64_t bound) {
        return (bytes - every_byte * bound) & ~bytes & top_bits;
    };
    const auto any_byte_equal_to = [&](std::uint64_t bytes, char wanted) {
        return any_byte_below(bytes ^ (every_byte * static_cast<unsigned char>(wanted)), 1);
    };
    return (any_byte_below(word, 0x20) | any_byte_equal_to(word, '"') | any_byte_equal_to(word, '\\')) != 0;
}

/**
 * The first byte from `position` that a string cannot hold as itself, or `limit`.
 *
 * The same answer advance_while gives for class_string_body. Where that looks at every byte,
 * this steps over eight at a time for as long as none of the eight needs a second look - which
 * suits a string, whose runs are long, and would not suit the runs advance_while is otherwise
 * asked about. SERPENT_WIDE_STRING_SCAN=0 makes it advance_while and nothing more.
 */
template<bool Terminated = false>
[[nodiscard]] SERPENT_ALWAYS_INLINE constexpr const char *end_of_plain_text(const char *position, const char *limit) noexcept {
#if SERPENT_WIDE_STRING_SCAN
    if (!std::is_constant_evaluated()) {
        while (limit - position >= 8) {
            std::uint64_t word;
            std::memcpy(&word, position, sizeof word);
            if (holds_byte_to_escape(word)) break;
            position += 8;
        }
    }
#endif
    return advance_while<Terminated>(position, limit, class_string_body);
}

[[nodiscard]] constexpr int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/**
 * A cursor over the text, latching the first failure. Mirrors detail::cursor.
 *
 * Bounds-checked against `limit`, which is where the text ends in either mode. A terminated
 * cursor is one whose caller has promised that the byte at `limit` exists and is zero - a
 * std::string keeps one there - and that promise lets every run of bytes be scanned with one
 * test per byte rather than two, since the terminator is in no character class. A zero inside
 * the text ends a run early in that mode; it can only be a control character, which nothing in
 * JSON may hold unescaped, so what follows finds the fault where a bounded scan would have.
 */
template<bool Terminated>
struct basic_cursor {
    static constexpr bool terminated = Terminated;

    const char *origin = nullptr;
    const char *position = nullptr;
    const char *limit = nullptr;

    errc failure = errc::ok;
    std::size_t failure_offset = 0;

    constexpr basic_cursor() = default;
    constexpr basic_cursor(std::string_view text, const char *position) noexcept
    : origin(text.data())
    , position(position)
    , limit(text.data() + text.size()) {}

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

using cursor = basic_cursor<false>;
using terminated_cursor = basic_cursor<true>;

template<bool Terminated>
constexpr void skip_whitespace(basic_cursor<Terminated> &scan) noexcept {
    if (!scan.ok() || scan.position == nullptr) return;
    scan.position = advance_while<Terminated>(scan.position, scan.limit, class_space);
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
template<bool Terminated>
[[nodiscard]] constexpr string_span scan_string(basic_cursor<Terminated> &scan) noexcept {
    string_span result;
    if (!scan.need(1)) return result;
    if (scan.peek() != '"') {
        scan.fail(errc::unexpected_character);
        return result;
    }
    scan.advance(1);

    const char *const begin = scan.position;
    while (true) {
        // Everything a string holds as itself goes by without being looked at twice; what stops
        // the run is the quote, a backslash, or a control character, and those are rare.
        scan.position = end_of_plain_text<Terminated>(scan.position, scan.limit);

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
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't': break;
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
                    scan.fail(errc::invalid_escape, escape_at); // a lone low surrogate
                    return result;
                }
                break;
            }
            default: scan.fail(errc::invalid_escape, escape_at); return result;
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
template<bool Terminated>
[[nodiscard]] constexpr std::string_view scan_number(basic_cursor<Terminated> &scan) noexcept {
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
        if (scan.available(1) && is_digit(scan.peek())) return reject(); // no leading zeros
    } else {
        scan.position = advance_while<Terminated>(scan.position, scan.limit, class_digit);
    }

    if (scan.available(1) && scan.peek() == '.') {
        scan.advance(1);
        if (!scan.available(1) || !is_digit(scan.peek())) return reject();
        scan.position = advance_while<Terminated>(scan.position, scan.limit, class_digit);
    }

    if (scan.available(1) && (scan.peek() == 'e' || scan.peek() == 'E')) {
        scan.advance(1);
        if (scan.available(1) && (scan.peek() == '+' || scan.peek() == '-')) scan.advance(1);
        if (!scan.available(1) || !is_digit(scan.peek())) return reject();
        while (scan.available(1) && is_digit(scan.peek()))
            scan.advance(1);
    }

    return std::string_view { begin, static_cast<std::size_t>(scan.position - begin) };
}

template<bool Terminated>
constexpr void scan_literal(basic_cursor<Terminated> &scan, std::string_view word) noexcept {
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

/**
 * Steps over `text` if that is exactly what stands at the cursor, and says whether it did.
 *
 * The answering sibling of scan_literal: not finding the text is an answer rather than a failure,
 * and the cursor is left where it was. For text known when the program is compiled - a literal, a
 * key and its punctuation.
 *
 * The width is part of the type, and the comparison is char_traits' rather than string_view's,
 * both on purpose. Comparing two string_views goes through basic_string_view::compare, which is
 * of fixed width only where the compiler happens to inline it - and whether it does depends on
 * what else the translation unit holds: the same call measured three times slower in a larger
 * program, as thirteen calls to compare() for every object read. This way the width is a
 * constant wherever the code ends up.
 */
template<bool Terminated, std::size_t Width>
[[nodiscard]] constexpr bool accept(basic_cursor<Terminated> &scan, const std::array<char, Width> &text) noexcept {
    if (!scan.available(Width)) return false;
    if (std::char_traits<char>::compare(scan.position, text.data(), Width) != 0) return false;
    scan.advance(Width);
    return true;
}

/** The same for one character, which is what most punctuation is. */
template<bool Terminated>
[[nodiscard]] constexpr bool accept(basic_cursor<Terminated> &scan, char character) noexcept {
    if (!scan.available(1) || scan.peek() != character) return false;
    scan.advance(1);
    return true;
}

/** The same for a literal, whose terminator is not part of what is looked for. */
template<bool Terminated, std::size_t Size>
[[nodiscard]] constexpr bool accept(basic_cursor<Terminated> &scan, const char (&literal)[Size]) noexcept {
    std::array<char, Size - 1> text {};
    for (std::size_t index = 0; index < text.size(); ++index) text[index] = literal[index];
    return accept(scan, text);
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
constexpr void decode_string(string_span text, Append &&append) noexcept {
    // The text between escapes is handed over as one run where the receiver takes runs, and a
    // character at a time where it takes only those.
    const auto run = [&](std::size_t from, std::size_t to) {
        if constexpr (requires { append(std::string_view {}); }) {
            append(text.contents.substr(from, to - from));
        } else {
            for (std::size_t index = from; index < to; ++index) append(text.contents[index]);
        }
    };
    if (!text.escaped) {
        run(0, text.contents.size());
        return;
    }

    const auto read_hex = [&](std::size_t at) {
        std::uint32_t code = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            code = (code << 4) | static_cast<std::uint32_t>(hex_value(text.contents[at + index]));
        }
        return code;
    };

    // Between escapes the text holds nothing a string cannot hold as itself - scan_string has
    // seen to that - so the first byte end_of_plain_text stops on is the next backslash.
    const char *const first = text.contents.data();
    const char *const limit = first + text.contents.size();
    std::size_t index = 0;
    while (index < text.contents.size()) {
        const std::size_t escape = static_cast<std::size_t>(end_of_plain_text(first + index, limit) - first);
        if (escape == text.contents.size()) {
            run(index, escape);
            return;
        }
        run(index, escape);
        index = escape + 1;

        const char kind = text.contents[index++];
        switch (kind) {
        case '"': append('"'); break;
        case '\\': append('\\'); break;
        case '/': append('/'); break;
        case 'b': append('\b'); break;
        case 'f': append('\f'); break;
        case 'n': append('\n'); break;
        case 'r': append('\r'); break;
        case 't': append('\t'); break;
        case 'u': {
            std::uint32_t code = read_hex(index);
            index += 4;
            if (code >= 0xD800 && code <= 0xDBFF) {
                const std::uint32_t low = read_hex(index + 2);
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

/** Receives a decoded string into memory the caller has already sized. */
struct writing_to {
    char *position;
    constexpr void operator()(char value) noexcept { *this->position++ = value; }
    void operator()(std::string_view run) noexcept {
        // A run between dense escapes is a byte or two, not worth a call to copy.
        if (run.size() < 16) {
            for (const char value : run) *this->position++ = value;
            return;
        }
        std::memcpy(this->position, run.data(), run.size());
        this->position += run.size();
    }
};

/**
 * The decoded string, in a std::string's own storage. A string that carries no escape is taken
 * whole; one that does is decoded into room the size of the text as it stands, since every
 * escape is longer than what it stands for.
 */
inline void decode_string(string_span text, std::string &into) {
    if (!text.escaped) {
        into.assign(text.contents);
        return;
    }
    into.resize_and_overwrite(text.contents.size(), [&](char *buffer, std::size_t) {
        writing_to at { buffer };
        decode_string(text, at);
        return static_cast<std::size_t>(at.position - buffer);
    });
}

/** How many bytes the decoded string occupies. */
[[nodiscard]] constexpr std::size_t decoded_length(string_span text) noexcept {
    if (!text.escaped) return text.contents.size();
    std::size_t length = 0;
    decode_string(text, [&]<typename Piece>(Piece piece) {
        if constexpr (std::same_as<Piece, std::string_view>) length += piece.size();
        else ++length;
    });
    return length;
}

/** Compares a scanned string against a plain one without materialising it. */
[[nodiscard]] constexpr bool equals(string_span text, std::string_view plain) noexcept {
    if (!text.escaped) return text.contents == plain;

    std::size_t index = 0;
    bool matched = true;
    decode_string(text, [&]<typename Piece>(Piece piece) {
        if constexpr (std::same_as<Piece, std::string_view>) {
            if (index > plain.size() || plain.substr(index, piece.size()) != piece) matched = false;
            index += piece.size();
        } else {
            if (index >= plain.size() || plain[index] != piece) matched = false;
            ++index;
        }
    });
    return matched && index == plain.size();
}

template<bool Terminated>
void skip_value(basic_cursor<Terminated> &scan, int depth) noexcept;

template<bool Terminated>
void skip_container(basic_cursor<Terminated> &scan, bool object, int depth) noexcept {
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
            std::ignore = scan_string(scan);
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

template<bool Terminated>
void skip_value(basic_cursor<Terminated> &scan, int depth) noexcept {
    if (depth > max_depth) {
        scan.fail(errc::depth_exceeded);
        return;
    }
    skip_whitespace(scan);
    if (!scan.need(1)) return;

    switch (scan.peek()) {
    case '{': skip_container(scan, true, depth); return;
    case '[': skip_container(scan, false, depth); return;
    case '"': std::ignore = scan_string(scan); return;
    case 't': scan_literal(scan, "true"); return;
    case 'f': scan_literal(scan, "false"); return;
    case 'n': scan_literal(scan, "null"); return;
    default:
        if (scan.peek() == '-' || is_digit(scan.peek())) {
            std::ignore = scan_number(scan);
            return;
        }
        scan.fail(errc::unexpected_character);
        return;
    }
}

} // namespace serpent::json::scanner
