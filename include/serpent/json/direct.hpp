#pragma once

// Reading a value whose type is already known, where the cursor stands.
//
// A reader handle is for a caller who does not yet know what a value is: it can be asked, copied
// and read more than once. Underneath, every one of its scalar reads is one of these - a grammar
// scan, then a conversion of exactly the text the scan accepted - and a reader generated for a
// type, which knows what each member is, calls them without a handle in between.
//
// Each returns false when what stands at the cursor is not a value of the kind asked for, leaving
// the cursor where it was so the caller can step over whatever is there instead. A value of the
// right kind that is malformed fails the cursor itself.

#include <serpent/config.hpp>
#include <serpent/json/scan.hpp>

#if SERPENT_USE_FAST_FLOAT
#include <serpent/external/fast_float/fast_float.h>
#endif

#include <charconv>
#include <concepts>
#include <limits>
#include <string>
#include <utility>

namespace serpent::json::direct {

template<bool Terminated>
SERPENT_ALWAYS_INLINE [[nodiscard]] bool read(scanner::basic_cursor<Terminated> &scan, bool &into) noexcept {
    if (scanner::accept(scan, "true")) {
        into = true;
        return true;
    }
    if (scanner::accept(scan, "false")) {
        into = false;
        return true;
    }
    return false;
}

/** Whether a number could begin at the cursor: a digit, or the sign before one. */
template<bool Terminated>
[[nodiscard]] constexpr bool at_number(const scanner::basic_cursor<Terminated> &scan) noexcept {
    return scan.available(1) && (scan.peek() == '-' || scanner::is_digit(scan.peek()));
}

/**
 * An integer, converted in the same walk that finds where it ends.
 *
 * std::from_chars reads an integer exactly as JSON writes one, with two exceptions that are both
 * decided by looking at a single character afterwards: it accepts a leading zero, which JSON
 * forbids, and it stops happily at the point or exponent of a real, which here means the value
 * is not an integer at all. So there is no need to walk the digits first to check the grammar.
 * Converted into the type asked for, which is how "does not fit" is found: a value too wide for
 * it is out of range.
 */
/**
 * The same over a terminated text, where the digits can be walked with one test per byte: a
 * digit is taken, anything else - the terminator included - ends the run. Nineteen digits fit
 * a uint64 whatever they are, so the run is accumulated unchecked up to there and only what
 * comes after is looked at. The loop is unrolled: its count is a constant, and each step is a
 * load, a compare and a multiply-add.
 */
template<std::integral T>
    requires (!std::same_as<T, bool>)
SERPENT_ALWAYS_INLINE [[nodiscard]] bool read_terminated(scanner::terminated_cursor &scan, T &into) noexcept {
    const char *const start = scan.position;
    const char *at = start;
    const bool negative = *at == '-';
    if (negative) ++at;

    const auto digit_at = [](const char *position) noexcept {
        return static_cast<unsigned>(static_cast<unsigned char>(*position) - static_cast<unsigned char>('0'));
    };
    if (digit_at(at) > 9) return false;
    if (*at == '0' && digit_at(at + 1) <= 9) {
        scan.fail(errc::invalid_number, start);
        return false;
    }

    std::uint64_t magnitude = 0;
#pragma GCC unroll 19
    for (int count = 0; count < 19; ++count) {
        const unsigned digit = digit_at(at);
        if (digit > 9) break;
        magnitude = magnitude * 10 + digit;
        ++at;
    }
    if (digit_at(at) <= 9) {
        // A twentieth digit fits a uint64 only just, and there is never a twenty-first.
        const unsigned digit = digit_at(at);
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
        ++at;
        if (digit_at(at) <= 9) return false;
    }
    if (*at == '.' || *at == 'e' || *at == 'E') return false;

    if constexpr (std::is_signed_v<T>) {
        constexpr auto most = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        if (negative) {
            if (magnitude > most + 1) return false;
            into = static_cast<T>(std::uint64_t { 0 } - magnitude);
        } else {
            if (magnitude > most) return false;
            into = static_cast<T>(magnitude);
        }
    } else {
        if (negative && magnitude != 0) return false;
        if (magnitude > std::numeric_limits<T>::max()) return false;
        into = static_cast<T>(magnitude);
    }
    scan.position = at;
    return true;
}

template<bool Terminated, std::integral T>
    requires (!std::same_as<T, bool>)
SERPENT_ALWAYS_INLINE [[nodiscard]] bool read(scanner::basic_cursor<Terminated> &scan, T &into) noexcept {
    if constexpr (Terminated) return read_terminated(scan, into);

    if (!at_number(scan)) return false;

    const char *const start = scan.position;
    T value {};
    bool fits = true;
    std::from_chars_result converted {};
    if constexpr (std::is_unsigned_v<T>) {
        // A negative integer fits an unsigned type only as -0, which from_chars will not read
        // into one; the signed read says whether it is that - once the text is known to be a
        // number at all, which is what is checked first.
        if (*start == '-') {
            std::int64_t negative {};
            converted = std::from_chars(start, scan.limit, negative);
            fits = std::in_range<T>(negative);
            value = fits ? static_cast<T>(negative) : T {};
        } else {
            converted = std::from_chars(start, scan.limit, value);
        }
    } else {
        converted = std::from_chars(start, scan.limit, value);
    }
    if (converted.ec == std::errc::invalid_argument) return false;

    // A leading zero is not a number whatever the type, so it is found before the value is
    // found not to fit: from_chars leaves ptr past the digits either way.
    const char *const digits = *start == '-' ? start + 1 : start;
    if (*digits == '0' && converted.ptr - digits > 1) {
        scan.fail(errc::invalid_number, start);
        return false;
    }
    if (converted.ec != std::errc {}) return false;
    const bool is_real = converted.ptr != scan.limit
            && (*converted.ptr == '.' || *converted.ptr == 'e' || *converted.ptr == 'E');
    if (is_real || !fits) return false;

    into = value;
    scan.position = converted.ptr;
    return true;
}

/**
 * A real, in two walks: the grammar is checked, then what it accepted is converted.
 *
 * Two where an integer needs one, because what std::from_chars accepts as a real is wider than
 * JSON in ways that cannot be told from one character afterwards - "inf", ".5", "5." - so the
 * conversion is only ever shown text the grammar has already passed.
 */
template<bool Terminated, std::floating_point T>
[[nodiscard, gnu::noinline, gnu::cold]] bool read_real_in_two_walks(scanner::basic_cursor<Terminated> &scan, T &into) noexcept {
    const auto text = scanner::scan_number(scan);
    if (!scan.ok()) return false;

    double value = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
    // out_of_range means the literal overflows a double; JSON has no infinity to mean.
    if (converted.ec != std::errc {}) {
        scan.position = text.data();
        return false;
    }
    into = static_cast<T>(value);
    return true;
}

#if SERPENT_USE_FAST_FLOAT

/**
 * A real, in one walk: fast_float can be told to accept what JSON does, so the walk that
 * converts is also the one that checks.
 *
 * With one exception, decided by looking at a single character afterwards as the integer read
 * does. Given an exponent with no digits - "1e", "1e+" - fast_float takes the number before it
 * and stops at the 'e', where JSON says the whole is malformed. A number it has taken is never
 * followed by an 'e' otherwise, so one that is, is that case.
 *
 * Text that is refused, either way, is handed to the two-walk read, which refuses it too and
 * says why and where - the slow way to fail, and failing is not what needs to be fast.
 */
template<bool Terminated, std::floating_point T>
[[nodiscard]] bool read_real_in_one_walk(scanner::basic_cursor<Terminated> &scan, T &into) noexcept {
    namespace fast_float = serpent::external::fast_float;

    double value = 0;
    const auto converted = fast_float::from_chars(scan.position, scan.limit, value, fast_float::chars_format::json);
    const bool exponent_without_digits =
            converted.ptr != scan.limit && (*converted.ptr == 'e' || *converted.ptr == 'E');
    if (converted.ec == std::errc::invalid_argument || exponent_without_digits)
        return read_real_in_two_walks(scan, into);
    if (converted.ec != std::errc {}) return false;

    into = static_cast<T>(value);
    scan.position = converted.ptr;
    return true;
}

#endif

#if SERPENT_USE_FAST_FLOAT

/**
 * A real over a terminated text: the digits gathered with one test per byte, then handed to
 * fast_float's conversion as the number it found, which is the seam it offers for a front end
 * of one's own. What this front end takes is the common shape - up to nineteen significant
 * digits, the grammar as JSON has it. Anything else goes the bounded way, which decides it
 * the same and says why where it refuses: more digits than a uint64 holds, and the text at
 * fault when there is one.
 */
template<std::floating_point T>
[[nodiscard]] bool read_real_terminated(scanner::terminated_cursor &scan, T &into) noexcept {
    namespace fast_float = serpent::external::fast_float;

    const auto digit_at = [](const char *position) noexcept {
        return static_cast<unsigned>(static_cast<unsigned char>(*position) - static_cast<unsigned char>('0'));
    };
    const auto the_bounded_way = [&] {
        scanner::cursor bounded;
        bounded.origin = scan.origin;
        bounded.position = scan.position;
        bounded.limit = scan.limit;
        const bool read = at_number(bounded) && read_real_in_one_walk(bounded, into);
        scan.position = bounded.position;
        if (!bounded.ok()) scan.fail(bounded.failure, bounded.origin + bounded.failure_offset);
        return read;
    };

    fast_float::parsed_number_string_t<char> number;
    const char *p = scan.position;
    number.negative = *p == '-';
    if (number.negative) ++p;

    const char *const start_digits = p;
    std::uint64_t mantissa = 0;
    while (digit_at(p) <= 9) {
        mantissa = 10 * mantissa + digit_at(p);
        ++p;
    }
    std::int64_t digit_count = p - start_digits;
    if (digit_count == 0) return number.negative ? the_bounded_way() : false;
    if (*start_digits == '0' && digit_count > 1) return the_bounded_way();
    number.integer = fast_float::span<const char>(start_digits, static_cast<std::size_t>(digit_count));

    std::int64_t exponent = 0;
    if (*p == '.') {
        ++p;
        const char *const before = p;
        fast_float::loop_parse_if_eight_digits(p, scan.limit, mantissa);
        while (digit_at(p) <= 9) {
            mantissa = 10 * mantissa + digit_at(p);
            ++p;
        }
        if (p == before) return the_bounded_way();
        exponent = before - p;
        number.fraction = fast_float::span<const char>(before, static_cast<std::size_t>(p - before));
        digit_count += p - before;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        bool negative_exponent = false;
        if (*p == '-') {
            negative_exponent = true;
            ++p;
        } else if (*p == '+') {
            ++p;
        }
        if (digit_at(p) > 9) return the_bounded_way();
        std::int64_t explicit_exponent = 0;
        while (digit_at(p) <= 9) {
            if (explicit_exponent < 0x10000000) explicit_exponent = 10 * explicit_exponent + digit_at(p);
            ++p;
        }
        exponent += negative_exponent ? -explicit_exponent : explicit_exponent;
    }
    if (digit_count > 19) return the_bounded_way();

    number.mantissa = mantissa;
    number.exponent = exponent;
    number.lastmatch = p;
    number.valid = true;
    double value = 0;
    const auto converted = fast_float::from_chars_advanced(number, value);
    if (converted.ec != std::errc {}) return false;
    into = static_cast<T>(value);
    scan.position = p;
    return true;
}

#endif

template<bool Terminated, std::floating_point T>
SERPENT_ALWAYS_INLINE [[nodiscard]] bool read(scanner::basic_cursor<Terminated> &scan, T &into) noexcept {
#if SERPENT_USE_FAST_FLOAT
    if constexpr (Terminated) return read_real_terminated(scan, into);
    if (!at_number(scan)) return false;
    return read_real_in_one_walk(scan, into);
#else
    if (!at_number(scan)) return false;
    return read_real_in_two_walks(scan, into);
#endif
}

/**
 * A string, which always owns its bytes once read.
 *
 * The scan that finds the closing quote also finds out whether anything between the quotes needs
 * decoding. Most strings need nothing and are taken whole; one that carries an escape is decoded
 * straight into the string's own storage.
 */
template<bool Terminated>
[[nodiscard]] bool read(scanner::basic_cursor<Terminated> &scan, std::string &into) {
    if (!scan.available(1) || scan.peek() != '"') return false;

    const auto text = scanner::scan_string(scan);
    if (!scan.ok()) return false;

    scanner::decode_string(text, into);
    return true;
}

/** A type one of the reads above handles. */
template<typename T>
concept readable = requires(scanner::cursor &scan, T &into) {
    { direct::read(scan, into) } -> std::same_as<bool>;
};

} // namespace serpent::json::direct
