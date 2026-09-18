// The owning tree: building a document whose shape is not known until it is built.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <limits>
#include <string>
#include <vector>

using serpent::conversion_object_t;
using serpent::errc;
using serpent::value;
namespace json = serpent::json;
namespace bjdata = serpent::bjdata;

namespace {

/** A type with a member whose shape it does not know, which is what the tree is for. */
struct failure_report {
    std::string reason;
    value detail;

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), failure_report> item) {
        visitor.member("reason", item.reason);
        visitor.member("detail", item.detail);
    }
};

void built_a_piece_at_a_time() {
    value info;
    check(info.is_null(), "a value starts as null");

    info["records"] = 3;
    info["source"] = "index";
    check(info.is_object(), "naming a member makes it an object");
    check_equal(info.size(), std::size_t { 2 }, "two members");
    check_equal(*info["records"].as<int>(), 3, "the value is there");
    check_equal(*info["source"].as<std::string_view>(), std::string_view { "index" }, "and so is the string");

    // The case the tree exists for: a branch decides whether a field is there at all.
    const bool checked = false;
    if (checked) info["crcs"] = 1;
    check(!info.contains("crcs"), "a member the branch did not add is absent");

    info["window"]["first"] = 10;
    info["window"]["last"] = 20;
    check(info["window"].is_object(), "a nested member is made on the way");
    check_equal(*info["window"]["last"].as<int>(), 20, "and reads back");

    info["ids"].push_back(1);
    info["ids"].push_back(2);
    check(info["ids"].is_array() && info["ids"].size() == 2, "push_back makes an array");

    // Or written out in one piece, where it is known in one piece.
    const auto literal = value::of({ { "first", 10 }, { "last", 20 } });
    check(literal == info["window"], "the same object, built the other way");
}

void what_it_holds() {
    check(value {}.is_null(), "default");
    check(value { true }.is_boolean(), "bool");
    check(value { 7 }.is_integer(), "int");
    check(value { 7u }.is_integer(), "unsigned");
    check(value { 1.5 }.is_real(), "double");
    check(value { "text" }.is_string(), "a string literal is a string, not a bool");
    check(value { std::string { "text" } }.is_string(), "string");
    check(value { value::binary { std::byte { 1 } } }.is_binary(), "binary");
    check(value { value::array { 1, 2 } }.is_array(), "array");

    // Widths are not kept: the writer narrows every integer to the marker that holds it, so
    // there is nothing for the tree to remember.
    check_equal(*value { std::uint8_t { 200 } }.as<int>(), 200, "a narrow integer reads back");
    check(!value { -1 }.as<unsigned>().has_value(), "and a value that does not fit is nothing");
    check_equal(*value { 3 }.as<double>(), 3.0, "an integer reads as a real");
    check(!value { 1.5 }.as<int>().has_value(), "but a real does not read as an integer");

    const auto missing = value::of({ { "a", 1 } })["b"];
    check(missing.is_null(), "an absent member reads as null");
}

void round_trips() {
    value document;
    document["name"] = "sunrise";
    document["counts"].push_back(1);
    document["counts"].push_back(2);
    document["nested"]["flag"] = true;
    document["nothing"] = nullptr;
    document["real"] = 1.5;

    const auto bytes = bjdata::encode(document);
    const auto binary_back = bjdata::decode<value>(bytes);
    check(binary_back.has_value(), "a tree decodes back out of BJData");
    check(*binary_back == document, "unchanged");

    const auto text = json::encode(document);
    const auto text_back = json::decode<value>(text);
    check(text_back.has_value(), "and out of JSON");
    check(*text_back == document, "unchanged");

    // Keys go out in the order they were added, which is the order they were meant in.
    check_equal(std::string_view { text }.substr(0, 17), std::string_view { R"({"name":"sunrise")" },
            "the first member written is the first member added");

    // Binary is binary where the format has it, and the numbers it was where it does not.
    value blob;
    blob["bytes"] = value::binary { std::byte { 1 }, std::byte { 2 } };
    const auto blob_back = bjdata::decode<value>(bjdata::encode(blob));
    check(blob_back && (*blob_back)["bytes"].is_binary(), "BJData keeps binary binary");
    const auto as_text = json::decode<value>(json::encode(blob));
    check(as_text && (*as_text)["bytes"].is_array(), "JSON carries it as an array of numbers");
}

void a_member_of_another_type() {
    failure_report failure { "out_of_range", {} };
    failure.detail["records"] = 12;
    failure.detail["asked"] = 30;

    const auto bytes = bjdata::encode(failure);
    const auto back = bjdata::decode<failure_report>(bytes);
    check(back.has_value(), "a struct with a tree in it round-trips");
    check_equal(back->reason, std::string { "out_of_range" }, "the named member");
    check_equal(*back->detail["asked"].as<int>(), 30, "and the one whose shape it did not know");

    // The tree is read through the same document as everything else, so a reader that wants
    // only part of it does not build the rest.
    const auto reader = bjdata::reader::over(bytes);
    check_equal(*reader["detail"]["records"].as<int>(), 12, "read in place without a tree at all");
}

#if SERPENT_HAS_REFLECTION
/** The same, said the way types are meant to say it. */
struct[[= serpent::serializable {}]] annotated_report {
    std::string reason;
    value detail;
};
#endif

void a_member_of_an_annotated_type() {
#if SERPENT_HAS_REFLECTION
    annotated_report failure { "out_of_range", {} };
    failure.detail["records"] = 12;

    const auto back = bjdata::decode<annotated_report>(bjdata::encode(failure));
    check(back.has_value(), "an annotated struct with a tree in it round-trips");
    check(back && back->detail == failure.detail, "the tree survives");
#endif
}

/**
 * Any serializable type as a tree, through its own conversion.
 *
 * The bridge between the two halves: a tree is otherwise buildable only out of the builtin
 * kinds, which leaves every type that has a conversion unreachable from it.
 */
void any_type_as_a_tree() {
    failure_report original { "out_of_range", {} };
    original.detail["records"] = 12;

    // A type with a hand-written json_convert, which knows nothing about trees.
    const auto tree = serpent::to_value(original);
    check(tree.is_object() && tree.size() == 2, "a hand-written conversion reaches the tree");
    check_equal(*tree["reason"].as<std::string_view>(), std::string_view { "out_of_range" }, "its members are there");
    check_equal(*tree["detail"]["records"].as<int>(), 12, "nested, including a tree inside a tree");

    // Writing the tree must be the same document as writing the value.
    check(bjdata::encode(tree) == bjdata::encode(original), "the tree encodes as the value did");

    // Which is the point: shape it afterwards.
    auto shaped = serpent::to_value(original);
    shaped["at"] = 1700000000;
    check(shaped.size() == 3 && shaped["at"].is_integer(), "and then it can be added to");

    // Scalars and containers are trees too, not only objects.
    check(serpent::to_value(42).as<int>() == 42, "a scalar");
    check(serpent::to_value(std::vector<int> { 1, 2, 3 }).size() == 3, "a container");
    check(serpent::to_value(std::vector<std::byte> { std::byte { 9 } }).is_binary(), "and a byte range stays binary");
}

/**
 * The tree answers what a reader over bytes answers, so it is a third way to hold a document
 * rather than a separate world: bytes scanned in place, and values already materialised, reach
 * a type through the same code.
 */
void a_tree_is_a_source() {
    const failure_report original { "out_of_range", serpent::value::of({ { "records", 12 } }) };
    const auto tree = serpent::to_value(original);

    const auto recovered = serpent::from_value<failure_report>(tree);
    check(recovered.has_value(), "a type reads straight out of a tree");
    check(recovered && recovered->reason == "out_of_range", "its members are there");
    check(recovered && *recovered->detail["records"].as<int>() == 12, "nested, including a tree inside it");

    // The same answers a reader gives, from the same document held the other way.
    const serpent::value_reader handle { tree };
    check(handle.is_object() && handle.size() == 2, "shape");
    check_equal(*handle["reason"].as<std::string_view>(), std::string_view { "out_of_range" }, "a member by key");
    check(!handle["nope"].is_valid(), "and an absent member is invalid, not null");

    std::size_t walked = 0;
    for (const auto &entry : handle.items()) walked += entry.key_is("reason") ? 1 : 0;
    check_equal(walked, std::size_t { 1 }, "items() walks it the way every reader's does");

    const auto numbers = serpent::to_value(std::vector<int> { 4, 5, 6 });
    check_equal(serpent::value_reader { numbers }[1].as<int>().value_or(0), 5, "and an array indexes");
    check(serpent::from_value<std::vector<int>>(numbers) == std::vector<int> { 4, 5, 6 }, "and reads back whole");
}

void refusals() {
    const auto throws = [](auto &&action, errc expected, std::string_view what) {
        try {
            action();
        } catch (const serpent::error &failure) {
            check_equal(failure.code(), expected, what);
            return;
        }
        check(false, what);
    };

    const auto document = value::of({ { "a", 1 } });
    throws([&] { return document.at("b"); }, errc::missing_key, "at() on a missing key throws");
    throws([&] { return document.at(std::size_t { 0 }); }, errc::out_of_range, "at() on a non-array throws");

    value scalar { 1 };
    throws([&] { return scalar["key"]; }, errc::type_mismatch, "naming a member of a number throws");
    throws([&] { return scalar.push_back(1); }, errc::type_mismatch, "appending to a number throws");

    // Order is not part of what an object is.
    check(value::of({ { "a", 1 }, { "b", 2 } }) == value::of({ { "b", 2 }, { "a", 1 } }),
            "the same members in a different order are the same object");
    check(!(value::of({ { "a", 1 } }) == value::of({ { "a", 2 } })), "different values are not");
}

} // namespace

int main() {
    built_a_piece_at_a_time();
    what_it_holds();
    round_trips();
    a_member_of_another_type();
    a_member_of_an_annotated_type();
    any_type_as_a_tree();
    a_tree_is_a_source();
    refusals();
    // A document with the same key twice keeps the last value, wherever the object's size puts
    // it - the few-members way or the many-members way - and no duplicate survives.
    {
        const auto small = json::decode<value>(R"({"a":1,"b":2,"a":3})");
        check(small && small->size() == 2 && small->at("a").as<int>() == 3, "a repeated key keeps the last value");
        check_equal(json::encode(*small), std::string { R"({"a":3,"b":2})" }, "in the place of the first");

        std::string text = "{";
        for (int index = 0; index < 40; ++index) text += (index ? "," : "") + std::string { "\"k" } + std::to_string(index) + "\":" + std::to_string(index);
        text += R"(,"k7":700,"k39":3900,"k0":0})";
        const auto large = json::decode<value>(text);
        check(large && large->size() == 40 && large->at("k7").as<int>() == 700 && large->at("k39").as<int>() == 3900
                        && large->at("k0").as<int>() == 0,
                "and so does an object with many members");
        std::size_t position = 0;
        for (const auto &[name, held] : *large->as_object()) {
            if (name == "k7") check_equal(position, std::size_t { 7 }, "which stays where its first occurrence was");
            ++position;
        }
    }

    // A tree built from JSON text takes its own one-pass reader, which has to refuse exactly
    // what the reader handle refuses, and stop exactly where a value ends.
    {
        for (const auto *bad : { "[1,2", "{\"a\":1,}", "[01]", "[1e]", "{\"a\" 1}", "[1 2]", "{1:2}", "\"unterminated",
                     "[\"\\x\"]", "nul", "-", "[-]", "{\"a\":}" })
            check(!json::decode<serpent::value>(bad).has_value(), "a malformed document is refused");
        std::string deep;
        for (int i = 0; i < 40; ++i) deep += '[';
        for (int i = 0; i < 40; ++i) deep += ']';
        check(!json::decode<serpent::value>(deep).has_value(), "and one nested past the depth limit");

        const auto numbers = json::decode<serpent::value>("[0,-1,18446744073709551615,-9223372036854775808,1.5,-2e3,1E2,7.0]");
        check(numbers.has_value() && numbers->size() == 8, "every spelling of a number");
        check(numbers && numbers->at(0).is_integer() && numbers->at(1).as<int>() == -1, "an integer stays one");
        check(numbers && numbers->at(2).as<std::uint64_t>() == 18446744073709551615ull, "as wide as it needs");
        check(numbers && numbers->at(3).as<std::int64_t>() == std::numeric_limits<std::int64_t>::min(), "either way");
        check(numbers && numbers->at(4).is_real() && numbers->at(5).as<double>() == -2000.0 && numbers->at(6).as<double>() == 100.0,
                "a point or an exponent makes a real");
        check(numbers && numbers->at(7).is_real() && numbers->at(7).as<double>() == 7.0, "even of a whole value");

        // A tree as a member of a type: read where it stands, and the member after it still
        // found - a required one, so that the reader not being told where the tree ended would
        // show as a missing member rather than pass.
        const auto report = json::decode<failure_report>(
                R"({"detail":{"a":[1,{"b":"c\u0041"}],"d":null},"reason":"x"})");
        check(report.has_value() && report->reason == "x", "a tree member inside a type, and the member after it");
        check(report && report->detail.at("a").at(1).at("b").as<std::string_view>() == "cA", "holds what the text held, decoded");

        // Read into a tree that already holds something, everything it held is gone.
        serpent::value reused = serpent::value::of({ { "old", 1 } });
        check(json::decode_into("[1,2,3]", reused) && reused.is_array() && reused.size() == 3, "a tree read again is replaced");
        check(json::decode_into("{\"x\": [ ] , \"y\" : { } }", reused) && reused["x"].is_array() && reused["y"].is_object(),
                "whitespace everywhere it may be");
    }

    return report("value");
}
