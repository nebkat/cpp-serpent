// JSON output. The fixture suite already checks 44 documents against dart-bjdata's own JSON
// rendering; this covers what JSON-from-a-C++-value adds on top, and the lossy mappings.

#include "check.hpp"

#include <nonstd/bjdata.hpp>
#include <nonstd/bjdata/json.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace nonstd::bjdata;

namespace {

struct point {
    int x = 0;
    int y = 0;
    BJDATA_DEFINE_TYPE(point, x, y)
};

/** A to_bjdata-only type, which reaches JSON through the transcoding fallback. */
struct legacy {
    int code = 0;
    std::string label;

    friend void to_bjdata(writer &out, const legacy &value) {
        const auto scope = out.object();
        scope.member("code", value.code);
        scope.member("label", value.label);
    }
    friend bool from_bjdata(view source, legacy &value) {
        value.code = source["code"].as_int<int>().value_or(0);
        value.label = source["label"].as_string().value_or("");
        return source.is_object();
    }
};

view parse(std::string_view hex, std::vector<std::byte> &storage) {
    storage = from_hex(hex);
    return view::over(storage);
}

void scalars_and_escaping() {
    check_equal(to_json(nullptr), std::string { "null" }, "null");
    check_equal(to_json(true), std::string { "true" }, "true");
    check_equal(to_json(42), std::string { "42" }, "integer");
    check_equal(to_json(-1), std::string { "-1" }, "negative integer");
    check_equal(to_json(1.5), std::string { "1.5" }, "real");
    check_equal(to_json(2.0), std::string { "2.0" }, "a whole real keeps its point");
    check_equal(to_json(std::string { "hello" }), std::string { "\"hello\"" }, "string");

    check_equal(to_json(std::string { "a\"b" }), std::string { "\"a\\\"b\"" }, "quote is escaped");
    check_equal(to_json(std::string { "a\\b" }), std::string { "\"a\\\\b\"" }, "backslash is escaped");
    check_equal(to_json(std::string { "a\nb\tc" }), std::string { "\"a\\nb\\tc\"" }, "newline and tab");
    check_equal(to_json(std::string { "a\x01" "b" }), std::string { "\"a\\u0001b\"" }, "control character");
    check_equal(to_json(std::string { "a\x7f" }), std::string { "\"a\x7f\"" }, "delete is not escaped");
    // UTF-8 passes through rather than being expanded to \u escapes.
    check_equal(to_json(std::string { "h\xc3\xa9llo \xe2\x9c\x93" }), std::string { "\"h\xc3\xa9llo \xe2\x9c\x93\"" },
                "utf-8 passes through");
    check_equal(to_json(std::string { "" }), std::string { "\"\"" }, "empty string");

    // JSON cannot express these, so they become null rather than invalid output.
    check_equal(to_json(std::numeric_limits<double>::quiet_NaN()), std::string { "null" }, "NaN is null");
    check_equal(to_json(std::numeric_limits<double>::infinity()), std::string { "null" }, "infinity is null");
    check_equal(to_json(-std::numeric_limits<double>::infinity()), std::string { "null" }, "-infinity is null");
}

void containers() {
    check_equal(to_json(std::vector<int> {}), std::string { "[]" }, "empty array");
    check_equal(to_json(std::vector<int> { 1, 2, 3 }), std::string { "[1,2,3]" }, "compact array");
    check_equal(to_json(std::vector<std::vector<int>> { { 1 }, { 2, 3 } }), std::string { "[[1],[2,3]]" },
                "nested arrays");
    check_equal(to_json(std::map<std::string, int> { { "a", 1 }, { "b", 2 } }), std::string { "{\"a\":1,\"b\":2}" },
                "map is an object");
    check_equal(to_json(std::vector<std::optional<int>> { 1, std::nullopt }), std::string { "[1,null]" },
                "an empty optional is null");

    check_equal(to_json(point { 3, 4 }), std::string { "{\"x\":3,\"y\":4}" }, "the macro form writes JSON directly");
    check_equal(to_json(legacy { 7, "x" }), std::string { "{\"code\":7,\"label\":\"x\"}" },
                "a to_bjdata type reaches JSON through a document");
    // The same type still round-trips through BJData itself, which is the path JSON borrows.
    const auto restored = from_bytes<legacy>(to_bytes(legacy { 7, "x" }));
    check(restored.has_value() && restored->code == 7 && restored->label == "x",
          "and round-trips through BJData unchanged");

    check_equal(to_json(std::vector<point> { { 1, 2 }, { 3, 4 } }),
                std::string { "[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]" }, "array of structs");

    // Binary has no JSON spelling and is written as an array of integers.
    const std::array<std::byte, 3> bytes { std::byte { 0xde }, std::byte { 0xad }, std::byte { 0x01 } };
    check_equal(to_json(bytes), std::string { "[222,173,1]" }, "binary is an array of integers");
}

void indentation() {
    check_equal(to_json(point { 3, 4 }, { .indent = 2 }),
                std::string { "{\n  \"x\": 3,\n  \"y\": 4\n}" }, "indented object");
    check_equal(to_json(std::vector<int> { 1, 2 }, { .indent = 2 }),
                std::string { "[\n  1,\n  2\n]" }, "indented array");
    // An empty container stays on one line, as the reference does.
    check_equal(to_json(std::vector<int> {}, { .indent = 2 }), std::string { "[]" }, "indented empty array");
    check_equal(to_json(std::vector<std::vector<int>> { { 1 } }, { .indent = 2 }),
                std::string { "[\n  [\n    1\n  ]\n]" }, "indent nests");
    check_equal(to_json(point { 1, 2 }, { .indent = 4 }),
                std::string { "{\n    \"x\": 1,\n    \"y\": 2\n}" }, "indent width is configurable");
}

void from_documents() {
    std::vector<std::byte> storage;

    // High precision is a number too wide for a double, so it stays unquoted.
    check_equal(to_json(parse("4855143132333435363738393031323334353637383930", storage)),
                std::string { "12345678901234567890" }, "high precision is an unquoted number");

    // A char is a one-character string.
    check_equal(to_json(parse("4361", storage)), std::string { "\"a\"" }, "char is a string");

    // A typed array is just an array.
    check_equal(to_json(parse("5b2455235503010203", storage)), std::string { "[1,2,3]" }, "typed array");

    // An N-D array is nested rather than flattened.
    check_equal(to_json(parse("5b2455235b550255035d010203040506", storage)),
                std::string { "[[1,2,3],[4,5,6]]" }, "2x3 N-D array nests");
    check_equal(to_json(parse("5b2455235b5b550255035d5d010203040506", storage)),
                std::string { "[[1,3,5],[2,4,6]]" }, "column-major N-D array nests in logical order");

    // A malformed document yields nothing rather than half a string.
    check(to_json(parse("5b5501", storage)).empty(), "a truncated document produces no JSON");
}

void sinks_and_failures() {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    const auto written = write_json(out, point { 1, 2 });
    check(written.has_value(), "write_json reports success");
    check_equal(*written, buffer.size(), "and the byte count");

    counting_sink counter;
    json_writer measuring { counter };
    measuring.value(point { 1, 2 });
    check(measuring.finish().has_value(), "counting a JSON document");
    check_equal(counter.size(), std::size_t { 13 }, "{\"x\":1,\"y\":2} is thirteen bytes");

    {
        json_writer target { out };
        target.key("orphan");
        check_equal(target.error_code(), errc::key_outside_object, "a key outside an object fails");
    }
    {
        // A fixed buffer latches rather than truncating silently.
        std::array<std::byte, 4> tiny {};
        span_sink small { tiny };
        json_writer target { small };
        target.value(std::vector<int> { 1, 2, 3, 4, 5, 6 });
        check(!target.finish().has_value(), "a bounded sink fails");
        check(small.overflowed(), "and latches overflow");
    }
}

}// namespace

int main() {
    scalars_and_escaping();
    containers();
    indentation();
    from_documents();
    sinks_and_failures();
    return report("bjdata_json");
}
