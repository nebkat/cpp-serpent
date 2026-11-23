// decode, asked to trace, naming where a document stopped fitting its type, in every format.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/msgpack.hpp>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace serpent;
namespace bjdata = serpent::bjdata;
namespace json = serpent::json;
namespace msgpack = serpent::msgpack;

namespace {

constexpr decode_options traced { .trace = true };

struct[[= serpent::serializable {}]] address {
    std::string ip;
    std::string mask;
};

struct[[= serpent::serializable {}]] network {
    std::string ssid;
    std::optional<address> ip_info;
};

struct[[= serpent::serializable {}]] networks {
    std::vector<network> aps;
};

/** The same shape, with the one member a number, as a device that disagrees would send. */
struct[[= serpent::serializable {}]] numbered_address {
    int ip = 0;
    std::string mask;
};

struct[[= serpent::serializable {}]] numbered_network {
    std::string ssid;
    std::optional<numbered_address> ip_info;
};

struct[[= serpent::serializable {}]] numbered_networks {
    std::vector<numbered_network> aps;
};

struct[[= serpent::serializable {}]] two_numbers {
    int a = 0;
    int b = 0;
};

struct[[= serpent::serializable {}]] number_and_text {
    int a = 0;
    std::string b;
};

/** Fails against an object of two numbers, at y, before point is tried and accepts it. */
struct[[= serpent::serializable {}]] labelled_point {
    int x = 0;
    std::string y;
};

struct[[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};

struct[[= serpent::serializable {}]] after_a_variant {
    std::variant<labelled_point, point> where;
    int count = 0;
};

struct[[= serpent::serializable {}]] node {
    std::vector<node> children;
    [[= serpent::defaulted {}]] int leaf = 0;
};

struct[[= serpent::serializable {}]] either {
    std::variant<int, std::string> value;
};

const numbered_networks numbered {
    .aps = {
        { .ssid = "shed", .ip_info = std::nullopt },
        { .ssid = "yard", .ip_info = numbered_address { .ip = 5, .mask = "255.255.255.0" } },
    },
};

void nested_member_of_the_wrong_type() {
    constexpr std::string_view text
            = R"({"aps":[{"ssid":"shed"},{"ssid":"yard","ip_info":{"ip":5,"mask":"255.255.255.0"}}]})";
    const auto decoded = json::decode<networks>(text, traced);
    check(!decoded && decoded.error().code() == errc::type_mismatch, "a number where text belongs does not fit");
    check_equal(decoded.error().path(), std::string_view { "aps[1].ip_info.ip" }, "and the path leads to it");
    check(decoded.error().found() == kind::integer, "naming what was there instead");
    check_equal(decoded.error().offset(), text.find("5,"), "at the offset where it stands");

    const auto binary = bjdata::decode<networks>(bjdata::encode(numbered), traced);
    check(!binary && binary.error().code() == errc::type_mismatch, "BJData's generated reader refuses it too");
    check_equal(binary.error().path(), std::string_view { "aps[1].ip_info.ip" },
            "and its second walk finds the same member");
    check(binary.error().found() == kind::integer, "holding a number");

    const auto packed = msgpack::decode<networks>(msgpack::encode(numbered), traced);
    check(!packed && packed.error().code() == errc::type_mismatch, "as does MessagePack");
    check_equal(packed.error().path(), std::string_view { "aps[1].ip_info.ip" }, "at the same path");
}

void nested_member_missing() {
    const auto decoded = json::decode<networks>(R"({"aps":[{"ssid":"shed"},{"ip_info":null}]})", traced);
    check(!decoded && decoded.error().code() == errc::missing_key, "a member left out deep down is named");
    check_equal(decoded.error().key(), "ssid", "by its key");
    check_equal(decoded.error().path(), std::string_view { "aps[1]" }, "in the object that lacks it");
    check(decoded.error().found() == kind::object, "which is an object");

    const auto top = json::decode<two_numbers>(R"({"a":1})", traced);
    check(!top && top.error().code() == errc::missing_key && top.error().key() == "b", "at the top as before");
    check_equal(top.error().path(), std::string_view { "" }, "where the object is the root");
}

void a_record_of_a_table() {
    // Enough records alike for BJData to write them as a table, whose reader is generated too.
    std::vector<number_and_text> rows(40, number_and_text { .a = 1, .b = "text" });
    const auto bytes = bjdata::encode(rows);
    check(bytes.size() > 2 && bytes[1] == std::byte { '$' }, "the records are written as a table");
    const auto decoded = bjdata::decode<std::vector<two_numbers>>(bytes, traced);
    check(!decoded && decoded.error().code() == errc::type_mismatch, "text where a table's number belongs");
    check_equal(decoded.error().path(), std::string_view { "[0].b" }, "is found in the first record");
    check(decoded.error().found() == kind::string, "as text");
}

void the_root_itself() {
    const auto decoded = json::decode<networks>("[1,2]", traced);
    check(!decoded && decoded.error().code() == errc::type_mismatch, "an array where an object belongs");
    check_equal(decoded.error().path(), std::string_view { "" }, "is the root, with an empty path");
    check(decoded.error().found() == kind::array, "and says it found an array");
}

void the_first_fault_of_several() {
    const auto decoded = json::decode<two_numbers>(R"({"a":"one","b":"two"})", traced);
    check(!decoded, "neither member fits");
    check_equal(decoded.error().path(), std::string_view { "a" }, "the first one read is reported");
}

void keys_that_are_not_names() {
    const auto decoded = json::decode<std::map<std::string, int>>(R"({"plain":1,"with space":"x"})", traced);
    check(!decoded && decoded.error().code() == errc::type_mismatch, "a map value of the wrong type");
    check_equal(decoded.error().path(), std::string_view { R"(["with space"])" }, "under a key a dot could not carry");

    const auto quoted = json::decode<std::map<std::string, int>>(R"({"say \"hi\"":"x"})", traced);
    check(!quoted, "a key with quotes in it");
    check_equal(quoted.error().path(), std::string_view { R"(["say \"hi\""])" },
            "keeps them escaped, as the document had them");
}

void elements_of_nested_arrays() {
    const auto decoded = json::decode<std::vector<std::vector<int>>>("[[1,2],[3,true]]", traced);
    check(!decoded, "a boolean among numbers");
    check_equal(decoded.error().path(), std::string_view { "[1][1]" }, "is found by index at each level");
    check(decoded.error().found() == kind::boolean, "as a boolean");
}

void a_variant_none_of_whose_alternatives_fit() {
    const auto decoded = json::decode<either>(R"({"value":{"x":1}})", traced);
    check(!decoded, "an object fits neither a number nor text");
    check_equal(decoded.error().path(), std::string_view { "value" },
            "the variant is where it stops, not inside an alternative");
    check(decoded.error().found() == kind::object, "holding the object it was given");
}

void a_variant_that_recovered() {
    // labelled_point fails at where.y before point accepts the value: where fits, and the member
    // that really is wrong comes after it.
    const auto decoded = json::decode<after_a_variant>(R"({"where":{"x":1,"y":2},"count":"many"})", traced);
    check(!decoded, "count is not a number");
    check_equal(decoded.error().path(), std::string_view { "count" },
            "and is reported, rather than the abandoned alternative");
    check(decoded.error().found() == kind::string, "as text");
}

void a_path_too_long_to_keep() {
    const std::string key(40, 'k');
    const std::string text = R"({")" + key + R"(":{")" + key + R"(":"not a number"}})";
    const auto decoded = json::decode<std::map<std::string, std::map<std::string, int>>>(text, traced);
    check(!decoded, "deep under long keys");
    const auto path = decoded.error().path();
    check(path.size() <= error::path_capacity, "the path is kept within its capacity");
    check(path.starts_with("..."), "marked as cut short");
    check(path.ends_with("." + key), "keeping the end nearest the fault");
}

void a_path_too_deep_to_keep() {
    std::string text;
    constexpr int levels = 15; // with the leaf's object and its empty array, the deepest a document may go
    for (int i = 0; i < levels; ++i) text += R"({"children":[)";
    text += R"({"children":[],"leaf":"x"})";
    for (int i = 0; i < levels; ++i) text += "]}";
    const auto decoded = json::decode<node>(text, traced);
    check(!decoded && decoded.error().code() == errc::type_mismatch, "a leaf of the wrong type, at the greatest depth");
    const auto path = decoded.error().path();
    check(path.size() <= error::path_capacity, "the path is kept within its capacity");
    check(path.starts_with("..."), "marked as cut short");
    check(path.ends_with("children[0].leaf"), "keeping the segments nearest the fault");
}

void only_when_asked() {
    constexpr std::string_view text = R"({"aps":[{"ssid":"shed"},{"ssid":"yard","ip_info":{"ip":5,"mask":"m"}}]})";
    const auto plain = json::decode<networks>(text);
    check(!plain && plain.error().code() == errc::type_mismatch, "untraced, the mismatch is still reported");
    check(plain.error().path().empty() && plain.error().offset() == 0, "with no path and no offset");
    check(plain.error().found() == kind::invalid, "and nothing said of what was found");

    const auto deep = json::decode<networks>(R"({"aps":[{"ip_info":null}]})");
    check(!deep && deep.error().code() == errc::type_mismatch, "a key missing below the root is a plain mismatch");

    const auto top = json::decode<two_numbers>(R"({"a":1})");
    check(!top && top.error().code() == errc::missing_key && top.error().key() == "b",
            "one missing from the root is named");
}

void a_decode_that_succeeds_is_untouched() {
    const auto decoded = json::decode<networks>(R"({"aps":[{"ssid":"shed"}]})", traced);
    check(decoded && decoded->aps.size() == 1, "a document that fits decodes as it always did");
}

} // namespace

int main() {
    nested_member_of_the_wrong_type();
    nested_member_missing();
    a_record_of_a_table();
    the_root_itself();
    the_first_fault_of_several();
    keys_that_are_not_names();
    elements_of_nested_arrays();
    a_variant_none_of_whose_alternatives_fit();
    a_variant_that_recovered();
    a_path_too_long_to_keep();
    a_path_too_deep_to_keep();
    only_when_asked();
    a_decode_that_succeeds_is_untouched();
    return report("decode_failure");
}
