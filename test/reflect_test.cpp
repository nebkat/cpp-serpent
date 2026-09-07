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
#include <string>
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

// Annotated and hand-written at once, which is what migrating a type looks like midway.
struct[[= serpent::serializable {}]] both_forms {
    int x = 1;
    int y = 2;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), both_forms> value) {
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

void a_hand_written_conversion_outranks_reflection() {
    check(json::encode(both_forms {}) == R"({"only_x":1})", "the hand-written json_convert wins");
    const auto back = json::decode<both_forms>(R"({"only_x":9})");
    check(back && back->x == 9 && back->y == 2, "and is what reads, so y keeps its default");

    check(json::encode(specialized {}) == "5", "a serializer specialization wins too");
    const auto scalar = json::decode<specialized>("7");
    check(scalar && scalar->value == 7, "in both directions");
}

void a_type_you_do_not_own() {
    check(json::encode(foreign_reading {}) == R"({"sensor_id":4,"degrees_celsius":21.5})",
            "enable_reflection opts it in and carries the naming rule");
    const auto back = json::decode<foreign_reading>(R"({"sensor_id":9,"degrees_celsius":1.5})");
    check(back && back->sensorId == 9 && back->degreesCelsius == 1.5, "and it reads back");
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
    identifiers_become_keys();
    a_hand_written_conversion_outranks_reflection();
    a_type_you_do_not_own();
    annotations_adjust_keys();
    an_absent_optional_is_still_written_null();
    reflection_composes_with_containers();
    the_same_type_serves_both_formats();
    naming_styles();
    return report("reflect");
}
