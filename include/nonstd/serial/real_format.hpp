#pragma once

// Shared by the block-notation writer and the JSON writer, which both need to render a
// double the way the reference implementation does.

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

namespace nonstd::serial::detail {

/**
 * Renders a double the way Dart's toString does - ECMAScript's rules, plus a trailing .0
 * on a whole number - so a trace diffs cleanly against the reference implementation.
 */
inline std::string format_real(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    if (value == 0) return std::signbit(value) ? "-0.0" : "0.0";

    const bool negative = value < 0;

    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer),
                                         negative ? -value : value, std::chars_format::scientific);
    const std::string_view text { buffer, static_cast<std::size_t>(converted.ptr - buffer) };

    const auto exponent_at = text.find('e');
    std::string digits { text.substr(0, exponent_at) };
    if (const auto point = digits.find('.'); point != std::string::npos) digits.erase(point, 1);

    int exponent = 0;
    // from_chars rejects a leading '+', which to_chars always writes for a positive exponent.
    const auto tail = text.substr(exponent_at + (text[exponent_at + 1] == '+' ? 2 : 1));
    std::from_chars(tail.data(), tail.data() + tail.size(), exponent);

    const int significant = static_cast<int>(digits.size());
    const int point = exponent + 1;   // where the decimal point falls among the digits

    std::string out;
    if (significant <= point && point <= 21) {
        out = digits + std::string(static_cast<std::size_t>(point - significant), '0') + ".0";
    } else if (point > 0 && point <= 21) {
        out = digits.substr(0, static_cast<std::size_t>(point)) + "." + digits.substr(static_cast<std::size_t>(point));
    } else if (point > -6 && point <= 0) {
        out = "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
    } else {
        out = digits.substr(0, 1);
        if (significant > 1) out += "." + digits.substr(1);
        out += "e";
        out += point > 0 ? "+" : "-";
        out += std::to_string(point > 0 ? point - 1 : 1 - point);
    }
    return negative ? "-" + out : out;
}

}// namespace nonstd::serial::detail
