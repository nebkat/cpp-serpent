// The JSON reader. The fixture suite additionally parses the reference implementation's own JSON for all 44
// documents and checks the values against the same digests the BJData reader is held to.

#include "check.hpp"

#include <serpent/json.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <tuple>

using namespace serpent;
using namespace serpent::json;

static_assert(std::input_iterator<array_iterator>);
static_assert(std::input_iterator<member_iterator>);
static_assert(std::ranges::input_range<array_range>);
static_assert(std::ranges::input_range<member_range>);

namespace {

reader parse(std::string_view text) { return reader::over(text); }

void scalars() {
    check(parse("null").is_null(), "null");
    check_equal(parse("true").as<bool>().value_or(false), true, "true");
    check_equal(parse("false").as<bool>().value_or(true), false, "false");

    check_equal(parse("0").as<int>().value_or(-1), 0, "zero");
    check_equal(parse("255").as<int>().value_or(0), 255, "255");
    check_equal(parse("-129").as<int>().value_or(0), -129, "negative");
    check_equal(parse("4294967296").as<std::int64_t>().value_or(0), 4294967296ll, "wide integer");
    check_equal(parse("-9223372036854775808").as<std::int64_t>().value_or(0),
            std::numeric_limits<std::int64_t>::min(), "int64 minimum");
    check_equal(parse("18446744073709551615").as<std::uint64_t>().value_or(0),
            std::numeric_limits<std::uint64_t>::max(), "uint64 maximum");

    check(parse("1").is_integer(), "1 is an integer");
    check(parse("1.0").is_real(), "1.0 is a real");
    check(parse("1e3").is_real(), "an exponent makes it a real");
    check(std::abs(parse("3.141592653589793").as<double>().value_or(0) - 3.141592653589793) < 1e-15, "pi");
    check(std::abs(parse("-0.25").as<double>().value_or(0) + 0.25) < 1e-15, "negative real");
    check(std::abs(parse("1e-20").as<double>().value_or(0) - 1e-20) < 1e-30, "small exponent");
    // An integer reads as a float, but not the other way round.
    check_equal(parse("2").as<double>().value_or(0), 2.0, "an integer reads as a float");
    check(!parse("2.5").as<int>().has_value(), "a real does not read as an integer");
    check(!parse("256").as<std::uint8_t>().has_value(), "narrowing is range checked");
    check(!parse("-1").as<unsigned>().has_value(), "a negative does not read as unsigned");

    // An integer is converted in the same walk that finds its end, so each place the standard
    // conversion and JSON disagree about what an integer is has to be settled afterwards. These
    // are those places.
    check(!parse("1e3").as<int>().has_value(), "an exponent makes a real, even with no point");
    check(!parse("1E3").as<int>().has_value(), "in either case");
    check(!parse("7.0").as<int>().has_value(), "a real that happens to be whole is still a real");
    check_equal(parse("-0").as<int>().value_or(9), 0, "negative zero is an integer");
    check_equal(parse("-0").as<unsigned>().value_or(9u), 0u, "and fits an unsigned type");
    check(!parse("9223372036854775808").as<std::int64_t>().has_value(), "one past the widest signed");
    check(!parse("18446744073709551616").as<std::uint64_t>().has_value(), "one past the widest unsigned");
    check(!parse("-9223372036854775809").as<std::int64_t>().has_value(), "one below the narrowest");
    check_equal(parse("[10,20]")[0].as<int>().value_or(0), 10, "an integer ends at a comma");
    check_equal(parse("[10]")[0].as<int>().value_or(0), 10, "or a bracket");
    check_equal(parse("{\"a\":10}")["a"].as<int>().value_or(0), 10, "or a brace");
    check_equal(parse("10").as<int>().value_or(0), 10, "or the end of the text");
    // Leading zeros are not JSON, whatever a conversion would make of them.
    check(!json::validate("01"), "a leading zero is not a number");
    check(!json::decode<int>("01").has_value(), "and does not decode as one");
    check(!json::decode<int>("-01").has_value(), "behind a sign either");
    check(!json::decode<std::vector<int>>("[1,02,3]").has_value(), "nor inside a container");

    // Grammatically valid but not representable.
    check(parse("1e400").is_real(), "1e400 parses");
    check(!parse("1e400").as<double>().has_value(), "1e400 does not fit a double");
}

void strings() {
    check_equal(parse(R"("hello")").as<std::string>().value_or("?"), std::string { "hello" }, "plain");
    check_equal(parse(R"("")").as<std::string>().value_or("?"), std::string { "" }, "empty");
    check_equal(parse(R"("a\"b\\c\/d")").as<std::string>().value_or("?"), std::string { "a\"b\\c/d" }, "escapes");
    check_equal(
            parse(R"("a\nb\tc\r\b\f")").as<std::string>().value_or("?"), std::string { "a\nb\tc\r\b\f" }, "control escapes");
    check_equal(parse(R"("\u00e9\u2713")").as<std::string>().value_or("?"), std::string { "\xc3\xa9\xe2\x9c\x93" },
            "bmp escapes become utf-8");
    check_equal(parse(R"("\ud83d\ude00")").as<std::string>().value_or("?"), std::string { "\xf0\x9f\x98\x80" },
            "a surrogate pair becomes one code point");
    check_equal(parse("\"h\xc3\xa9llo\"").as<std::string>().value_or("?"), std::string { "h\xc3\xa9llo" },
            "raw utf-8 passes through");

    // Comparison never materialises the string.
    check(parse(R"("a\nb")").string_is("a\nb"), "escaped comparison matches");
    check(!parse(R"("a\nb")").string_is("a\\nb"), "and is not the literal text");

    // Decoding into caller storage, for paths that may not allocate.
    std::array<char, 8> buffer {};
    const auto written = parse(R"("a\nb")").decode_string_into(buffer);
    check(written.has_value() && *written == 3, "decode into a span");
    check_equal(std::string_view(buffer.data(), 3), std::string_view { "a\nb" }, "decoded contents");
    std::array<char, 2> tiny {};
    check(!parse(R"("hello")").decode_string_into(tiny).has_value(), "a short buffer is refused, not truncated");
}

void containers() {
    check_equal(parse("[]").size(), std::size_t { 0 }, "empty array");
    check_equal(parse("{}").size(), std::size_t { 0 }, "empty object");
    check_equal(parse("[1,2,3]").size(), std::size_t { 3 }, "array size");
    check_equal(parse("[1,2,3]")[1].as<int>().value_or(0), 2, "array index");
    check(!parse("[1,2,3]")[3].is_valid(), "index past the end poisons");

    // Whitespace anywhere it is allowed.
    const auto spaced = parse("  {\n  \"a\" : [ 1 , 2 ] ,\n  \"b\" : \"x\"\n}  ");
    check_equal(spaced.size(), std::size_t { 2 }, "object with whitespace");
    check_equal(spaced["a"].size(), std::size_t { 2 }, "nested array with whitespace");
    check_equal(spaced["a"][1].as<int>().value_or(0), 2, "nested value");
    check_equal(spaced["b"].as<std::string>().value_or("?"), std::string { "x" }, "string member");
    check(!spaced["missing"].is_valid(), "missing key poisons");

    std::string keys;
    for (const auto &entry : spaced.items())
        keys += entry.key_string();
    check_equal(keys, std::string { "ab" }, "items preserves order");

    // A key containing an escape still matches.
    check_equal(parse(R"({"a\nb":7})")["a\nb"].as<int>().value_or(0), 7, "escaped key lookup");

    check_equal(parse(R"([[1,[2,[3]]]])")[0][1][1][0].as<int>().value_or(0), 3, "deep nesting");
    check_equal(parse(R"([{"a":1},{"a":2}])")[1]["a"].as<int>().value_or(0), 2, "array of objects");
}

void malformed() {
    const auto rejected = [](std::string_view text, errc expected, std::string_view what) {
        const auto result = json::validate(text);
        check(!result.has_value(), what);
        if (!result) check_equal(result.error().code(), expected, what);
    };

    check(json::validate("[1,2,3]").has_value(), "a well formed array validates");
    check(json::validate("  {\"a\": [1, null, true]}  ").has_value(), "whitespace is allowed around it");

    rejected("", errc::unexpected_end, "empty input");
    rejected("[1,2", errc::unterminated_container, "unterminated array");
    rejected("{\"a\":1", errc::unterminated_container, "unterminated object");
    rejected("[1,2,3]x", errc::trailing_data, "trailing data");
    rejected("[1,]", errc::unexpected_character, "trailing comma");
    rejected("{\"a\" 1}", errc::unexpected_character, "missing colon");
    rejected("{a:1}", errc::unexpected_character, "unquoted key");
    rejected("tru", errc::unexpected_end, "truncated literal");
    rejected("trux", errc::unexpected_character, "misspelled literal");

    // JSON's number grammar is narrower than from_chars would accept.
    rejected("01", errc::invalid_number, "leading zero");
    rejected("-", errc::invalid_number, "lone minus");
    rejected(".5", errc::unexpected_character, "bare fraction");
    rejected("5.", errc::invalid_number, "trailing point");
    rejected("1e", errc::invalid_number, "empty exponent");
    rejected("1e+", errc::invalid_number, "exponent sign with no digits");
    rejected("+1", errc::unexpected_character, "leading plus");
    rejected("nan", errc::unexpected_character, "nan is not JSON");
    rejected("inf", errc::unexpected_character, "infinity is not JSON");

    rejected("\"a\\q\"", errc::invalid_escape, "unknown escape");
    rejected("\"a\\u00\"", errc::invalid_escape, "short unicode escape");
    rejected("\"\\ud83d\"", errc::invalid_escape, "unpaired high surrogate");
    rejected("\"\\udc00\"", errc::invalid_escape, "lone low surrogate");
    rejected("\"a\nb\"", errc::invalid_string, "raw newline in a string");

    std::string deep;
    for (int level = 0; level < max_depth + 5; ++level)
        deep += "[";
    deep += "1";
    for (int level = 0; level < max_depth + 5; ++level)
        deep += "]";
    rejected(deep, errc::depth_exceeded, "over deep nesting");
}

void truncation() {
    for (const auto *text : { R"({"a":[1,2,3],"b":"x\ny","c":{"d":true}})", R"([1,-2.5,1e10,null,false,"\u00e9"])",
                 R"({"":{},"x":[]})" }) {
        const std::string_view whole { text };
        for (std::size_t length = 0; length < whole.size(); ++length) {
            const auto prefix = whole.substr(0, length);
            check(!json::validate(prefix).has_value(), "a truncated document fails validation");

            // Traversal of an unvalidated truncated document must still be safe.
            const auto value = reader::over(prefix);
            std::size_t seen = 0;
            for (const auto &element : value.array()) {
                std::ignore = element.as<long long>();
                if (++seen > 64) break;
            }
            for (const auto &entry : value.items()) {
                std::ignore = entry.key_is("a");
                std::ignore = entry.value.as<std::string>();
                if (++seen > 64) break;
            }
            std::ignore = value.size();
            std::ignore = value["a"];
            std::ignore = value[0];
            std::ignore = value.as<std::string>();
            std::ignore = value.extent();
        }
    }
}

} // namespace

int main() {
    scalars();
    strings();
    containers();
    malformed();
    truncation();
    return report("json_reader");
}
