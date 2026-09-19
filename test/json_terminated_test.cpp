// A terminated text is read with loops that trust the zero byte after it instead of checking
// where it ends, and its numbers by readers of their own. Nothing a caller can see may differ
// from the bounded reading of the same text: whether a value was taken, its bits, where the
// cursor stopped, and for text that is refused, the error and where it points. Numbers are
// tried at their edges, and whole documents - every fixture, and text that is not a document -
// through both readers.

#include "check.hpp"

#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <bit>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace direct = serpent::json::direct;
namespace scanner = serpent::json::scanner;
namespace json = serpent::json;

namespace {

struct outcome {
    bool taken = false;
    std::uint64_t bits = 0;
    std::size_t stopped_at = 0;
    serpent::errc failure = serpent::errc::ok;
    std::size_t failure_offset = 0;

    friend bool operator==(const outcome &, const outcome &) = default;
};

template<typename T, bool Terminated>
outcome attempt(const std::string &text) {
    scanner::basic_cursor<Terminated> scan { text, text.data() };
    T value {};
    outcome result;
    result.taken = direct::read(scan, value);
    if constexpr (std::same_as<T, double>) result.bits = result.taken ? std::bit_cast<std::uint64_t>(value) : 0;
    else if constexpr (std::same_as<T, float>) result.bits = result.taken ? std::bit_cast<std::uint32_t>(value) : 0;
    else result.bits = result.taken ? static_cast<std::uint64_t>(value) : 0;
    result.stopped_at = static_cast<std::size_t>(scan.position - text.data());
    result.failure = scan.failure;
    result.failure_offset = scan.failure_offset;
    return result;
}

int compared = 0;

template<typename T>
void same_either_way(const std::string &text, const char *type) {
    const auto bounded = attempt<T, false>(text);
    const auto terminated = attempt<T, true>(text);
    ++compared;
    if (bounded == terminated) return;
    std::printf("  FAIL %s \"%s\": bounded    -> taken %d, bits %016" PRIx64 ", stopped %zu, error %d at %zu\n"
                "            terminated -> taken %d, bits %016" PRIx64 ", stopped %zu, error %d at %zu\n",
            type, text.c_str(), bounded.taken, bounded.bits, bounded.stopped_at, static_cast<int>(bounded.failure),
            bounded.failure_offset, terminated.taken, terminated.bits, terminated.stopped_at,
            static_cast<int>(terminated.failure), terminated.failure_offset);
    ++failures;
}

void every_type(const std::string &text) {
    same_either_way<double>(text, "double");
    same_either_way<float>(text, "float");
    same_either_way<std::int64_t>(text, "int64");
    same_either_way<std::uint64_t>(text, "uint64");
    same_either_way<std::int32_t>(text, "int32");
    same_either_way<std::uint8_t>(text, "uint8");
}

/** The same text alone, and with each thing a number can be followed by in a document. */
void in_each_setting(const std::string &number) {
    every_type(number);
    for (const char *after : { ",", "]", "}", " ", "\n", ",1", "x", ".", "e", "-", "\"" })
        every_type(number + after);
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream in { path, std::ios::binary };
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/** A document read both ways comes out the same tree, or is refused both ways. */
void same_document(const std::string &text) {
    const auto bounded = json::reader::over(text).as<serpent::value>();
    const auto terminated = json::terminated_reader::over(text).as<serpent::value>();
    check(bounded.has_value() == terminated.has_value(), "a document is taken or refused both ways");
    if (bounded && terminated) check(*bounded == *terminated, "and holds the same tree");
    check(json::validate(std::string_view { text }).has_value() == json::validate(text).has_value(),
            "and validates the same both ways");
}

} // namespace

int main() {
    for (const char *text : { "0", "-0", "0.0", "-0.0", "1", "-1", "10", "1.5", "-1.5", "1e5", "1E5", "1e+5", "1e-5",
                 "1.5e300", "1.5e-300", "0e0", "0.0e-0", "123456789012345678901234567890", "0.1", "0.2", "0.3",
                 "9007199254740993", "2.2250738585072014e-308", "4.9e-324", "1.7976931348623157e308", "1e309",
                 "-1e309", "1e-400", "1e400", "0.000000000000000000000000000000000000000000000000001",
                 "9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809",
                 "18446744073709551615", "18446744073709551616", "4294967295", "4294967296", "255", "256", "-1",
                 "1234567890123456789", "12345678901234567890", "123456789012345678901", "00", "01", "-01",
                 "1e", "1e+", "1e-", "1.", "1.e5", ".5", "-.5", "-", "--1", "+1", "0x10", "1e5.5", "1.5.5",
                 "1ee5", "inf", "nan", "1_000", "", "e5", "-e5", "0e", "0.e1", "1.0e", "1.0E+",
                 "1.0e0000000000000000000000000000000000000001", "0.5e-99999999999", "1e99999999999" })
        in_each_setting(text);

    std::mt19937_64 random { 20260919 };
    for (int index = 0; index < 200'000; ++index) {
        const double value = std::bit_cast<double>(random());
        if (value != value || value - value != 0) continue;
        char text[64];
        for (const char *format : { "%.17g", "%.15g", "%.6g", "%e", "%.3f", "%.0f" }) {
            std::snprintf(text, sizeof text, format, value);
            every_type(text);
        }
        std::snprintf(text, sizeof text, "%" PRId64, static_cast<std::int64_t>(random()));
        every_type(text);
        std::snprintf(text, sizeof text, "%" PRIu64, random());
        every_type(text);
    }
    constexpr std::string_view alphabet = "0123456789012345.eE+--";
    for (int index = 0; index < 2'000'000; ++index) {
        std::string text;
        const std::size_t length = 1 + random() % 24;
        for (std::size_t each = 0; each < length; ++each) text.push_back(alphabet[random() % alphabet.size()]);
        every_type(text);
    }
    std::printf("%d texts read both ways\n", compared);

    // Whole documents: every fixture the corpus has, and text that is not one.
    int documents = 0;
    for (const auto &entry : std::filesystem::directory_iterator { SERPENT_FIXTURE_DIR }) {
        if (entry.path().extension() != ".json") continue;
        same_document(read_file(entry.path()));
        ++documents;
    }
    for (const char *bad : { "[1,2", "{\"a\":1,}", "[01]", "[1e]", "{\"a\" 1}", "[1 2]", "{1:2}", "\"unterminated", "[\"\\x\"]",
                 "nul", "-", "[-]", "{\"a\":}", "", " ", "[", "]", "{\"", "[\"a", "tru", "[1,]", "{\"a\":1}x", "[1]\n\n ",
                 "\t{ \"k\" : [ true , false , null , 1.5e2 , \"s\\u00e9\" ] } " })
        same_document(bad);
    std::printf("%d fixtures and some non-documents read both ways\n", documents);

    return report("json_terminated");
}
