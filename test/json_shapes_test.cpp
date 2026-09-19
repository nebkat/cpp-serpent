// The reader generated for a type recognises the punctuation it would have written itself, and
// falls back to scanning when a document is written some other way. The fast path is what every
// other suite exercises, because they read documents this library wrote. This one is the rest:
// whitespace where we put none, keys in another order, keys we do not name, escapes in a key.

#include "check.hpp"

#include <serpent/json.hpp>

#include <array>
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

struct [[= serpent::serializable {}]] holder {
    std::string name;
    record inner;
};

struct [[= serpent::serializable {}]] grid {
    std::vector<std::array<int, 2>> cells;
    std::vector<std::vector<std::string>> rows;
    int after = 0;

    friend bool operator==(const grid &, const grid &) = default;
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

    // Sequences of sequences are read with the one cursor too, whatever the depth.
    const grid expected_grid { { { 1, 2 }, { 3, 4 } }, { { "a" }, {}, { "b", "c" } }, 7 };
    check(json::decode<grid>(R"({"cells":[[1,2],[3,4]],"rows":[["a"],[],["b","c"]],"after":7})") == expected_grid,
            "nested sequences read in place");
    check(json::decode<grid>(R"( { "cells" : [ [ 1 , 2 ] , [ 3 , 4 ] ] , "rows" : [ [ "a" ] , [ ] , [ "b" , "c" ] ] , "after" : 7 } )")
                    == expected_grid,
            "with whitespace anywhere");
    check(!json::decode<grid>(R"({"cells":[[1,2,3]],"rows":[],"after":7})").has_value(),
            "a fixed-length element of another length is not this type");
    check(!json::decode<grid>(R"({"cells":[[1,2]],"rows":[["a",1]],"after":7})").has_value(),
            "an element that is not of the type is refused");
    check(!json::decode<grid>(R"({"cells":[[1,2],[3,]],"rows":[],"after":7})").has_value(), "and so is a malformed one");

    // A member that does not convert is stepped over, and the members after it are still read:
    // the object is reported as not converting, not as missing them.
    grid partly;
    check(!json::decode_into(R"({"cells":[[1,"x"]],"rows":[["z"]],"after":7})", partly), "a member that does not convert fails the read");
    check(partly.after == 7 && partly.rows == std::vector<std::vector<std::string>> { { "z" } },
            "but the members after it were still read");

    // Read into an object that already holds one, a nested member is filled where it stands: its
    // containers keep the capacity they have rather than being built afresh and moved in.
    holder reused;
    check(json::decode_into(R"({"name":"a","inner":{"id":1,"name":"north","ratio":0.5,"active":true,"tags":[1,2,3,4,5,6,7,8]}})", reused),
            "a nested described member reads in place");
    const auto *const tags_before = reused.inner.tags.data();
    const auto *const name_before = reused.inner.name.data();
    check(json::decode_into(R"({"name":"b","inner":{"id":2,"name":"south","ratio":0.25,"active":false,"tags":[9,8,7]}})", reused),
            "and again");

    // And written into a string that already exists, the same way round: the first write leaves
    // the string with the room a document of this size takes, and the next one keeps it.
    std::string out = "junk";
    check(json::write(reused, out).has_value() && out == json::encode(reused), "written into a string");
    const auto *const storage = out.data();
    check(json::write(reused, out).has_value() && out == json::encode(reused) && out.data() == storage,
            "and again, into the storage it has");
    check(reused.inner.tags.data() == tags_before && reused.inner.tags == std::vector<int> { 9, 8, 7 },
            "keeping the vector's storage");
    check(reused.inner.name.data() == name_before && reused.inner.name == "south", "and the string's");

    return report("json_shapes");
}
