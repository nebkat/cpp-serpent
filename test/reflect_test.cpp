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
struct serpent::enable_reflection<site> : std::true_type {};

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
struct serpent::enable_reflection<foreign_reading> : std::true_type {
    static constexpr serpent::naming_style style = serpent::naming_style::snake_case;
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
            "enable_reflection opts it in and carries the naming rule");
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
    const auto by_identifier = json::decode<drawing>(R"({"body":{"kind":"plain_point","x":1,"y":2}})");
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

    // A strongly typed object: the members share one marker, so the values carry none.
    const std::uint8_t typed[] = { '{', '$', 'U', '#', 'U', 2, 'U', 1, 'x', 7, 'U', 1, 'y', 9 };
    const auto strong = bjdata::decode<point>(std::as_bytes(std::span { typed }));
    check(strong && strong->x == 7 && strong->y == 9, "a strongly typed object reads");

    // Reordered and partial, against the generated reader specifically.
    check(bjdata::view::over(bjdata::encode(point { 1, 2 })).is_object(), "an object is what we wrote");
    const auto reordered = json::decode<point>(R"({"y":20,"x":10})");
    check(reordered && reordered->x == 10 && reordered->y == 20, "declaration order is not required");
    const auto partial = json::decode<point>(R"({"x":7})");
    check(partial && partial->x == 7 && partial->y == 0, "an absent key leaves the default");
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

int main() {
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
