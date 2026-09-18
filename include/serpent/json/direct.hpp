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
#include <string>
#include <utility>

namespace serpent::json::direct {

[[nodiscard]] inline bool read(scanner::cursor &scan, bool &into) noexcept {
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
[[nodiscard]] constexpr bool at_number(const scanner::cursor &scan) noexcept {
    return scan.available(1) && (scan.peek() == '-' || scanner::is_digit(scan.peek()));
}

/**
 * An integer, converted in the same walk that finds where it ends.
 *
 * std::from_chars reads an integer exactly as JSON writes one, with two exceptions that are both
 * decided by looking at a single character afterwards: it accepts a leading zero, which JSON
 * forbids, and it stops happily at the point or exponent of a real, which here means the value
 * is not an integer at all. So there is no need to walk the digits first to check the grammar.
 *
 * Read as the widest integer of its signedness and then checked against the type asked for, so
 * that "does not fit" is one test whatever the width.
 */
template<std::integral T>
    requires (!std::same_as<T, bool>)
[[nodiscard]] bool read(scanner::cursor &scan, T &into) noexcept {
    if (!at_number(scan)) return false;

    const char *const start = scan.position;
    const bool negative = *start == '-';
    const char *const digits = negative ? start + 1 : start;

    T value {};
    const auto convert = [&]<typename Wide>() -> const char * {
        Wide wide {};
        const auto converted = std::from_chars(start, scan.limit, wide);
        if (converted.ec != std::errc {} || !std::in_range<T>(wide)) return nullptr;
        value = static_cast<T>(wide);
        return converted.ptr;
    };
    const char *const end = negative ? convert.template operator()<std::int64_t>()
                                     : convert.template operator()<std::uint64_t>();
    if (end == nullptr) return false;

    if (*digits == '0' && end - digits > 1) {
        scan.fail(errc::invalid_number, start);
        return false;
    }
    const bool is_real = end != scan.limit && (*end == '.' || *end == 'e' || *end == 'E');
    if (is_real) return false;

    into = value;
    scan.position = end;
    return true;
}

/**
 * A real, in two walks: the grammar is checked, then what it accepted is converted.
 *
 * Two where an integer needs one, because what std::from_chars accepts as a real is wider than
 * JSON in ways that cannot be told from one character afterwards - "inf", ".5", "5." - so the
 * conversion is only ever shown text the grammar has already passed.
 */
template<std::floating_point T>
[[nodiscard]] bool read_real_in_two_walks(scanner::cursor &scan, T &into) noexcept {
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
template<std::floating_point T>
[[nodiscard]] bool read_real_in_one_walk(scanner::cursor &scan, T &into) noexcept {
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

template<std::floating_point T>
[[nodiscard]] bool read(scanner::cursor &scan, T &into) noexcept {
    if (!at_number(scan)) return false;
#if SERPENT_USE_FAST_FLOAT
    return read_real_in_one_walk(scan, into);
#else
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
[[nodiscard]] inline bool read(scanner::cursor &scan, std::string &into) {
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
