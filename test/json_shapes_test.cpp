// The reader generated for a type recognises the punctuation it would have written itself, and
// falls back to scanning when a document is written some other way. The fast path is what every
// other suite exercises, because they read documents this library wrote. This one is the rest:
// whitespace where we put none, keys in another order, keys we do not name, escapes in a key.

#include "check.hpp"

#include <serpent/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace json = serpent::json;

namespace {

struct [[= serpent::serializable {}]] record {
    int id = 0;
    std::string name;
    double ratio = 0;
    bool active = false;
    std::vector<int> tags;

    friend bool operator==(const record &, const record &) = default;
};

struct [[= serpent::serializable {}]] with_defaults {
    int always = 0;
    [[= serpent::defaulted {}]] int sometimes = 77;
    std::optional<std::string> maybe;
};

struct [[= serpent::serializable {}]] prefixes {
    int name = 0;
    int named = 0;
    int names = 0;

    friend bool operator==(const prefixes &, const prefixes &) = default;
};

struct [[= serpent::serializable {}]] nothing_required {
    [[= serpent::defaulted {}]] int value = 9;
};

const record expected { 42, "north", 0.5, true, { 1, 2, 3 } };

} // namespace

int main() {
    // What this library writes: no space anywhere, keys in declaration order.
    const auto compact = json::encode(expected);
    check_equal(compact, std::string { R"({"id":42,"name":"north","ratio":0.5,"active":true,"tags":[1,2,3]})" },
            "the shape the fast path is for");
    check(json::decode<record>(compact) == expected, "and it reads");

    // Space around every piece of punctuation there is.
    const auto spaced = json::decode<record>(R"(  {
        "id"  :  42 ,
        "name"   : "north",
        "ratio": 0.5   ,
        "active"    :true,
        "tags"  :  [ 1 , 2 , 3 ]
    }  )");
    check(spaced == expected, "the same document written with whitespace");

    // Declaration order reversed.
    check(json::decode<record>(
                  R"({"tags":[1,2,3],"active":true,"ratio":0.5,"name":"north","id":42})")
                    == expected,
            "the same members in the opposite order");

    // Keys this type does not name, before, between and after the ones it does.
    check(json::decode<record>(R"({"extra":{"a":[1,{"b":null}]},"id":42,"unused":"x","name":"north",
            "ratio":0.5,"active":true,"tags":[1,2,3],"trailing":[[]]})")
                    == expected,
            "keys the type does not name are skipped, whatever they hold");

    // A key that has to be decoded before it can be matched: "na\u006de" is "name", and the
    // fast path cannot see that, so this is the escape branch of the fallback.
    check(json::decode<record>(
                  R"({"id":42,"na\u006de":"north","ratio":0.5,"active":true,"tags":[1,2,3]})")
                    == expected,
            "a key written with escapes still matches the member it names");
    check(json::decode<record>(
                  R"({"\u0069\u0064":42,"name":"north","ratio":0.5,"active":true,"tags":[1,2,3]})")
                    == expected,
            "and one written entirely in escapes");

    // A key that begins like another. "name" must not be taken for "named".
    check(json::decode<prefixes>(R"({"named":2,"names":3,"name":1})") == prefixes { 1, 2, 3 },
            "one key is not mistaken for another it is a prefix of");

    // Absence, which the fast path must not paper over.
    const auto sparse = json::decode<with_defaults>(R"({"always":5})");
    check(sparse && sparse->always == 5 && sparse->sometimes == 77 && !sparse->maybe,
            "a defaulted member keeps its value and an optional stays empty");
    check(!json::decode<with_defaults>(R"({"sometimes":1})").has_value(),
            "a member the type needs is still required");

    // The empty object, and one whose only content is a key we do not name.
    const auto empty = json::decode<with_defaults>(R"({})");
    check(!empty.has_value(), "an empty object is missing what the type needs");
    const auto blank = json::decode<nothing_required>(R"({})");
    check(blank && blank->value == 9, "an empty object leaves a defaulted member alone");
    const auto ignored = json::decode<nothing_required>(R"({"other":[1,2,3]})");
    check(ignored && ignored->value == 9, "so does one naming nothing this type knows");

    // Malformed documents must be refused rather than half-read.
    for (const auto *bad : { R"({"id":42,)", R"({"id"42})", R"({"id":42"name":"x"})", R"({"id":}) ",
                 R"({"id":42,,"name":"x"})", R"({)" })
        check(!json::decode<record>(bad).has_value(), "a malformed object is refused");

    // A member whose value is the wrong type is a failed read, not a wrong value.
    check(!json::decode<record>(R"({"id":"forty-two","name":"north","ratio":0.5,"active":true,"tags":[]})")
                    .has_value(),
            "a string where an integer belongs fails");
    check(!json::decode<record>(R"({"id":1.5,"name":"north","ratio":0.5,"active":true,"tags":[]})").has_value(),
            "and so does a real");

    return report("json_shapes");
}
