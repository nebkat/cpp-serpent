// The reflected path: the compiler names the fields, annotations adjust the keys.
//
// Only built where the compiler supports reflection. Everything here has a hand-written
// counterpart in serializer_test.cpp, so the two forms are held to the same behaviour.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/bjdata/json.hpp>
#include <serpent/json.hpp>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

static_assert(serpent::reflection_available, "this test is only built where reflection works");

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;

// Field identifiers become the keys, unchanged.
struct[[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};

// A naming rule on the type rewrites every key; a field can still override its own, and
// skipped fields never reach the wire in either direction.
struct[[ = serpent::serializable {}, = serpent::naming { serpent::naming_style::snake_case } ]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress = "0.0.0.0";
    [[= serpent::skip {}]] int cacheGeneration = 0;
    int mtuBytes = 1500;
    std::optional<int> vlanId {};
};

// Nesting, containers and a type opted in without touching its definition.
struct site {
    point origin {};
    std::vector<point> corners {};
    std::map<std::string, int> counters {};
};
template<>
struct serpent::describe<site> {};

// Hand-written and never annotated: reflection must keep out of its way entirely.
struct hand_written {
    int x = 1;
    int y = 2;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), hand_written> value) {
        visitor.member("only_x", value.x);
    }
};

// A type from elsewhere: never annotated, opted in and given a naming rule from outside.
struct foreign_reading {
    int sensorId = 4;
    double degreesCelsius = 21.5;
};
template<>
struct serpent::describe<foreign_reading> {
    static constexpr serpent::naming naming { serpent::naming_style::snake_case };
};

// A type whose conversion is supplied wholesale, which must also outrank reflection.
struct[[= serpent::serializable {}]] specialized {
    int value = 5;
};
template<>
struct serpent::serializer<specialized, void> {
    template<typename Writer>
    static void write(Writer &out, const specialized &object) {
        out.value(object.value);
    }
    template<typename Source>
    static bool read(Source source, specialized &object) {
        const auto number = source.template try_get<int>();
        if (!number) return false;
        object.value = *number;
        return true;
    }
};

void a_hand_written_conversion_is_left_alone() {
    check(json::encode(hand_written {}) == R"({"only_x":1})", "an unannotated type uses its json_convert");
    const auto back = json::decode<hand_written>(R"({"only_x":9})");
    check(back && back->x == 9 && back->y == 2, "in both directions, so y keeps its default");

    // Annotating this type as well would be a compile error - see reflect_conflict_test.cpp.
    check(json::encode(specialized {}) == "5", "a serializer specialization replaces reflection");
    const auto scalar = json::decode<specialized>("7");
    check(scalar && scalar->value == 7, "and is not diagnosed, because it replaces the dispatch");
}

void a_type_you_do_not_own() {
    check(json::encode(foreign_reading {}) == R"({"sensor_id":4,"degrees_celsius":21.5})",
            "describe opts it in and carries the naming rule");
    const auto back = json::decode<foreign_reading>(R"({"sensor_id":9,"degrees_celsius":1.5})");
    check(back && back->sensorId == 9 && back->degreesCelsius == 1.5, "and it reads back");
}

// Same members, different meaning. Nothing but a name can tell these apart, so trying each in
// turn and keeping the first that parses would always answer with the first.
struct[[= serpent::discriminant("unit")]] celsius {
    double value = 0;
};
struct[[= serpent::discriminant("unit")]] fahrenheit {
    double value = 0;
};
struct[[= serpent::discriminant("unit", "K")]] kelvin {
    double value = 0;
};

using temperature = std::variant<celsius, fahrenheit, kelvin>;

void a_discriminant_names_the_type() {
    check_equal(json::encode(celsius { 21.5 }), R"({"unit":"celsius","value":21.5})",
            "the name is written before the members");
    check_equal(json::encode(kelvin { 294.6 }), R"({"unit":"K","value":294.6})",
            "an explicit name overrides the identifier");

    // It is not a member: reading one on its own ignores it, and nothing declares it.
    const auto alone = json::decode<celsius>(R"({"unit":"celsius","value":21.5})");
    check(alone && alone->value == 21.5, "a discriminated type reads back on its own");

    check(json::decode<temperature>(R"({"unit":"celsius","value":21.5})")->index() == 0,
            "an alternative is chosen by what the document calls it");
    check(json::decode<temperature>(R"({"unit":"fahrenheit","value":70.7})")->index() == 1,
            "including one it could not be told apart from otherwise");
    check(json::decode<temperature>(R"({"unit":"K","value":294.6})")->index() == 2, "and one renamed");

    check(!json::decode<temperature>(R"({"unit":"rankine","value":1})"),
            "a name that matches nothing fails rather than guessing");
    check(!json::decode<temperature>(R"({"value":1})"), "and so does no name at all");

    const temperature held { fahrenheit { 70.7 } };
    const auto binary = bjdata::decode<temperature>(bjdata::encode(held));
    check(binary && binary->index() == 1, "the same through BJData");
}

// Plain structs, never annotated: the sort of thing that arrives from a header you do not own.
struct plain_point {
    int x = 0;
    int y = 0;
};
struct plain_circle {
    int radius = 0;
};

struct[[= serpent::serializable {}]] drawing {
    std::string title;
    [[= serpent::tagged("kind")]] std::variant<plain_point, plain_circle> body;
};

struct[[= serpent::serializable {}]] renamed {
    [[= serpent::tagged("k", { "pt", "circ" })]] std::variant<plain_point, plain_circle> body;
};

void a_field_can_carry_the_tag() {
    const drawing shape { "a", plain_circle { 9 } };
    check_equal(json::encode(shape), R"({"title":"a","body":{"kind":"plain_circle","radius":9}})",
            "the tag names the alternative inside the field's own object");

    const auto back = json::decode<drawing>(json::encode(shape));
    check(back && back->body.index() == 1, "and the name selects it on the way back");
    check(back && std::get<plain_circle>(back->body).radius == 9, "with its members intact");

    // Neither alternative opted in to reflection; naming them on the field is that opt-in.
    const auto by_identifier = json::decode<drawing>(R"({"title":"b","body":{"kind":"plain_point","x":1,"y":2}})");
    check(by_identifier && by_identifier->body.index() == 0, "an alternative named by its own identifier");
    check(by_identifier && std::get<plain_point>(by_identifier->body).y == 2, "and read in full");

    check_equal(json::encode(renamed { plain_point { 3, 4 } }), R"({"body":{"k":"pt","x":3,"y":4}})",
            "an explicit list overrides the identifiers");
    const auto short_form = json::decode<renamed>(R"({"body":{"k":"circ","radius":5}})");
    check(short_form && short_form->body.index() == 1, "and reads back by those names");

    check(!json::decode<renamed>(R"({"body":{"k":"nope","radius":5}})"),
            "a name matching no alternative fails rather than guessing");

    const drawing binary_source { "b", plain_point { 3, 4 } };
    const auto binary = bjdata::decode<drawing>(bjdata::encode(binary_source));
    check(binary && binary->body.index() == 0, "the same through BJData");
}

// A sum type where only some alternatives are objects. The scalar cannot carry a tag and does
// not need one: a number already says what it is.
struct[[= serpent::serializable {}]] mixed_holder {
    [[= serpent::tagged("kind")]] std::variant<int, plain_point, plain_circle> body;
};

using mixed_temperature = std::variant<int, celsius, fahrenheit>;

void a_variant_can_mix_shapes() {
    check_equal(json::encode(mixed_holder { 42 }), R"({"body":42})", "a scalar alternative is written bare");
    check_equal(json::encode(mixed_holder { plain_circle { 9 } }), R"({"body":{"kind":"plain_circle","radius":9}})",
            "an object alternative still carries the name");

    check(json::decode<mixed_holder>(R"({"body":42})")->body.index() == 0, "a number reads as the number");
    check(json::decode<mixed_holder>(R"({"body":{"kind":"plain_point","x":1,"y":2}})")->body.index() == 1,
            "and a named object as that object");
    check(!json::decode<mixed_holder>(R"({"body":{"kind":"nope"}})"),
            "a name matching nothing still fails rather than falling through to the scalar");

    // The same on the type-level path: the discriminant is used for the objects that carry one,
    // and the alternative that cannot is recovered as itself.
    check_equal(json::encode(mixed_temperature { 5 }), "5", "a scalar beside discriminated types");
    check_equal(json::encode(mixed_temperature { fahrenheit { 70.7 } }), R"({"unit":"fahrenheit","value":70.7})",
            "which does not stop the others being named");
    check(json::decode<mixed_temperature>("5")->index() == 0, "the scalar reads back");
    check(json::decode<mixed_temperature>(R"({"unit":"fahrenheit","value":70.7})")->index() == 2,
            "and the named object is still told from its twin");
}

/**
 * BJData reads a reflected type through a reader generated for it rather than the member
 * iterator, so everything the generic walk handles has to hold there too.
 */
void the_generated_reader_agrees_with_the_generic_one() {
    // A key the type does not name is skipped, whatever it holds.
    const auto extra = json::decode<point>(R"({"x":1,"note":{"nested":[1,2,3]},"y":2})");
    check(extra && extra->x == 1 && extra->y == 2, "an unknown key is skipped, including a whole subtree");
    const auto binary_extra = bjdata::decode<point>(bjdata::encode(extra.value()));
    check(binary_extra && binary_extra->y == 2, "and the same through BJData");

    // A key written the way we write it is recognised whole, without being parsed. One written
    // any other legal way has to still read, which is what the parsing fallback is for: here the
    // lengths use int8 markers rather than the uint8 we would emit.
    const std::uint8_t foreign[] = { '{', 'i', 1, 'x', 'U', 4, 'i', 1, 'y', 'U', 6, '}' };
    const auto other_encoder = bjdata::decode<point>(std::as_bytes(std::span { foreign }));
    check(other_encoder && other_encoder->x == 4 && other_encoder->y == 6,
            "a key whose length marker differs from ours still reads");

    // Nothing obliges an encoder to spell a length in the fewest bytes. Here 1 is written as an
    // int16, which no constant of ours can match, so it has to go through the parsing path.
    const std::uint8_t wide_length[] = { '{', 'I', 1, 0, 'x', 'U', 4, 'I', 1, 0, 'y', 'U', 6, '}' };
    const auto padded = bjdata::decode<point>(std::as_bytes(std::span { wide_length }));
    check(padded && padded->x == 4 && padded->y == 6, "a length written wider than it needs to be still reads");

    // Mixed in one document: the first key as we write it, the second as someone else would.
    const std::uint8_t mixed[] = { '{', 'U', 1, 'x', 'U', 1, 'i', 1, 'y', 'U', 2, '}' };
    const auto both_ways = bjdata::decode<point>(std::as_bytes(std::span { mixed }));
    check(both_ways && both_ways->x == 1 && both_ways->y == 2, "and the two forms mix in one object");

    // A strongly typed object: the members share one marker, so the values carry none.
    const std::uint8_t typed[] = { '{', '$', 'U', '#', 'U', 2, 'U', 1, 'x', 7, 'U', 1, 'y', 9 };
    const auto strong = bjdata::decode<point>(std::as_bytes(std::span { typed }));
    check(strong && strong->x == 7 && strong->y == 9, "a strongly typed object reads");

    // Reordered and partial, against the generated reader specifically.
    check(bjdata::view::over(bjdata::encode(point { 1, 2 })).is_object(), "an object is what we wrote");
    const auto reordered = json::decode<point>(R"({"y":20,"x":10})");
    check(reordered && reordered->x == 10 && reordered->y == 20, "declaration order is not required");
    check(!json::decode<point>(R"({"x":7})"), "a member the type needs may not be left out");
}

/** The same keys as point, named by hand, so it takes the general path. */
struct by_hand {
    int x = 0;
    int y = 0;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), by_hand> value) {
        visitor.member("x", value.x);
        visitor.member("y", value.y);
    }
};

// A key the compiler knows is framed once at compile time. Where that framing would be wrong,
// the general path has to take over, and this is what says so.
struct[[= serpent::serializable {}]] awkward {
    [[= serpent::key("a\"b")]] int quoted = 1;
    [[= serpent::key("tab\there")]] int control = 2;
    int plain = 3;
};

void a_constant_key_is_framed_once() {
    // by_hand is declared at namespace scope below; it names the same keys the general way.
    // Escapes: the constant path cannot pre-frame these, so it defers and they come out escaped.
    check_equal(json::encode(awkward {}), R"({"a\"b":1,"tab\there":2,"plain":3})",
            "a key needing escapes is still escaped");
    const auto back = json::decode<awkward>(json::encode(awkward { 7, 8, 9 }));
    check(back && back->quoted == 7 && back->control == 8 && back->plain == 9, "and reads back");

    // Indenting: the framing has no room for the spacing, so it defers there too.
    check_equal(json::encode(point { 1, 2 }, { .indent = 2 }), "{\n  \"x\": 1,\n  \"y\": 2\n}",
            "indented output is unchanged");

    // BJData frames the length marker, the length and the bytes together. The bytes it produces
    // have to be exactly what the general path produces, so compare against a type that names
    // the same keys by hand and therefore takes that path.
    check(bjdata::encode(point { 3, 4 }) == bjdata::encode(by_hand { 3, 4 }),
            "a framed key writes the same bytes as one written out");
    check_equal(json::encode(point { 3, 4 }), json::encode(by_hand { 3, 4 }), "and the same JSON");
}

/**
 * A sequence of reflected objects is read by a hook that walks the array once. It has to agree
 * with the generic path on every shape an array can take, including ones we never write.
 */
void a_sequence_is_walked_once() {
    const std::vector<point> many { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    const auto counted = bjdata::encode<bjdata::prefer::speed>(many);
    const auto back = bjdata::decode<std::vector<point>>(counted);
    check(back && back->size() == 3 && back->at(2).y == 6, "a counted array of objects");

    // Unbounded: what is written where size is preferred, and what another implementation may send.
    const auto unbounded = bjdata::encode<bjdata::prefer::size>(many);
    const auto walked = bjdata::decode<std::vector<point>>(unbounded);
    check(walked && walked->size() == 3 && walked->at(1).x == 3, "an unbounded array of objects");

    check(bjdata::decode<std::vector<point>>(bjdata::encode(std::vector<point> {}))->empty(), "an empty array");

    // Noops may appear between elements and must be stepped over.
    const std::uint8_t noops[] = { '[', 'N', '{', 'U', 1, 'x', 'U', 7, 'U', 1, 'y', 'U', 8, '}', 'N', ']' };
    const auto stepped = bjdata::decode<std::vector<point>>(std::as_bytes(std::span { noops }));
    check(stepped && stepped->size() == 1 && stepped->at(0).x == 7, "noops between elements");

    // An element that is not an object is a failure, not a silent empty.
    const std::uint8_t wrong[] = { '[', '#', 'U', 1, 'U', 5 };
    check(!bjdata::decode<std::vector<point>>(std::as_bytes(std::span { wrong })),
            "an array of numbers is not an array of objects");

    // Truncation must not be read as a short but valid array.
    const std::uint8_t cut[] = { '[', '#', 'U', 2, '{', 'U', 1, 'x', 'U', 7, '}' };
    check(!bjdata::decode<std::vector<point>>(std::as_bytes(std::span { cut })),
            "a count promising more than is there fails");

    // Nesting: the element type is itself read by the generated reader.
    const std::vector<std::vector<point>> nested { { { 1, 2 } }, { { 3, 4 }, { 5, 6 } } };
    const auto deep = bjdata::decode<std::vector<std::vector<point>>>(bjdata::encode(nested));
    check(deep && deep->size() == 2 && deep->at(1).size() == 2 && deep->at(1).at(1).y == 6, "a sequence of sequences");

    // And it must agree with the generic reader, which is what json::reader still uses.
    const auto through_json = json::decode<std::vector<point>>(json::encode(many));
    check(through_json && through_json->size() == 3 && through_json->at(2).x == 5, "the generic path agrees");
}

// An enumeration that says what its values are on the wire. Everything here is drawn from a
// real mapping table: names that are not the identifier, the same spelling meaning different
// things in two enumerations, numbers that are not the underlying values, an integer and a real
// in one table, null, and a designated fallback.
enum class[[= serpent::serializable {}]] fix_dimension {
    none[[ = serpent::fallback {}, = serpent::as(nullptr) ]],
    two_dimensional[[= serpent::as("2d")]],
    three_dimensional[[= serpent::as("3d")]],
};
enum class[[= serpent::serializable {}]] rtk_fix_type {
    none,
    rtk_float[[= serpent::as("float")]],
};
enum class[[= serpent::serializable {}]] fix_type { none, rtk_float };
enum class[[= serpent::serializable {}]] stop_bits {
    one[[= serpent::as(1)]],
    one_and_half[[= serpent::as(1.5)]],
    two[[= serpent::as(2)]],
};
enum class[[= serpent::serializable {}]] nav_system { unknown[[= serpent::fallback {}]], gps };
enum class[[ = serpent::serializable {}, = serpent::naming { serpent::naming_style::kebab_case } ]] link_state {
    notConnected,
};
enum class plain_enum { first, second };

template<typename E>
void round_trips(E value, std::string_view expected, std::string_view what) {
    check_equal(json::encode(value), std::string { expected }, what);
    const auto back = json::decode<E>(json::encode(value));
    check(back && *back == value, "and reads back");
    const auto binary = bjdata::decode<E>(bjdata::encode(value));
    check(binary && *binary == value, "in both formats");
}

void an_enum_can_say_what_it_is_on_the_wire() {
    round_trips(fix_dimension::two_dimensional, R"("2d")", "a name that is not the identifier");
    round_trips(fix_dimension::none, "null", "an enumerator that is null");
    round_trips(rtk_fix_type::rtk_float, R"("float")", "one spelling in one enumeration");
    round_trips(fix_type::rtk_float, R"("rtk_float")", "and the same spelling in another");
    round_trips(stop_bits::one, "1", "a whole number");
    round_trips(stop_bits::one_and_half, "1.5", "and a real beside it in the same table");
    round_trips(link_state::notConnected, R"("not-connected")", "the type's naming rule applies");
    round_trips(nav_system::gps, R"("gps")", "an unannotated enumerator is its identifier");

    // The fallback, in both directions.
    check(json::decode<nav_system>(R"("galileo")") == nav_system::unknown,
            "a value matching no enumerator reads as the fallback");
    check_equal(json::encode(static_cast<fix_dimension>(99)), "null", "and a value that is no enumerator writes as it");
    check(!json::decode<stop_bits>("7"), "with no fallback declared, an unknown value fails");

    // An enumeration that says nothing keeps going out as its number, and now reads back too.
    check_equal(json::encode(plain_enum::second), "1", "an unannotated enumeration is a number");
    check(json::decode<plain_enum>("1") == plain_enum::second, "which now reads back as well");
}

void identifiers_become_keys() {
    const point p { 3, 4 };
    check(json::encode(p) == R"({"x":3,"y":4})", "the identifiers are the keys");
    const auto back = json::decode<point>(json::encode(p));
    check(back && back->x == 3 && back->y == 4, "and read back");
}

void annotations_adjust_keys() {
    const link_config config { .ipAddress = "10.0.0.4", .cacheGeneration = 7, .mtuBytes = 9000, .vlanId = 12 };
    check(json::encode(config) == R"({"ip":"10.0.0.4","mtu_bytes":9000,"vlan_id":12})",
            "key() overrides, naming rewrites, skip omits");

    const auto back = json::decode<link_config>(json::encode(config));
    check(back && back->ipAddress == "10.0.0.4" && back->mtuBytes == 9000, "the named fields come back");
    check(back && back->vlanId == 12, "including the optional");
    check(back && back->cacheGeneration == 0, "a skipped field keeps its default rather than the written value");
}

void an_absent_optional_is_still_written_null() {
    const link_config config {};
    const auto text = json::encode(config);
    check(text.find("\"vlan_id\":null") != std::string::npos, "an empty optional writes null");
    const auto back = json::decode<link_config>(text);
    check(back && !back->vlanId.has_value(), "and reads back empty");
}

void reflection_composes_with_containers() {
    const site value {
        .origin = { 1, 2 },
        .corners = { { 3, 4 }, { 5, 6 } },
        .counters = { { "a", 1 } },
    };
    check(json::encode(value)
                    == R"({"origin":{"x":1,"y":2},"corners":[{"x":3,"y":4},{"x":5,"y":6}],"counters":{"a":1}})",
            "nested reflected types, a vector of them, and a map");

    const auto back = json::decode<site>(json::encode(value));
    check(back && back->origin.y == 2 && back->corners.size() == 2 && back->corners[1].x == 5,
            "the whole shape reads back");
    check(back && back->counters.at("a") == 1, "including the map");
}

void the_same_type_serves_both_formats() {
    // One annotated definition, no per-format customization anywhere.
    const link_config config { .ipAddress = "192.168.1.1", .cacheGeneration = 3, .mtuBytes = 1500, .vlanId = {} };
    const auto bytes = bjdata::encode(config);
    const auto back = bjdata::decode<link_config>(bytes);
    check(back && back->ipAddress == "192.168.1.1" && back->mtuBytes == 1500, "BJData round-trips it too");
    check(back && back->cacheGeneration == 0, "and honours skip");

    // The keys are the same on both wires, so a document written as one reads as the other.
    check(json::encode(bjdata::view::over(bytes)) == json::encode(config), "both formats agree on the keys");
}

void naming_styles() {
    using serpent::naming_style;
    using serpent::detail::convert_case;
    static_assert(convert_case("mtuBytes", naming_style::snake_case).view() == "mtu_bytes");
    static_assert(convert_case("mtu_bytes", naming_style::camel_case).view() == "mtuBytes");
    static_assert(convert_case("mtu_bytes", naming_style::pascal_case).view() == "MtuBytes");
    static_assert(convert_case("mtuBytes", naming_style::kebab_case).view() == "mtu-bytes");
    static_assert(convert_case("mtuBytes", naming_style::screaming_snake_case).view() == "MTU_BYTES");
    static_assert(convert_case("mtuBytes", naming_style::as_written).view() == "mtuBytes");
}

// Two members happening to be called first and second does not make a type a pair. It used to:
// such a type was written as a two-element array of those two, every other member was dropped
// without a word, and what was written could not be read back.
struct [[= serpent::serializable {}]] podium {
    std::string first;
    std::string second;
    std::string third;

    friend bool operator==(const podium &, const podium &) = default;
};

void members_called_first_and_second_are_still_members() {
    const podium value { "gold", "silver", "bronze" };
    check_equal(json::encode(value), std::string { R"({"first":"gold","second":"silver","third":"bronze"})" },
            "a type with members called first and second is an object, with all of them");
    check(json::decode<podium>(json::encode(value)) == value, "and reads back");
    check(bjdata::decode<podium>(bjdata::encode(value)) == value, "in both formats");

    // A real pair is still a pair.
    check_equal(json::encode(std::pair { 1, std::string { "one" } }), std::string { R"([1,"one"])" },
            "a std::pair is still a two-element array");
    static_assert(!serpent::detail::pair_like<podium>);
    static_assert(serpent::detail::pair_like<std::pair<int, int>>);
}

int main() {
    members_called_first_and_second_are_still_members();
    an_enum_can_say_what_it_is_on_the_wire();
    a_sequence_is_walked_once();
    a_constant_key_is_framed_once();
    the_generated_reader_agrees_with_the_generic_one();
    identifiers_become_keys();
    a_discriminant_names_the_type();
    a_field_can_carry_the_tag();
    a_variant_can_mix_shapes();
    a_hand_written_conversion_is_left_alone();
    a_type_you_do_not_own();
    annotations_adjust_keys();
    an_absent_optional_is_still_written_null();
    reflection_composes_with_containers();
    the_same_type_serves_both_formats();
    naming_styles();
    return report("reflect");
}
