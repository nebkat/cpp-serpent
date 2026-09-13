// The owning tree: building a document whose shape is not known until it is built.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

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
    check_equal(*info["records"].as_int<int>(), 3, "the value is there");
    check_equal(*info["source"].as_string(), std::string_view { "index" }, "and so is the string");

    // The case the tree exists for: a branch decides whether a field is there at all.
    const bool checked = false;
    if (checked) info["crcs"] = 1;
    check(!info.contains("crcs"), "a member the branch did not add is absent");

    info["window"]["first"] = 10;
    info["window"]["last"] = 20;
    check(info["window"].is_object(), "a nested member is made on the way");
    check_equal(*info["window"]["last"].as_int<int>(), 20, "and reads back");

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
    check_equal(*value { std::uint8_t { 200 } }.as_int<int>(), 200, "a narrow integer reads back");
    check(!value { -1 }.as_int<unsigned>().has_value(), "and a value that does not fit is nothing");
    check_equal(*value { 3 }.as_float<double>(), 3.0, "an integer reads as a real");
    check(!value { 1.5 }.as_int<int>().has_value(), "but a real does not read as an integer");

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
    check_equal(*back->detail["asked"].as_int<int>(), 30, "and the one whose shape it did not know");

    // The tree is read through the same document as everything else, so a reader that wants
    // only part of it does not build the rest.
    const auto view = bjdata::view::over(bytes);
    check_equal(*view["detail"]["records"].as_int<int>(), 12, "read in place without a tree at all");
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
    refusals();
    return report("value");
}
