// The three ways a user type opts in, and that all of them round-trip.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;
namespace json = serpent::json;

namespace {

enum class mode : std::uint8_t { off, automatic, on };

/** Aggregates that C++20 would let you build straight from a std::string. */
struct labelled {
    std::string text;
    SERPENT_DEFINE_TYPE(labelled, text)
};

struct first_string_member {
    std::string name;
    int count = 0;
    SERPENT_DEFINE_TYPE(first_string_member, name, count)
};

// --- Form C: the macro, for a plain aggregate ---
struct point {
    int x = 0;
    int y = 0;
    SERPENT_DEFINE_TYPE(point, x, y)
};

// --- Form B: one function, both directions ---
struct segment {
    point start {};
    point end {};
    std::string label = "unnamed";

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), segment> value) {
        visitor.member("start", value.start);
        visitor.member("end", value.end);
        visitor.member("label", value.label);
    }
};

// --- Form A: separate functions, for behaviour that differs by direction ---
// An absent optional is omitted from the output entirely, and a missing key on the way in
// falls back to a default-constructed value.
struct connection {
    std::string host = "localhost";
    std::optional<int> port {};
    mode fallback = mode::automatic;

    friend void to_json(auto &out, const connection &value) {
        const auto scope = out.object();
        scope.member("host", value.host);
        if (value.port) scope.member("port", *value.port);
        scope.member("fallback", value.fallback);
    }

    friend bool from_json(auto source, connection &value) {
        if (!source.is_object()) return false;
        const connection defaults {};
        value.host = source["host"].as_string().value_or(defaults.host);
        value.port = source["port"].template as_int<int>();
        value.fallback = static_cast<mode>(
                source["fallback"].template as_int<std::uint8_t>().value_or(std::to_underlying(defaults.fallback)));
        return true;
    }
};

// --- Form C again, non-intrusively, for a type you cannot edit ---
struct extent {
    int width = 0;
    int height = 0;
};
SERPENT_DEFINE_TYPE_NON_INTRUSIVE(extent, width, height)

struct document {
    std::vector<point> points {};
    std::map<std::string, int> counts {};
    std::optional<segment> highlight {};

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), document> value) {
        visitor.member("points", value.points);
        visitor.member("counts", value.counts);
        visitor.member("highlight", value.highlight);
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

template<typename T>
std::optional<T> round_trip(const T &value) {
    const auto bytes = encode(value);
    check(validate(bytes).has_value(), "the encoding is well formed");
    return decode<T>(bytes);
}

void macro_form() {
    const auto bytes = encode(point { 3, 4 });
    check_equal(std::string_view { hex(bytes) }, "7b550178550355017955047d", "point encodes as {x:3,y:4}");

    const auto back = round_trip(point { 3, 4 });
    check(back.has_value(), "point round-trips");
    check_equal(back->x, 3, "point.x");
    check_equal(back->y, 4, "point.y");
}

void convert_form() {
    const segment original { { 1, 2 }, { 3, 4 }, "edge" };
    const auto back = round_trip(original);
    check(back.has_value(), "segment round-trips");
    check_equal(back->start.x, 1, "nested start.x");
    check_equal(back->end.y, 4, "nested end.y");
    check_equal(back->label, std::string { "edge" }, "nested label");
}

void separate_form() {
    const connection engaged { "example.com", 8080, mode::on };
    const auto back = round_trip(engaged);
    check(back.has_value(), "connection round-trips");
    check_equal(back->host, std::string { "example.com" }, "host");
    check(back->port.has_value() && *back->port == 8080, "port");
    check_equal(std::to_underlying(back->fallback), std::to_underlying(mode::on), "enum member");

    // An absent optional is omitted from the document, not written as null.
    const connection absent { "h", std::nullopt, mode::off };
    const auto bytes = encode(absent);
    check(hex(bytes).find("706f7274") == std::string::npos, "an absent optional writes no key at all");
    const auto restored = decode<connection>(bytes);
    check(restored.has_value() && !restored->port.has_value(), "an absent key reads back as nullopt");
}

void non_intrusive_form() {
    const auto back = round_trip(extent { 640, 480 });
    check(back.has_value(), "extent round-trips");
    check_equal(back->width, 640, "extent.width");
    check_equal(back->height, 480, "extent.height");
}

void containers_and_nesting() {
    document original;
    original.points = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    original.counts = { { "a", 1 }, { "b", 2 } };
    original.highlight = segment { { 0, 0 }, { 9, 9 }, "hot" };

    const auto back = round_trip(original);
    check(back.has_value(), "document round-trips");
    check_equal(back->points.size(), std::size_t { 3 }, "vector member size");
    check_equal(back->points.at(2).y, 6, "vector member contents");
    check_equal(back->counts.size(), std::size_t { 2 }, "map member size");
    check_equal(back->counts.at("b"), 2, "map member contents");
    check(back->highlight.has_value() && back->highlight->label == "hot", "optional struct member");

    document empty;
    const auto blank = round_trip(empty);
    check(blank.has_value() && blank->points.empty(), "an empty document round-trips");
    check(blank.has_value() && !blank->highlight.has_value(), "an absent optional struct stays absent");
}

void key_order_and_absence() {
    // The read cursor takes the fast path on declaration order, but must still be correct
    // when the document disagrees.
    std::vector<std::byte> reordered;
    container_sink out { reordered };
    writer target { out };
    {
        const auto scope = target.object();
        scope.member("y", 20);
        scope.member("x", 10);
    }
    check(target.finish().has_value(), "hand-built reordered object");

    const auto back = decode<point>(reordered);
    check(back.has_value(), "a reordered object still reads");
    check_equal(back->x, 10, "reordered x");
    check_equal(back->y, 20, "reordered y");

    // A missing key leaves the member at its default rather than failing.
    std::vector<std::byte> partial;
    container_sink partial_out { partial };
    writer partial_target { partial_out };
    {
        const auto scope = partial_target.object();
        scope.member("x", 7);
    }
    check(partial_target.finish().has_value(), "hand-built partial object");

    const auto incomplete = decode<point>(partial);
    check(incomplete.has_value(), "a partial object still reads");
    check_equal(incomplete->x, 7, "present member");
    check_equal(incomplete->y, 0, "absent member keeps its default");

    // Extra keys are ignored rather than rejected.
    std::vector<std::byte> extra;
    container_sink extra_out { extra };
    writer extra_target { extra_out };
    {
        const auto scope = extra_target.object();
        scope.member("x", 1);
        scope.member("unexpected", "ignored");
        scope.member("y", 2);
    }
    check(extra_target.finish().has_value(), "hand-built object with an extra key");
    const auto tolerated = decode<point>(extra);
    check(tolerated.has_value() && tolerated->x == 1 && tolerated->y == 2, "an unknown key is ignored");

    // A value that is not an object at all is refused.
    check(!decode<point>(from_hex("5501")).has_value(), "a scalar does not read as a struct");
}

/**
 * The reflected path itself cannot be compiled here, but the key naming it depends on is
 * ordinary constexpr code and is tested in full, so only the binding to std::meta is unproven.
 */
void reflection_seam() {
    using serpent::detail::convert_case;
    const auto converted = [](std::string_view identifier, naming_style style) {
        return std::string { convert_case(identifier, style).view() };
    };

    check_equal(converted("ip_address", naming_style::as_written), std::string { "ip_address" }, "as_written");

    check_equal(converted("deviceType", naming_style::snake_case), std::string { "device_type" }, "camel to snake");
    check_equal(converted("device_type", naming_style::snake_case), std::string { "device_type" }, "snake to snake");
    check_equal(converted("DeviceType", naming_style::snake_case), std::string { "device_type" }, "pascal to snake");

    check_equal(converted("device_type", naming_style::camel_case), std::string { "deviceType" }, "snake to camel");
    check_equal(converted("deviceType", naming_style::camel_case), std::string { "deviceType" }, "camel to camel");
    check_equal(converted("device_type", naming_style::pascal_case), std::string { "DeviceType" }, "snake to pascal");
    check_equal(converted("device_type", naming_style::kebab_case), std::string { "device-type" }, "snake to kebab");
    check_equal(converted("deviceType", naming_style::screaming_snake_case), std::string { "DEVICE_TYPE" },
            "camel to screaming snake");

    // A digit begins a word, so both spellings of a numbered field agree.
    check_equal(converted("value2x", naming_style::snake_case), std::string { "value_2x" }, "digit starts a word");
    check_equal(converted("x", naming_style::snake_case), std::string { "x" }, "single character");
    check_equal(converted("", naming_style::snake_case), std::string { "" }, "empty identifier");

    // Runs as a constant expression, which is the only way it is ever used.
    static_assert(convert_case("deviceType", naming_style::snake_case).view() == std::string_view { "device_type" });
    static_assert(convert_case("device_type", naming_style::camel_case).view() == std::string_view { "deviceType" });

    // The annotation types are nameable in every build, even where they cannot be attached.
    static_assert(sizeof(key) > 0 && sizeof(skip) >= 1 && sizeof(serializable) >= 1);
    static_assert(naming {}.style == naming_style::as_written);

    // Nothing is reflected on this toolchain, so the macro and function forms carry everything.
    check_equal(reflection_available, false, "no reflection on this toolchain");
    static_assert(!reflected_type<point>, "point is not reflected here");
    static_assert(!enable_reflection<point>::value, "reflection is opt-in");

    // Which does not stop any of the supported forms from working.
    check(round_trip(point { 1, 2 }).has_value(), "the macro form still carries the type");
}

/**
 * One definition, two formats, both directions.
 *
 * json_convert never names either reader or either writer, so a type that uses it - which
 * is what SERPENT_DEFINE_TYPE writes - is carried by all four paths without being told about
 * any of them.
 */
void both_formats() {
    const document original {
        .points = { { 1, 2 }, { 3, 4 } },
        .counts = { { "a", 1 }, { "b", 2 } },
        .highlight = segment { { 0, 0 }, { 9, 9 }, "hot" },
    };

    const auto binary = encode(original);
    const auto text = json::encode(original);
    check(validate(binary).has_value(), "the BJData encoding is well formed");
    check(json::validate(text).has_value(), "the JSON encoding is well formed");

    const auto from_binary = decode<document>(binary);
    const auto from_text = json::decode<document>(text);
    check(from_binary.has_value(), "reads back from BJData");
    check(from_text.has_value(), "reads back from JSON");
    if (!from_binary || !from_text) return;

    // The two paths must agree with each other, not merely each with itself.
    check_equal(json::encode(*from_binary), json::encode(*from_text), "both formats decode to the same value");
    check_equal(json::encode(*from_text), text, "and JSON survives a full round trip unchanged");

    check_equal(from_text->points.size(), std::size_t { 2 }, "vector member through JSON");
    check_equal(from_text->points.at(1).y, 4, "nested struct through JSON");
    check_equal(from_text->counts.at("b"), 2, "map member through JSON");
    check(from_text->highlight.has_value() && from_text->highlight->label == "hot",
            "optional struct member through JSON");

    // A key absent from the JSON leaves the member at its default, exactly as for BJData.
    const auto partial = json::decode<point>(R"({"y":5})");
    check(partial.has_value() && partial->x == 0 && partial->y == 5, "an absent key keeps its default");

    // And key order still does not matter.
    const auto reordered = json::decode<point>(R"({ "y" : 2 , "x" : 1 })");
    check(reordered.has_value() && reordered->x == 1 && reordered->y == 2, "reordered JSON keys");

    // Non-intrusive and macro forms carry across too.
    check(json::decode<extent>(R"({"width":640,"height":480})")->width == 640, "non-intrusive form reads JSON");
    check(json::decode<segment>(json::encode(segment { { 1, 1 }, { 2, 2 }, "s" }))->label == "s",
            "the convert form round-trips through JSON");
}

/**
 * A struct is not a string, however constructible from one it happens to be.
 *
 * C++20 parenthesized aggregate initialization makes std::constructible_from<T, std::string>
 * true for any aggregate whose members can take one, so a type whose first member is a string
 * once looked like a string to the reader and failed to decode.
 */
void aggregates_are_not_strings() {
    static_assert(std::constructible_from<labelled, std::string>, "the trap this guards against");
    static_assert(!serpent::detail::string_like<labelled>, "but it is not a string");

    const labelled original { "north ridge" };
    const auto from_binary = decode<labelled>(encode(original));
    const auto from_text = json::decode<labelled>(json::encode(original));
    check(from_binary.has_value() && from_binary->text == original.text, "a lone string member reads from BJData");
    check(from_text.has_value() && from_text->text == original.text, "and from JSON");

    const first_string_member mixed { "n", 3 };
    const auto mixed_text = json::decode<first_string_member>(json::encode(mixed));
    check(mixed_text.has_value() && mixed_text->name == "n" && mixed_text->count == 3,
            "a string first member does not swallow the object");
}

/**
 * A container needs no customization, so decoding straight into one must work.
 *
 * encode() has always accepted these; decode() used to look for a json_convert that a
 * std::vector was never going to have, and refuse to compile.
 */
void containers_need_no_customization() {
    const std::vector<std::uint16_t> numbers { 900, 901, 902 };
    check(decode<std::vector<std::uint16_t>>(encode(numbers)) == numbers, "vector through BJData");
    check(json::decode<std::vector<std::uint16_t>>(json::encode(numbers)) == numbers, "vector through JSON");

    const std::map<std::string, int> keyed { { "a", 1 }, { "b", 2 } };
    check(decode<std::map<std::string, int>>(encode(keyed)) == keyed, "map through BJData");
    check(json::decode<std::map<std::string, int>>(json::encode(keyed)) == keyed, "map through JSON");

    const std::vector<point> structs { { 1, 2 }, { 3, 4 } };
    const auto binary_structs = decode<std::vector<point>>(encode(structs));
    const auto text_structs = json::decode<std::vector<point>>(json::encode(structs));
    check(binary_structs && binary_structs->size() == 2 && binary_structs->at(1).y == 4,
            "a container of custom types through BJData");
    check(text_structs && text_structs->size() == 2 && text_structs->at(1).y == 4, "and through JSON");

    // Binary is a range of bytes in BJData and an array of integers in JSON, both ways.
    const std::vector<std::byte> raw { std::byte { 0xde }, std::byte { 0xad } };
    check(decode<std::vector<std::byte>>(encode(raw)) == raw, "bytes through BJData");
    check(json::decode<std::vector<std::byte>>(json::encode(raw)) == raw, "bytes through JSON");

    const std::optional<int> engaged { 7 };
    check(decode<std::optional<int>>(encode(engaged)) == engaged, "optional through BJData");
    check(json::decode<std::optional<int>>(json::encode(engaged)) == engaged, "optional through JSON");
}

/**
 * A counted array lets the reader size its container once instead of growing it.
 *
 * Off by default, because the reference encoder only writes a count beside a type marker, and
 * the fixture suite holds the default output to that byte for byte.
 */
void counted_containers() {
    constexpr serpent::bjdata::writer_options counted { .counted_containers = true };

    const std::vector<point> points { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    const auto unbounded = encode(points);
    const auto with_count = encode<counted>(points);

    check(with_count.size() > unbounded.size(), "the count costs a few bytes");
    check(hex(unbounded).find("5b7b") == 0, "the default is still an unbounded array of objects");
    check(hex(with_count).find("5b23") == 0, "the counted form writes [# before the elements");

    const auto back = decode<std::vector<point>>(with_count);
    check(back && back->size() == 3 && back->at(2).y == 6, "and it round-trips");

    // The point of the count: it is readable without walking the elements.
    const auto counted_view = view::over(with_count);
    const auto hint = counted_view.size_hint();
    check(hint && *hint == 3, "a counted array states its length");
    check(!view::over(unbounded).size_hint(), "an unbounded one does not, rather than counting");
    check(counted_view.size() == 3, "and size() agrees either way");
    check(view::over(unbounded).size() == 3, "including when it has to walk");

    // A typed array already carried a count, so it needs no option to be sized on read.
    const auto typed = encode(std::vector<std::uint16_t> { 900, 901, 902, 903, 904 });
    const auto typed_hint = view::over(typed).size_hint();
    check(typed_hint && *typed_hint == 5, "a packed numeric array states its length already");
}

void sizing() {
    const point value { 3, 4 };
    check_equal(measure(value), encode(value).size(), "measure agrees with encode_TMP");
    check_equal(measure(document {}), encode(document {}).size(), "measure agrees for a nested type");
}

} // namespace

int main() {
    macro_form();
    convert_form();
    separate_form();
    non_intrusive_form();
    containers_and_nesting();
    key_order_and_absence();
    reflection_seam();
    aggregates_are_not_strings();
    containers_need_no_customization();
    counted_containers();
    both_formats();
    sizing();
    return report("serializer");
}
