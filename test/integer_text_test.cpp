// An integer's text has to be the same whichever way it is produced, and the table-driven ways
// store digits in pairs and fours without first finding out how many there are - so they may
// write a little past where they stop. They are given room for the longest value of the type
// and no more, and this holds them to it: every value is written into exactly that much room
// with guard bytes after it, and compared with std::to_chars.
//
// Built once for each setting of SERPENT_INTEGER_TABLE.

#include "check.hpp"

#include <serpent/json/text.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <type_traits>
#include <utility>
#include <string_view>

namespace json = serpent::json;

namespace {

constexpr std::size_t guard = 8;
constexpr char guard_byte = '#';

template<typename T>
void same_as_to_chars(T value) {
    constexpr std::size_t room = json::widest_text<T>;

    std::array<char, room + guard> written;
    written.fill(guard_byte);
    const char *const end = json::write_text(written.data(), value);

    // As a number, never as the character a char would otherwise be taken for.
    using wide = std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>;
    std::array<char, 24> expected {};
    const auto reference = std::to_chars(expected.data(), expected.data() + expected.size(), static_cast<wide>(value));

    const std::string_view ours { written.data(), static_cast<std::size_t>(end - written.data()) };
    const std::string_view theirs { expected.data(), static_cast<std::size_t>(reference.ptr - expected.data()) };
    if (ours != theirs) check_equal(ours, theirs, "an integer's text");

    for (std::size_t index = room; index < written.size(); ++index)
        if (written[index] != guard_byte) check(false, "nothing is written past the longest the type can be");
}

/** Every value near where the number of digits changes, and near where the number of bits does. */
template<typename T>
void edges() {
    using limits = std::numeric_limits<T>;
    using wide = std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>;

    const auto around = [](wide centre) {
        for (wide offset = -3; offset <= 3; ++offset) {
            const wide value = centre + offset;
            if (std::in_range<T>(value)) same_as_to_chars(static_cast<T>(value));
        }
    };

    same_as_to_chars(limits::min());
    same_as_to_chars(limits::max());
    same_as_to_chars(static_cast<T>(limits::min() + 1));
    same_as_to_chars(static_cast<T>(limits::max() - 1));
    around(0);

    wide power = 1;
    for (int digits = 1; digits < limits::digits10 + 1; ++digits) {
        power *= 10;
        around(power);
        if constexpr (std::is_signed_v<T>) around(-power);
    }
    for (int bit = 1; bit < limits::digits; ++bit) {
        around(static_cast<wide>(1ULL << bit));
        if constexpr (std::is_signed_v<T>) around(-static_cast<wide>(1ULL << bit));
    }
}

/** Values of every length about equally often, which values drawn evenly from the range are not. */
template<typename T>
void of_every_length(std::mt19937_64 &random, int count) {
    using unsigned_t = std::make_unsigned_t<T>;
    for (int index = 0; index < count; ++index) {
        const int bits = static_cast<int>(random() % (sizeof(T) * 8)) + 1;
        const auto mask = bits == 64 ? ~0ULL : (1ULL << bits) - 1;
        same_as_to_chars(static_cast<T>(static_cast<unsigned_t>(random() & mask)));
    }
}

template<typename T>
void every_value() {
    for (long long value = std::numeric_limits<T>::min(); value <= std::numeric_limits<T>::max(); ++value)
        same_as_to_chars(static_cast<T>(value));
}

} // namespace

int main() {
    std::printf("SERPENT_INTEGER_TABLE=%d\n", SERPENT_INTEGER_TABLE);

    every_value<std::int8_t>();
    every_value<std::uint8_t>();
    every_value<std::int16_t>();
    every_value<std::uint16_t>();
    every_value<char>();

    std::mt19937_64 random { 20260917 };
    edges<std::int32_t>();
    edges<std::uint32_t>();
    edges<std::int64_t>();
    edges<std::uint64_t>();
    edges<long>();
    edges<unsigned long>();
    edges<long long>();
    edges<unsigned long long>();

    of_every_length<std::int32_t>(random, 2'000'000);
    of_every_length<std::uint32_t>(random, 2'000'000);
    of_every_length<std::int64_t>(random, 2'000'000);
    of_every_length<std::uint64_t>(random, 2'000'000);

    return report("integer_text");
}
