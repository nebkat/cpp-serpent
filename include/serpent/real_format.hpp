#pragma once

#include <algorithm>
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

/**
 * ECMAScript's number-to-string rules, plus a trailing .0 on a whole number.
 *
 * std::to_chars finds the shortest digits that read back as the same double, which is the hard
 * part, and in scientific form it hands them over as "d.ddde+XX": a first digit, the rest after
 * a point, and where the point really belongs. What is left is to lay those same digits out the
 * way the reference implementation does - written out in full between a millionth and 1e21, with
 * an exponent beyond - which is a matter of copying two runs of digits to the right places.
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
    if (value == 0) {
        out.append(std::signbit(value) ? "-0.0" : "0.0");
        return out;
    }
    if (value < 0) out.push('-');

    char buffer[32];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), std::abs(value), std::chars_format::scientific);
    const std::string_view text { buffer, static_cast<std::size_t>(converted.ptr - buffer) };

    // "d.ddde+XX": one digit, then the rest behind a point that is not there when there are none.
    const auto exponent_at = text.rfind('e');
    const std::string_view first = text.substr(0, 1);
    const std::string_view rest = exponent_at > 1 ? text.substr(2, exponent_at - 2) : std::string_view {};
    const std::size_t significant = 1 + rest.size();

    const bool exponent_negative = text[exponent_at + 1] == '-';
    int exponent = 0;
    for (const char digit : text.substr(exponent_at + 2)) exponent = exponent * 10 + (digit - '0');
    if (exponent_negative) exponent = -exponent;

    const int point = exponent + 1; // how many digits stand before the decimal point

    if (point > 21 || point <= -6) {
        // Too large or too small to write out: the digits as they came, and the exponent without
        // the leading zero to_chars pads it with.
        out.append(first);
        if (!rest.empty()) {
            out.push('.');
            out.append(rest);
        }
        out.push('e');
        out.push(exponent_negative ? '-' : '+');
        std::string_view magnitude = text.substr(exponent_at + 2);
        if (magnitude.size() > 1 && magnitude.front() == '0') magnitude.remove_prefix(1);
        out.append(magnitude);
    } else if (point <= 0) {
        // Smaller than one: "0.", the zeros the exponent stands for, then every digit.
        out.append("0.");
        out.append(static_cast<std::size_t>(-point), '0');
        out.append(first);
        out.append(rest);
    } else if (static_cast<std::size_t>(point) >= significant) {
        // A whole number: every digit, the zeros that pad it out to its size, and ".0".
        out.append(first);
        out.append(rest);
        out.append(static_cast<std::size_t>(point) - significant, '0');
        out.append(".0");
    } else {
        // The point falls among the digits, `point - 1` of the way into the rest.
        const auto before = static_cast<std::size_t>(point) - 1;
        out.append(first);
        out.append(rest.substr(0, before));
        out.push('.');
        out.append(rest.substr(before));
    }

    return out;
}

} // namespace serpent::detail
