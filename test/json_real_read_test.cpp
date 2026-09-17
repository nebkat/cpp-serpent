// A real can be read in two walks - the grammar, then std::from_chars over what it accepted - or
// in one, by fast_float told to accept exactly JSON. Which is used is a switch, so the two must
// not differ in anything a caller can see: whether the text was taken, the value to the bit,
// where the cursor was left, and for text that is refused, the error and where it points.
//
// Most of what is tried here is not a number. Agreeing about well-formed numbers is the easy
// half; the grammars could differ only at the edges, so the edges are where the text comes from.

#include "check.hpp"

#include <serpent/json/direct.hpp>

#include <bit>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#if SERPENT_USE_FAST_FLOAT

namespace direct = serpent::json::direct;
namespace scanner = serpent::json::scanner;

namespace {

struct outcome {
    bool taken = false;
    std::uint64_t bits = 0;
    std::size_t stopped_at = 0;
    serpent::errc failure = serpent::errc::ok;
    std::size_t failure_offset = 0;

    friend bool operator==(const outcome &, const outcome &) = default;
};

template<typename Read>
outcome attempt(std::string_view text, Read read) {
    scanner::cursor scan { text, text.data() };
    double value = 0;

    outcome result;
    result.taken = direct::at_number(scan) && read(scan, value);
    result.bits = result.taken ? std::bit_cast<std::uint64_t>(value) : 0;
    result.stopped_at = static_cast<std::size_t>(scan.position - text.data());
    result.failure = scan.failure;
    result.failure_offset = scan.failure_offset;
    return result;
}

int compared = 0;

void same_either_way(std::string_view text) {
    const auto two = attempt(text, [](auto &scan, double &into) { return direct::read_real_in_two_walks(scan, into); });
    const auto one = attempt(text, [](auto &scan, double &into) { return direct::read_real_in_one_walk(scan, into); });
    ++compared;
    if (one == two) return;

    std::printf("  FAIL \"%.*s\": two walks -> taken %d, bits %016" PRIx64 ", stopped %zu, error %d at %zu\n"
                "        one walk  -> taken %d, bits %016" PRIx64 ", stopped %zu, error %d at %zu\n",
            static_cast<int>(text.size()), text.data(), two.taken, two.bits, two.stopped_at,
            static_cast<int>(two.failure), two.failure_offset, one.taken, one.bits, one.stopped_at,
            static_cast<int>(one.failure), one.failure_offset);
    ++failures;
}

/** The same text alone, and with each thing a number can be followed by in a document. */
void in_each_setting(const std::string &number) {
    same_either_way(number);
    for (const char *after : { ",", "]", "}", " ", "\n", ",1", "x", ".", "e", "-", "\"" })
        same_either_way(number + after);
}

std::string printed(const char *format, double value) {
    char text[512];
    const int length = std::snprintf(text, sizeof text, format, value);
    return std::string { text, static_cast<std::size_t>(length) };
}

} // namespace

int main() {
    for (const char *text : { "0", "-0", "0.0", "-0.0", "1", "-1", "10", "1.5", "-1.5", "1e5", "1E5", "1e+5", "1e-5",
                 "1.5e300", "1.5e-300", "0e0", "0.0e-0", "123456789012345678901234567890", "0.1", "0.2", "0.3",
                 "9007199254740993", "2.2250738585072014e-308", "2.2250738585072011e-308", "4.9e-324", "5e-324",
                 "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623157e308",
                 "1.7976931348623158e308", "1.7976931348623159e308", "1e308", "1e309", "-1e309", "1e-400", "1e400",
                 "0.000000000000000000000000000000000000000000000000000000000000000000000000000000001",
                 "1e", "1e+", "1e-", "1.", "1.e5", ".5", "-.5", "-", "--1", "+1", "01", "-01", "00", "0x10", "1e5.5",
                 "1.5.5", "1ee5", "1e++5", "inf", "-inf", "nan", "-nan", "Infinity", "NaN", "1_000", "1,5", "",
                 "e5", "-e5", "0e", "0.e1", "1.0e", "1.0E+", "1.0e0000000000000000000000000000000000000001",
                 "0.00000000000000000000000000000000000000000000000000000000000000000000000000000e500" })
        in_each_setting(text);

    // Numbers as they are usually written, in every layout printf has.
    std::mt19937_64 random { 20260917 };
    for (int index = 0; index < 300'000; ++index) {
        const double value = std::bit_cast<double>(random());
        if (value != value || value - value != 0) continue;
        for (const char *format : { "%.17g", "%.15g", "%.6g", "%e", "%.3f", "%.0f" })
            same_either_way(printed(format, value));
    }

    // And text that mostly is not a number: whatever these characters happen to spell.
    constexpr std::string_view alphabet = "0123456789012345.eE+--";
    for (int index = 0; index < 5'000'000; ++index) {
        std::string text;
        const std::size_t length = 1 + random() % 12;
        for (std::size_t each = 0; each < length; ++each) text.push_back(alphabet[random() % alphabet.size()]);
        same_either_way(text);
    }

    std::printf("%d texts read both ways\n", compared);
    return report("json_real_read");
}

#else

int main() {
    std::printf("SERPENT_USE_FAST_FLOAT is off, so there is one way to read a real and nothing to compare\n");
    return report("json_real_read");
}

#endif
