#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>

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
    std::array<char, 40> storage {};
    std::size_t length = 0;

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return { this->storage.data(), this->length };
    }

    constexpr void push(char value) noexcept {
        if (this->length < this->storage.size()) this->storage[this->length++] = value;
    }

    constexpr void append(std::string_view text) noexcept {
        for (const char value : text) this->push(value);
    }

    constexpr void append(std::size_t count, char value) noexcept {
        for (std::size_t index = 0; index < count; ++index) this->push(value);
    }
};

/** ECMAScript's number-to-string rules, plus a trailing .0 on a whole number. */
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
    if (value == 0) {
        out.append(std::signbit(value) ? "-0.0" : "0.0");
        return out;
    }

    const bool negative = value < 0;
    if (negative) out.push('-');

    char buffer[64];
    const auto converted =
            std::to_chars(buffer, buffer + sizeof(buffer), negative ? -value : value, std::chars_format::scientific);
    const std::string_view text { buffer, static_cast<std::size_t>(converted.ptr - buffer) };

    const auto exponent_at = text.find('e');

    // The mantissa with its point taken out. A shortest round-trip double is at most 17 digits.
    std::array<char, 24> digits {};
    std::size_t significant = 0;
    for (const char digit : text.substr(0, exponent_at)) {
        if (digit != '.' && significant < digits.size()) digits[significant++] = digit;
    }

    int exponent = 0;
    // from_chars rejects a leading '+', which to_chars always writes for a positive exponent.
    const auto tail = text.substr(exponent_at + (text[exponent_at + 1] == '+' ? 2 : 1));
    std::from_chars(tail.data(), tail.data() + tail.size(), exponent);

    const std::string_view mantissa { digits.data(), significant };
    const int point = exponent + 1; // where the decimal point falls among the digits

    if (static_cast<int>(significant) <= point && point <= 21) {
        out.append(mantissa);
        out.append(static_cast<std::size_t>(point) - significant, '0');
        out.append(".0");
    } else if (point > 0 && point <= 21) {
        out.append(mantissa.substr(0, static_cast<std::size_t>(point)));
        out.push('.');
        out.append(mantissa.substr(static_cast<std::size_t>(point)));
    } else if (point > -6 && point <= 0) {
        out.append("0.");
        out.append(static_cast<std::size_t>(-point), '0');
        out.append(mantissa);
    } else {
        out.append(mantissa.substr(0, 1));
        if (significant > 1) {
            out.push('.');
            out.append(mantissa.substr(1));
        }
        out.push('e');
        out.push(point > 0 ? '+' : '-');

        char exponent_digits[8];
        const auto rendered = std::to_chars(
                exponent_digits, exponent_digits + sizeof(exponent_digits), point > 0 ? point - 1 : 1 - point);
        out.append({ exponent_digits, static_cast<std::size_t>(rendered.ptr - exponent_digits) });
    }

    return out;
}

} // namespace serpent::detail
