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

#include <serpent/json/scan.hpp>

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

/** Any arithmetic type but bool, which JSON spells as a word rather than a number. */
template<typename T>
concept number = (std::integral<T> || std::floating_point<T>) && !std::same_as<T, bool>;

/**
 * A number, converted from exactly the text the grammar accepted.
 *
 * An integer is read as the widest integer of its signedness and then checked against the type
 * asked for, so that "does not fit" is one test whatever the width. Converting an integer stops
 * at a fraction or an exponent, so a real leaves text unconverted - which is how a real is told
 * from an integer here, without looking for the point separately.
 */
template<number T>
[[nodiscard]] bool read(scanner::cursor &scan, T &into) noexcept {
    if (!scan.available(1) || !(scan.peek() == '-' || scanner::is_digit(scan.peek()))) return false;

    const char *const start = scan.position;
    const auto text = scanner::scan_number(scan);
    if (!scan.ok()) return false;
    const char *const end = text.data() + text.size();

    const auto convert = [&]<typename Wide>() {
        Wide wide {};
        const auto converted = std::from_chars(text.data(), end, wide);
        if (converted.ec != std::errc {} || converted.ptr != end) return false;
        if constexpr (std::integral<T>) {
            if (!std::in_range<T>(wide)) return false;
        }
        into = static_cast<T>(wide);
        return true;
    };

    bool fits = false;
    if constexpr (std::floating_point<T>) {
        fits = convert.template operator()<double>();
    } else if (text.front() == '-') {
        fits = convert.template operator()<std::int64_t>();
    } else {
        fits = convert.template operator()<std::uint64_t>();
    }
    if (!fits) scan.position = start;
    return fits;
}

/**
 * A string, which always owns its bytes once read.
 *
 * The scan that finds the closing quote also finds out whether anything between the quotes needs
 * decoding. Most strings need nothing and are taken whole; only one that carries an escape is
 * walked a character at a time to resolve it.
 */
[[nodiscard]] inline bool read(scanner::cursor &scan, std::string &into) {
    if (!scan.available(1) || scan.peek() != '"') return false;

    const auto text = scanner::scan_string(scan);
    if (!scan.ok()) return false;

    if (!text.escaped) {
        into.assign(text.contents);
        return true;
    }
    into.clear();
    into.reserve(scanner::decoded_length(text));
    scanner::decode_string(text, [&](char value) { into.push_back(value); });
    return true;
}

/** A type one of the reads above handles. */
template<typename T>
concept readable = requires(scanner::cursor &scan, T &into) {
    { direct::read(scan, into) } -> std::same_as<bool>;
};

} // namespace serpent::json::direct
