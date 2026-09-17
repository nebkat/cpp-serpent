#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>

// How a real is turned into text: by the copy of Żmij under external/, or by std::to_chars. The
// text is the same either way, character for character; Żmij produces it in about a third of the
// time. It has a source file that must be built, which the CMake target does - so the macro
// defaults to off here, for anyone using the headers without it, and CMake turns it on.
#ifndef SERPENT_USE_ZMIJ
#define SERPENT_USE_ZMIJ 0
#endif

#if SERPENT_USE_ZMIJ
#include <serpent/external/zmij/zmij.h>
#endif

namespace serpent::detail {

/**
 * A rendered number, held in the caller's frame.
 *
 * Returned by value rather than as a std::string because a document full of reals would
 * otherwise allocate two or three times per value, in the formatting rather than in the writer.
 * The longest form is a padded fixed-point number - twenty-one digits, a point and a zero -
 * with a sign.
 */
struct real_text {
    std::array<char, 40> storage; ///< left as it comes: only the first `length` characters mean anything
    std::size_t length = 0;

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return { this->storage.data(), this->length };
    }

    constexpr void push(char value) noexcept {
        if (this->length < this->storage.size()) this->storage[this->length++] = value;
    }

    constexpr void append(std::string_view text) noexcept {
        const auto count = std::min(text.size(), this->storage.size() - this->length);
        std::copy_n(text.data(), count, this->storage.data() + this->length);
        this->length += count;
    }

    constexpr void append(std::size_t count, char value) noexcept {
        count = std::min(count, this->storage.size() - this->length);
        std::fill_n(this->storage.data() + this->length, count, value);
        this->length += count;
    }
};

#if !SERPENT_USE_ZMIJ

/**
 * The shortest digits that read back as a double, and where the decimal point falls among them.
 *
 * Finding these is the hard part of writing a real, and the only part that depends on how it is
 * done: everything after it is layout. So this is the seam - one function produces it, and
 * which one is chosen when the library is built.
 */
struct shortest_digits {
    std::array<char, 32> storage {}; ///< room for the longest text a provider converts into
    std::size_t first = 0; ///< where in the storage the digits begin
    std::size_t count = 0; ///< a double needs at most seventeen
    int point = 0; ///< how many digits stand before the decimal point: may be negative, or more than there are

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return { this->storage.data() + this->first, this->count };
    }
};

/**
 * Finds them with the standard library, for a value that is finite and greater than zero.
 *
 * std::to_chars in scientific form writes "d.ddde+XX": the digits either side of a point, then
 * the power of ten of the first one. The point is all that stands between the first digit and
 * the rest, so writing the first digit over it leaves the digits as one run, a character later.
 */
[[nodiscard]] inline shortest_digits shortest_digits_of(double magnitude) {
    shortest_digits digits;
    char *const text = digits.storage.data();
    const auto converted = std::to_chars(text, text + digits.storage.size(), magnitude, std::chars_format::scientific);
    const std::string_view written { text, static_cast<std::size_t>(converted.ptr - text) };
    const auto exponent_at = written.rfind('e');

    if (exponent_at > 1) {
        text[1] = text[0];
        digits.first = 1;
        digits.count = exponent_at - 1;
    } else {
        digits.count = 1;
    }

    int exponent = 0;
    for (const char digit : written.substr(exponent_at + 2)) exponent = exponent * 10 + (digit - '0');
    if (written[exponent_at + 1] == '-') exponent = -exponent;
    digits.point = exponent + 1;
    return digits;
}

#endif

/**
 * A real as text: the shortest digits that read back as the same double, written out in full
 * from a ten-thousandth up to 1e16 and with an exponent beyond, and always recognisably a real.
 *
 * That last part is the ".0" on a whole number. JSON has one kind of number and does not say
 * whether 40 is an integer, so whatever reads it has to guess from the text - and a real written
 * as 40 comes back as an integer anywhere the type is inferred rather than known: a variant, a
 * tree, a document transcribed into a binary format. An exponent already marks a number as a
 * real, so only bare digits need the point.
 *
 * This is the format Żmij writes, which is Python's, with that one addition; the standard library
 * path lays the same digits out by the same rules, so the choice between them never shows in
 * the output.
 */
inline real_text format_real(double value) {
    real_text out;

    if (std::isnan(value)) {
        out.append("NaN");
        return out;
    }
    if (std::isinf(value)) {
        out.append(value < 0 ? "-Infinity" : "Infinity");
        return out;
    }

#if SERPENT_USE_ZMIJ
    char *const text = out.storage.data();
    out.length = static_cast<std::size_t>(external::zmij::write(text, out.storage.size(), value) - text);
    // Bare digits are what a whole number written in full comes out as, and the number says
    // whether it is one without the text being searched for a point.
    const bool bare_digits = std::abs(value) < 1e16 && std::trunc(value) == value;
    if (bare_digits) out.append(".0");
#else
    if (std::signbit(value)) out.push('-');
    if (value == 0) {
        out.append("0.0");
        return out;
    }

    const auto found = shortest_digits_of(std::abs(value));
    const std::string_view digits = found.view();
    const int point = found.point;

    if (point > 16 || point < -3) {
        // Too large or too small to write out: one digit, the rest behind a point, and the
        // power of ten of that first digit in at least two figures.
        out.append(digits.substr(0, 1));
        if (digits.size() > 1) {
            out.push('.');
            out.append(digits.substr(1));
        }
        const int power = point - 1;
        out.append(power < 0 ? "e-" : "e+");
        const int magnitude = power < 0 ? -power : power;
        if (magnitude < 10) out.push('0');
        char buffer[8];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), magnitude);
        out.append({ buffer, static_cast<std::size_t>(written.ptr - buffer) });
    } else if (point <= 0) {
        // Smaller than one: "0.", the zeros the exponent stands for, then every digit.
        out.append("0.");
        out.append(static_cast<std::size_t>(-point), '0');
        out.append(digits);
    } else if (static_cast<std::size_t>(point) >= digits.size()) {
        // A whole number: every digit, the zeros that pad it out to its size, and ".0".
        out.append(digits);
        out.append(static_cast<std::size_t>(point) - digits.size(), '0');
        out.append(".0");
    } else {
        // The point falls among the digits.
        out.append(digits.substr(0, static_cast<std::size_t>(point)));
        out.push('.');
        out.append(digits.substr(static_cast<std::size_t>(point)));
    }
#endif

    return out;
}

} // namespace serpent::detail
