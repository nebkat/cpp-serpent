// The three ways a user type opts in, and that all of them round-trip.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;
namespace json = serpent::json;

namespace {

enum class mode : std::uint8_t { off, automatic, on };

/** Aggregates that C++20 would let you build straight from a std::string. */
struct[[= serpent::serializable {}]] labelled {
    std::string text;
};

struct[[= serpent::serializable {}]] first_string_member {
    std::string name;
    int count = 0;
};

/** The same members as point, one of which the document may leave out. */
struct[[= serpent::serializable {}]] lenient {
    int x = 0;
    [[= serpent::defaulted {}]] int y = 0;
};

/** An optional is absent-tolerant; saying required makes the key mandatory, the value still not. */
struct[[= serpent::serializable {}]] with_optional {
    std::string name;
    std::optional<int> note;
};
struct[[= serpent::serializable {}]] with_insisted_optional {
    std::string name;
    [[= serpent::required {}]] std::optional<int> note;
};

// --- Form C: the macro, for a plain aggregate ---
struct[[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
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

void annotated_form() {
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

/**
 * A type's members written into an object the caller opened, plus fields only it knows.
 *
 * The document has to stay flat: writing the value as a member would nest it, and a nested
 * document is a different document.
 */
void members_into_an_open_object() {
    std::vector<std::byte> bytes;
    container_sink out { bytes };
    writer target { out };
    {
        const auto object = target.object();
        serpent::write_members(target, point { 3, 4 });
        object.member("label", "corner");
    }
    check(target.finish().has_value(), "flattened object");

    const auto document = view::over(bytes);
    check_equal(document.size(), std::size_t { 3 }, "the type's members and the caller's, side by side");
    check_equal(document["x"].as_int<int>().value_or(0), 3, "a member of the type");
    check_equal(document["label"].as_string().value_or("?"), "corner", "and one only the caller knew");

    // The inverse: the type reads its own members and ignores the rest.
    point recovered {};
    check(serpent::read_members(document, recovered), "the type reads back out of the wider object");
    check(recovered.x == 3 && recovered.y == 4, "unchanged");

    // A scope can be named without spelling out an options set the caller never chose.
    const auto write_through_scope = [](const object_scope<> &scope) { scope.member("via", "scope"); };
    std::vector<std::byte> second;
    container_sink second_out { second };
    writer second_target { second_out };
    {
        const auto object = second_target.object();
        write_through_scope(object);
    }
    check(second_target.finish().has_value(), "object_scope<> names itself");
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

    // A member the type insists on may not be left out.
    std::vector<std::byte> partial;
    container_sink partial_out { partial };
    writer partial_target { partial_out };
    {
        const auto scope = partial_target.object();
        scope.member("x", 7);
    }
    check(partial_target.finish().has_value(), "hand-built partial object");

    check(!decode<point>(partial), "a partial object does not read");

    // And says which member it wanted, in both formats. The generated reader knows only that
    // one was missing, so naming it is a second walk taken on the way to the error.
    const auto named = try_decode<point>(partial);
    check(!named && named.error().code() == errc::missing_key && named.error().key() == "y",
            "the failure names the member the document left out");
    const auto from_text = json::try_decode<point>(R"({"x":7})");
    check(!from_text && from_text.error().code() == errc::missing_key && from_text.error().key() == "y",
            "and the same reading text");

    // Unless it says otherwise. lenient declares the same two members, one of which the
    // document may leave out, and then the default is what was asked for rather than a
    // half-specified value passing for a whole one.
    const auto forgiving = decode<lenient>(partial);
    check(forgiving.has_value(), "a type that allows an absent member reads the same document");
    check_equal(forgiving->x, 7, "present member");
    check_equal(forgiving->y, 0, "and the absent one is the default it declared");

    // An optional is absent-tolerant already, since absence is what it represents.
    std::vector<std::byte> without;
    container_sink without_out { without };
    writer without_target { without_out };
    {
        const auto scope = without_target.object();
        scope.member("name", "n");
    }
    check(without_target.finish().has_value(), "hand-built object with no optional");
    const auto maybe = decode<with_optional>(without);
    check(maybe && !maybe->note.has_value(), "an absent optional is nothing, not an error");
    check(!decode<with_insisted_optional>(without), "unless the type insists the key be stated");
    const auto insisted = try_decode<with_insisted_optional>(without);
    check(insisted.error().code() == errc::missing_key && insisted.error().key() == "note",
            "an insisted-on optional names itself too");

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

    // Reflection is opt-in: a type that says nothing is not reflected, however plain it is.
    struct unannotated {
        int x;
    };
    static_assert(!reflected_type<unannotated>, "an aggregate is not reflected merely for being one");
    static_assert(!enable_reflection<unannotated>::value, "and the trait says no until told otherwise");

    // A type that does say so carries both formats through the annotation alone.
    static_assert(reflected_type<point>, "an annotated type is reflected");
    check(round_trip(point { 1, 2 }).has_value(), "and round-trips with nothing else written");
}

/**
 * One definition, two formats, both directions.
 *
 * json_convert never names either reader or either writer, so a type that uses it - which
 * is what the annotation generates - is carried by every path without being told about
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

    // A member the type needs may not be left out, in either format.
    check(!json::decode<point>(R"({"y":5})"), "an absent member is an error in JSON");
    check(!decode<point>(encode(first_string_member {})), "and in BJData");

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
 * A sized array states its length, so the reader sizes its container once instead of growing it.
 *
 * On by default from three elements up, where the two bytes it costs buy two allocations. Below
 * that it buys less than it costs, and reference_parity turns it off entirely.
 */
void counted_containers() {
    const std::vector<point> two { { 1, 2 }, { 3, 4 } };
    const std::vector<point> three { { 1, 2 }, { 3, 4 }, { 5, 6 } };

    check(hex(encode(two)).find("5b7b") == 0, "below the threshold an array stays unbounded");
    check(hex(encode(three)).find("5b23") == 0, "at three elements it states its length");
    check(hex(encode<reference_parity>(three)).find("5b7b") == 0, "reference_parity never states one");
    check(encode(three).size() == encode<reference_parity>(three).size() + 2, "which costs two bytes");

    const auto back = decode<std::vector<point>>(encode(three));
    check(back && back->size() == 3 && back->at(2).y == 6, "and it round-trips");

    // The point of the count: it is readable without walking the elements.
    const auto hint = view::over(encode(three)).size_hint();
    check(hint && *hint == 3, "a counted array states its length");
    check(!view::over(encode<reference_parity>(three)).size_hint(), "an unbounded one does not, rather than counting");
    check(view::over(encode<reference_parity>(three)).size() == 3, "though size() will still walk it");

    // A packed numeric array already carried a count, whatever the threshold.
    const auto typed = encode<reference_parity>(std::vector<std::uint16_t> { 900, 901, 902, 903, 904 });
    const auto typed_hint = view::over(typed).size_hint();
    check(typed_hint && *typed_hint == 5, "a packed numeric array states its length already");
}

struct[[= serpent::serializable {}]] circle {
    int radius = 0;
};

/**
 * A sum type is written as whichever alternative it holds and recovered by asking which one
 * the value fits. Nothing tags it: the value already says what it is.
 */
void variants() {
    using scalar = std::variant<bool, std::int64_t, double, std::string>;

    check_equal(json::encode(scalar { true }), "true", "a variant writes the alternative it holds");
    check_equal(json::encode(scalar { std::int64_t { 42 } }), "42", "and nothing else");
    check_equal(json::encode(scalar { std::string { "hi" } }), "\"hi\"", "including a string");

    check(json::decode<scalar>("true")->index() == 0, "a boolean comes back as the boolean");
    check(json::decode<scalar>("42")->index() == 1, "an integer as the integer");
    check(json::decode<scalar>("2.5")->index() == 2, "a real as the real");
    check(json::decode<scalar>("\"hi\"")->index() == 3, "a string as the string");

    const scalar held { 2.5 };
    check(decode<scalar>(encode(held)) == held, "and it round-trips through BJData too");

    // monostate is null, which is what makes variant<monostate, T> the optional-shaped case.
    using maybe = std::variant<std::monostate, int>;
    check_equal(json::encode(maybe {}), "null", "monostate writes null");
    check(json::decode<maybe>("null")->index() == 0, "and reads back as itself");
    check(json::decode<maybe>("7")->index() == 1, "without swallowing the other alternative");

    // Two alternatives of the same shape are told apart by which keys the document names. An
    // object that names none of a type's members is not that type, even though decoding one
    // outside a variant would succeed and leave every member at its default.
    using shape = std::variant<point, circle>;
    check_equal(json::encode(shape { circle { 9 } }), "{\"radius\":9}", "a struct alternative writes itself");
    const auto round_tripped = json::decode<shape>("{\"radius\":9}");
    check(round_tripped && round_tripped->index() == 1, "and is recognised rather than matching the first");
    check(json::decode<shape>("{\"x\":1,\"y\":2}")->index() == 0, "the other way round as well");

    // Where two alternatives genuinely both fit, declaration order decides.
    check(json::decode<std::variant<double, std::int64_t>>("4")->index() == 0,
            "an integer fits a real, so a real declared first takes it");
    check(json::decode<std::variant<std::int64_t, double>>("4")->index() == 0,
            "and the integer takes it when declared first");

    check(!json::decode<std::variant<bool, point>>("[1,2]"), "an array matches neither and fails");
}

void sizing() {
    const point value { 3, 4 };
    check_equal(measure(value), encode(value).size(), "measure agrees with encode_TMP");
    check_equal(measure(document {}), encode(document {}).size(), "measure agrees for a nested type");
}

} // namespace

int main() {
    annotated_form();
    convert_form();
    separate_form();
    non_intrusive_form();
    containers_and_nesting();
    members_into_an_open_object();
    key_order_and_absence();
    reflection_seam();
    aggregates_are_not_strings();
    containers_need_no_customization();
    counted_containers();
    variants();
    both_formats();
    sizing();
    return report("serializer");
}
