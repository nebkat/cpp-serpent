// The forms that exist for what reflection cannot do, and for types that are not yours.
//
// Deliberately free of annotations, so it builds and runs on a compiler with no reflection -
// which is the situation these forms are for.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/chrono.hpp>
#include <serpent/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

/** A member whose existing value is a default worth keeping is said so outright. */
struct lenient {
    std::string label;
    int count = 7;

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), lenient> value) {
        visitor.member("label", value.label);
        visitor.member_if_present("count", value.count);
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

/** A duration is its count; a time point and a time of day defer to their duration. */
struct timings {
    std::chrono::milliseconds elapsed {};
    std::chrono::sys_time<std::chrono::milliseconds> at {};
    std::chrono::hh_mm_ss<std::chrono::seconds> when {};
    std::chrono::duration<double> measured {};

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), timings> value) {
        visitor.member("elapsed", value.elapsed);
        visitor.member("at", value.at);
        visitor.member("when", value.when);
        visitor.member("measured", value.measured);
    }
};

void chrono_types() {
    using namespace std::chrono;
    const timings value { milliseconds { 1500 }, sys_time<milliseconds> { milliseconds { 1700000000000 } },
        hh_mm_ss { seconds { 3661 } }, duration<double> { 2.5 } };

    check_equal(json::encode(value), R"({"elapsed":1500,"at":1700000000000,"when":3661,"measured":2.5})",
            "each is the number its type means, and no unit travels");

    const auto back = json::decode<timings>(json::encode(value));
    check(back && back->elapsed == value.elapsed, "a duration round-trips");
    check(back && back->at == value.at, "a time point round-trips through its duration");
    check(back && back->when.to_duration() == value.when.to_duration(), "and a time of day");
    check(back && back->measured == value.measured, "including one whose representation is a real");

    const auto binary = bjdata::decode<timings>(bjdata::encode(value));
    check(binary && binary->at == value.at && binary->when.to_duration() == value.when.to_duration(),
            "the same through BJData");

    // The unit is the type's, so reading into a different one reinterprets the same number.
    // Nothing can catch that, which is why it is worth saying out loud.
    const auto as_seconds = json::decode<seconds>(json::encode(milliseconds { 1500 }));
    check(as_seconds && as_seconds->count() == 1500, "a count read as another unit is that count, not that time");
}

/** decode() says whether it worked; try_decode() says why it did not. */
void a_failed_decode_can_say_why() {
    const auto good = json::try_decode<labelled>(R"({"label":"a","count":2})");
    check(good && good->count == 2, "a document that fits gives the value");

    // Malformed: the kind and where.
    const auto truncated = json::try_decode<labelled>(R"({"label":"a","count":)");
    check(!truncated, "a truncated document fails");
    check(truncated.error().code() == serpent::errc::unexpected_end, "and says what was wrong");
    check(truncated.error().offset() > 0, "and where it ran out");

    const auto bad_escape = json::try_decode<labelled>(R"({"label":"a\q"})");
    check(!bad_escape && bad_escape.error().code() == serpent::errc::invalid_escape, "an unknown escape is named");

    // Well formed, but not this shape: named, with no offset to give.
    const auto wrong_shape = json::try_decode<labelled>("[1,2,3]");
    check(!wrong_shape && wrong_shape.error().code() == serpent::errc::type_mismatch,
            "a document that parses but does not fit is a type mismatch");

    // And the same through BJData, where a truncated buffer is the usual failure.
    const auto bytes = bjdata::encode(labelled { "a", 2 });
    const auto whole = bjdata::try_decode<labelled>(bytes);
    check(whole && whole->label == "a", "binary round-trips through try_decode");
    const auto cut = bjdata::try_decode<labelled>(std::span { bytes }.first(bytes.size() / 2));
    check(!cut, "a half a document fails");
    check(cut.error().code() == serpent::errc::unexpected_end, "and says so");

    // A hand-written walk gets the same guarantee the annotated types do, from writing
    // member() and nothing more: the member has to be there.
    const auto short_of_a_member = json::try_decode<labelled>(R"({"label":"a"})");
    check(!short_of_a_member, "a member the type names and the document leaves out is a failure");
    check(short_of_a_member.error().code() == serpent::errc::missing_key
                    && short_of_a_member.error().key() == "count",
            "and the error names which member");

    // The lenient spelling is the one that lets it through.
    const auto tolerated = json::decode<lenient>(R"({"label":"a"})");
    check(tolerated && tolerated->label == "a" && tolerated->count == 7,
            "member_if_present keeps the value already there");
}

/**
 * A container that hands out a proxy rather than a reference.
 *
 * std::vector<bool> is the one in the standard library, and it reaches the element paths in
 * both directions: nothing to bind a reference to on the way in, and something that is not the
 * element type on the way out.
 */
void containers_that_hand_out_proxies() {
    const std::vector<bool> flags { true, false, true, true };

    check_equal(json::encode(flags), std::string { "[true,false,true,true]" }, "written as booleans");

    const auto text_back = json::decode<std::vector<bool>>(json::encode(flags));
    check(text_back && *text_back == flags, "and read back from text");

    const auto bytes = bjdata::encode(flags);
    const auto binary_back = bjdata::decode<std::vector<bool>>(bytes);
    check(binary_back && *binary_back == flags, "and through binary");

    // T and F are not valid strong types, so this stays a counted untyped array rather than
    // becoming a packed one.
    const auto document = bjdata::view::over(bytes);
    check(document.is_array() && document.size() == 4, "an array of four");
    check(document[0].as_bool().value_or(false) && !document[1].as_bool().value_or(true),
            "whose elements are booleans, not numbers");

    // The element of an ordinary container is still taken by reference, not copied through its
    // value type - which a range of strings is what would notice.
    const std::vector<std::string> names { "alpha", "beta" };
    const auto names_back = bjdata::decode<std::vector<std::string>>(bjdata::encode(names));
    check(names_back && *names_back == names, "an ordinary container is unaffected");
}

int main() {
    one_function_both_directions();
    a_failed_decode_can_say_why();
    chrono_types();
    directions_that_differ();
    a_type_that_is_not_yours();
    a_type_that_is_not_an_object();
    containers_that_hand_out_proxies();
    return report("manual");
}
