// The forms that exist for what reflection cannot do, and for types that are not yours.
//
// Deliberately free of annotations, so it builds and runs on a compiler with no reflection -
// which is the situation these forms are for.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;
using serpent::conversion_object_t;

/** One function, both directions: for a body that does more than list members. */
struct labelled {
    std::string label;
    int count = 0;

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), labelled> value) {
        visitor.member("label", value.label);
        visitor.member("count", value.count);
    }
};

/** Two functions, for a type whose directions genuinely differ. */
struct connection {
    std::string host = "localhost";
    std::optional<int> port {};

    friend void to_json(auto &out, const connection &value) {
        const auto scope = out.object();
        out.key("host");
        out.value(value.host);
        // An absent optional writes no key at all, which a symmetric walk cannot express.
        if (value.port) {
            out.key("port");
            out.value(*value.port);
        }
    }

    friend bool from_json(auto source, connection &value) {
        if (!source.is_object()) return false;
        const connection defaults {};
        value.host = source["host"].as_string().value_or(defaults.host);
        value.port = source["port"].template as_int<int>();
        return true;
    }
};

/** A type from someone else's header, named from outside it. */
struct extent {
    int width = 0;
    int height = 0;
};
SERPENT_DEFINE_TYPE_NON_INTRUSIVE(extent, width, height)

/** A type that is not an object of its members at all. */
struct timestamp {
    std::int64_t seconds = 0;
};
template<>
struct serpent::serializer<timestamp, void> {
    template<typename Writer>
    static void write(Writer &out, const timestamp &value) {
        out.value(value.seconds);
    }
    template<typename Source>
    static bool read(Source source, timestamp &value) {
        const auto seconds = source.template try_get<std::int64_t>();
        if (!seconds) return false;
        value.seconds = *seconds;
        return true;
    }
};

void one_function_both_directions() {
    check_equal(json::encode(labelled { "a", 2 }), R"({"label":"a","count":2})", "json_convert writes both keys");
    const auto back = json::decode<labelled>(R"({"label":"a","count":2})");
    check(back && back->label == "a" && back->count == 2, "and reads them back");
    const auto binary = bjdata::decode<labelled>(bjdata::encode(labelled { "a", 2 }));
    check(binary && binary->count == 2, "the same definition serves BJData");
}

void directions_that_differ() {
    check_equal(json::encode(connection { "example.com", 8080 }), R"({"host":"example.com","port":8080})",
            "a present optional writes its key");
    check_equal(json::encode(connection { "example.com", {} }), R"({"host":"example.com"})",
            "an absent one writes nothing at all, rather than null");

    const auto filled = json::decode<connection>(R"({"host":"h","port":1})");
    check(filled && filled->host == "h" && filled->port == 1, "both fields read");
    const auto defaulted = json::decode<connection>("{}");
    check(defaulted && defaulted->host == "localhost" && !defaulted->port,
            "and an empty object falls back to the defaults the type declares");
}

void a_type_that_is_not_yours() {
    check_equal(json::encode(extent { 3, 4 }), R"({"width":3,"height":4})", "named from outside the type");
    const auto back = json::decode<extent>(R"({"width":3,"height":4})");
    check(back && back->width == 3 && back->height == 4, "and read back");
}

void a_type_that_is_not_an_object() {
    check_equal(json::encode(timestamp { 1700000000 }), "1700000000", "a serializer specialization writes a scalar");
    const auto back = json::decode<timestamp>("1700000000");
    check(back && back->seconds == 1700000000, "and reads one");
    const auto binary = bjdata::decode<timestamp>(bjdata::encode(timestamp { 42 }));
    check(binary && binary->seconds == 42, "in both formats");
}

int main() {
    one_function_both_directions();
    directions_that_differ();
    a_type_that_is_not_yours();
    a_type_that_is_not_an_object();
    return report("manual");
}
