// JSON output. The fixture suite already checks 44 documents against the reference implementation's own JSON
// rendering; this covers what JSON-from-a-C++-value adds on top, and the lossy mappings.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/bjdata/json.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;
namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

namespace {

struct[[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};

/**
 * A type whose two directions differ, so it uses the to_json/from_json pair rather than a
 * single json_convert.
 *
 * One template, and the body knows nothing about the writer: it says value(), and each writer
 * decides what that means. BJData measures the readings and picks a packed [$u# array when
 * that is smaller, copying the payload in one go; JSON writes plain numbers. Neither the type
 * nor its author has to know that happened.
 */
struct legacy {
    int code = 0;
    std::vector<std::uint16_t> readings;

    friend void to_json(auto &out, const legacy &value) {
        const auto scope = out.object();
        scope.member("code", value.code);
        scope.member("readings", value.readings);
    }

    friend bool from_json(const auto &source, legacy &value) {
        if (!source.is_object()) return false;
        value.code = source["code"].template as<int>().value_or(0);
        value.readings.clear();
        for (const auto &element : source["readings"].array()) {
            value.readings.push_back(element.template as<std::uint16_t>().value_or(0));
        }
        return true;
    }
};

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto value : bytes) {
        out += digits[static_cast<unsigned>(value) >> 4];
        out += digits[static_cast<unsigned>(value) & 0xF];
    }
    return out;
}

reader parse(std::string_view text, std::vector<std::byte> &storage) {
    storage = from_hex(text);
    return reader::over(storage);
}

void scalars_and_escaping() {
    check_equal(json::encode(nullptr), std::string { "null" }, "null");
    check_equal(json::encode(true), std::string { "true" }, "true");
    check_equal(json::encode(42), std::string { "42" }, "integer");
    check_equal(json::encode(-1), std::string { "-1" }, "negative integer");
    check_equal(json::encode(1.5), std::string { "1.5" }, "real");
    check_equal(json::encode(2.0), std::string { "2.0" }, "a whole real keeps its point");

    // A real is written with the shortest digits that read back as the same double: in full from
    // a ten-thousandth up to 1e16, with an exponent beyond, and a whole number keeping its ".0" so
    // that it still reads back as a real. Each of these is a place the layout changes or a digit
    // could be lost.
    const std::pair<double, std::string_view> reals[] {
        { -40.0, "-40.0" },
        { 100.0, "100.0" },
        { 0.0, "0.0" },
        { -0.0, "-0.0" },
        { 0.1, "0.1" },
        { -39.9, "-39.9" },
        { 0.001, "0.001" },
        { 1234.5, "1234.5" },
        { 0.30000000000000004, "0.30000000000000004" },
        { 1.0 / 3.0, "0.3333333333333333" },
        // the last whole numbers written in full, which is past where a double holds every digit
        { 1e15, "1000000000000000.0" },
        { 9007199254740993.0, "9007199254740992.0" },
        { 9999999999999998.0, "9999999999999998.0" },
        // and the first that are not
        { 1e16, "1e+16" },
        { 123456789012345680000.0, "1.2345678901234568e+20" },
        { 1e21, "1e+21" },
        { -1.5e21, "-1.5e+21" },
        // the same at the small end
        { 0.0001, "0.0001" },
        { 0.00012345, "0.00012345" },
        { 0.00001, "1e-05" },
        { 1.5e-7, "1.5e-07" },
        { -1e-7, "-1e-07" },
        // an exponent has at least two figures, and as many more as it needs
        { 1e100, "1e+100" },
        { 1.25e-100, "1.25e-100" },
        { 5e-324, "5e-324" },
        { 2.2250738585072014e-308, "2.2250738585072014e-308" },
        { 1.7976931348623157e308, "1.7976931348623157e+308" },
    };
    for (const auto &[value, expected] : reals)
        check_equal(std::string_view { json::encode(value) }, expected, "a real as text");
    check_equal(json::encode(std::string { "hello" }), std::string { "\"hello\"" }, "string");

    check_equal(json::encode(std::string { "a\"b" }), std::string { "\"a\\\"b\"" }, "quote is escaped");
    check_equal(json::encode(std::string { "a\\b" }), std::string { "\"a\\\\b\"" }, "backslash is escaped");
    check_equal(json::encode(std::string { "a\nb\tc" }), std::string { "\"a\\nb\\tc\"" }, "newline and tab");
    check_equal(json::encode(std::string { "a\x01"
                                           "b" }),
            std::string { "\"a\\u0001b\"" }, "control character");
    check_equal(json::encode(std::string { "a\x7f" }), std::string { "\"a\x7f\"" }, "delete is not escaped");
    // UTF-8 passes through rather than being expanded to \u escapes.
    check_equal(json::encode(std::string { "h\xc3\xa9llo \xe2\x9c\x93" }),
            std::string { "\"h\xc3\xa9llo \xe2\x9c\x93\"" }, "utf-8 passes through");
    check_equal(json::encode(std::string { "" }), std::string { "\"\"" }, "empty string");

    // JSON cannot express these, so they become null rather than invalid output.
    check_equal(json::encode(std::numeric_limits<double>::quiet_NaN()), std::string { "null" }, "NaN is null");
    check_equal(json::encode(std::numeric_limits<double>::infinity()), std::string { "null" }, "infinity is null");
    check_equal(json::encode(-std::numeric_limits<double>::infinity()), std::string { "null" }, "-infinity is null");
}

void containers() {
    check_equal(json::encode(std::vector<int> {}), std::string { "[]" }, "empty array");
    check_equal(json::encode(std::vector<int> { 1, 2, 3 }), std::string { "[1,2,3]" }, "compact array");
    check_equal(json::encode(std::vector<std::vector<int>> { { 1 }, { 2, 3 } }), std::string { "[[1],[2,3]]" },
            "nested arrays");
    check_equal(json::encode(std::map<std::string, int> { { "a", 1 }, { "b", 2 } }),
            std::string { "{\"a\":1,\"b\":2}" }, "map is an object");
    check_equal(json::encode(std::vector<std::optional<int>> { 1, std::nullopt }), std::string { "[1,null]" },
            "an empty optional is null");

    check_equal(
            json::encode(point { 3, 4 }), std::string { "{\"x\":3,\"y\":4}" }, "the macro form writes JSON directly");
    // One definition. The same call site writes a JSON array here...
    check_equal(json::encode(legacy { 7, { 1, 2, 3 } }), std::string { "{\"code\":7,\"readings\":[1,2,3]}" },
            "one templated to_json writes JSON");

    // ...and a typed array of what the readings are here, from the very same to_json body.
    const legacy larger { 7, { 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007 } };
    check_equal(std::string_view { hex(encode(larger)) },
            "7b5504636f64655507550872656164696e67735b2475235508e803e903ea03eb03ec03ed03ee03ef037d",
            "and BJData, with no change to the type");
    check_equal(json::encode(larger).substr(0, 24), std::string { "{\"code\":7,\"readings\":[10" },
            "while JSON writes numbers either way");

    const auto from_text = json::decode<legacy>(R"({"code":9,"readings":[4,5]})");
    check(from_text.has_value() && from_text->code == 9 && from_text->readings.size() == 2,
            "and the matching from_json reads JSON back");
    const auto from_binary = decode<legacy>(encode(larger));
    check(from_binary.has_value() && from_binary->readings.size() == 8 && from_binary->readings[7] == 1007,
            "and reads a packed BJData array back from the same definition");

    check_equal(json::encode(std::vector<point> { { 1, 2 }, { 3, 4 } }),
            std::string { "[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]" }, "array of structs");

    // Binary has no JSON spelling and is written as an array of integers.
    const std::array<std::byte, 3> bytes { std::byte { 0xde }, std::byte { 0xad }, std::byte { 0x01 } };
    check_equal(json::encode(bytes), std::string { "[222,173,1]" }, "binary is an array of integers");
}

void floats_at_their_own_precision() {
    check_equal(json::encode(0.1f), std::string { "0.1" }, "a float has fewer digits than the double it widens to");
    check_equal(json::encode(1e7f), std::string { "1e+07" }, "and takes an exponent from ten million");
    check_equal(json::encode(9999999.0f), std::string { "9999999.0" }, "just below, written in full");
    check_equal(json::encode(std::vector<float> { 0.1f, 0.2f }), std::string { "[0.1,0.2]" }, "in a range");
    check(json::decode<float>("0.1") == 0.1f, "and reads back as the float it was");
}

void indentation() {
    check_equal(json::encode(point { 3, 4 }, { .indent = 2 }), std::string { "{\n  \"x\": 3,\n  \"y\": 4\n}" },
            "indented object");
    check_equal(json::encode(std::vector<int> { 1, 2 }, { .indent = 2 }), std::string { "[\n  1,\n  2\n]" },
            "indented array");
    // An empty container stays on one line, as the reference does.
    check_equal(json::encode(std::vector<int> {}, { .indent = 2 }), std::string { "[]" }, "indented empty array");
    check_equal(json::encode(std::vector<std::vector<int>> { { 1 } }, { .indent = 2 }),
            std::string { "[\n  [\n    1\n  ]\n]" }, "indent nests");
    check_equal(json::encode(point { 1, 2 }, { .indent = 4 }), std::string { "{\n    \"x\": 1,\n    \"y\": 2\n}" },
            "indent width is configurable");
}

void from_documents() {
    std::vector<std::byte> storage;

    // High precision is a number too wide for a double, so it stays unquoted.
    check_equal(json::encode(parse("4855143132333435363738393031323334353637383930", storage)),
            std::string { "12345678901234567890" }, "high precision is an unquoted number");

    // A char is a one-character string.
    check_equal(json::encode(parse("4361", storage)), std::string { "\"a\"" }, "char is a string");

    // A typed array is just an array.
    check_equal(json::encode(parse("5b2455235503010203", storage)), std::string { "[1,2,3]" }, "typed array");

    // An N-D array is nested rather than flattened.
    check_equal(json::encode(parse("5b2455235b550255035d010203040506", storage)), std::string { "[[1,2,3],[4,5,6]]" },
            "2x3 N-D array nests");
    check_equal(json::encode(parse("5b2455235b5b550255035d5d010203040506", storage)),
            std::string { "[[1,3,5],[2,4,6]]" }, "column-major N-D array nests in logical order");

    // A malformed document yields nothing rather than half a string.
    check(json::encode(parse("5b5501", storage)).empty(), "a truncated document produces no JSON");
}

void sinks_and_failures() {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    const auto written = json::write(out, point { 1, 2 });
    check(written.has_value(), "write_TMP reports success");
    check_equal(*written, buffer.size(), "and the byte count");

    counting_sink counter;
    json::writer measuring { counter };
    measuring.value(point { 1, 2 });
    check(measuring.finish().has_value(), "counting a JSON document");
    check_equal(counter.size(), std::size_t { 13 }, "{\"x\":1,\"y\":2} is thirteen bytes");

    {
        json::writer target { out };
        target.key("orphan");
        check_equal(target.error_code(), errc::key_outside_object, "a key outside an object fails");
    }
    {
        // A fixed buffer latches rather than truncating silently.
        std::array<std::byte, 4> tiny {};
        span_sink small { tiny };
        json::writer target { small };
        target.value(std::vector<int> { 1, 2, 3, 4, 5, 6 });
        check(!target.finish().has_value(), "a bounded sink fails");
        check(small.overflowed(), "and latches overflow");
    }
}

} // namespace

int main() {
    scalars_and_escaping();
    containers();
    floats_at_their_own_precision();
    indentation();
    from_documents();
    sinks_and_failures();
    return report("json_writer");
}
