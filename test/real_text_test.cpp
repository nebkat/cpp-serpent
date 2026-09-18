// A real's text is produced by whichever of several pieces of code the build chose - Żmij with
// its table, Żmij without it, or std::to_chars - and has to be right whichever it was. Right is
// said here without reference to any of them: the text reads back as exactly the value it was
// written from, it carries no more digits than the shortest text that does, and a whole number
// keeps a ".0" so that it is not read back as an integer.

#include "check.hpp"

#include <serpent/real_format.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <string_view>

namespace {

/** The digits that carry the value: no sign, point, exponent, or zeros at either end. */
std::string significant_digits(std::string_view text) {
    std::string digits;
    for (const char one : text.substr(0, text.find_first_of("eE")))
        if (one >= '0' && one <= '9') digits.push_back(one);
    digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size()));
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    return digits;
}

int written = 0;

template<std::floating_point T>
void written_rightly(T value) {
    char room[serpent::detail::real_text_capacity + 1];
    const std::size_t length = serpent::detail::write_real(room, value);
    room[length] = '\0';
    const std::string_view text { room, length };
    ++written;

    if (length > serpent::detail::real_text_capacity) check(false, "a real fits the room it is given");

    const T back = std::same_as<T, float> ? T(std::strtof(room, nullptr)) : T(std::strtod(room, nullptr));
    if (std::bit_cast<std::conditional_t<std::same_as<T, float>, std::uint32_t, std::uint64_t>>(back)
            != std::bit_cast<std::conditional_t<std::same_as<T, float>, std::uint32_t, std::uint64_t>>(value))
        check_equal(text, std::string_view { "(text that reads back as the same value)" }, "a real reads back exactly");

    char shortest[64];
    // Scientific, because the plain form writes a large whole number out digit by digit, exactly,
    // which is more digits than it takes to tell it from its neighbours.
    const auto reference = std::to_chars(shortest, shortest + sizeof shortest, value, std::chars_format::scientific);
    const auto expected = significant_digits({ shortest, static_cast<std::size_t>(reference.ptr - shortest) });
    if (significant_digits(text) != expected)
        check_equal(text, std::string_view { expected }, "a real is written with its shortest digits");

    if (text.find_first_of(".e") == std::string_view::npos)
        check_equal(text, std::string_view { "(text with a point or an exponent)" }, "a real does not look like an integer");
}

} // namespace

int main() {
    std::printf("SERPENT_USE_ZMIJ=%d\n", SERPENT_USE_ZMIJ);

    for (const double value : { 0.0, -0.0, 1.0, -1.0, 0.1, 0.5, 1e15, 1e16, 1e17, 1e-4, 1e-5, 123456789.0,
                 9007199254740992.0, 9007199254740993.0, 1e21, 1e22, 1e23, 5e-324, 2.2250738585072014e-308,
                 2.2250738585072009e-308, 1.7976931348623157e308, 4.35, 0.3, 2.5e-7, 299792458.0, 6.02214076e23 }) {
        written_rightly(value);
        written_rightly(-value);
    }

    std::mt19937_64 random { 20260917 };

    // Every bit pattern about equally likely, which is mostly very large and very small values.
    for (int index = 0; index < 3'000'000; ++index) {
        const double value = std::bit_cast<double>(random());
        if (std::isfinite(value)) written_rightly(value);
    }
    // The values people write: a few digits, scaled by a power of ten, and whole numbers.
    for (int index = 0; index < 3'000'000; ++index) {
        const auto digits = static_cast<double>(random() % 10'000'000);
        const int scale = static_cast<int>(random() % 40) - 20;
        written_rightly(digits * std::pow(10.0, scale));
        written_rightly(static_cast<double>(static_cast<std::int64_t>(random() >> (random() % 64))));
    }
    // Neighbours of powers of two and ten, where the spacing of doubles changes.
    for (int exponent = -1070; exponent <= 1023; ++exponent) {
        const double power = std::ldexp(1.0, exponent);
        written_rightly(power);
        written_rightly(std::nextafter(power, 0.0));
        written_rightly(std::nextafter(power, std::numeric_limits<double>::infinity()));
    }
    for (int exponent = -320; exponent <= 308; ++exponent) {
        const double power = std::strtod(("1e" + std::to_string(exponent)).c_str(), nullptr);
        written_rightly(power);
        written_rightly(std::nextafter(power, 0.0));
        written_rightly(std::nextafter(power, std::numeric_limits<double>::infinity()));
    }

    // Floats: their own shortest digits, which are fewer, and their own limit on writing in full.
    for (const float value : { 0.1f, 1e7f, 9999999.0f, 16777216.0f, 1e-4f, 1e-5f, 3.4028235e38f, 1.4e-45f, 0.0f, 100.0f })
        written_rightly(value);
    for (int index = 0; index < 2'000'000; ++index) {
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        if (std::isfinite(value)) written_rightly(value);
    }
    for (int index = 0; index < 1'000'000; ++index)
        written_rightly(static_cast<float>(random() % 10'000'000) / 100.0f);

    std::printf("%d reals written\n", written);
    return report("real_text");
}
