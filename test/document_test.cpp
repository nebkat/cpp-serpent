// The borrowing tree: one arena behind every block, and what that does and does not allow.

#include "check.hpp"

#include <serpent/document.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <string>
#include <string_view>
#include <utility>

using serpent::document;
using serpent::value;
namespace json = serpent::json;

namespace {

const std::string sample = R"({
    "name": "a name long enough that it cannot sit inside the node",
    "short": "abc",
    "nested": { "a": [1, 2, 3], "b": [], "c": {} },
    "escaped": "line\nbreak and \"quotes\"",
    "dup": 1, "dup": 2,
    "deep": [[[[[42]]]]],
    "mixed": [1, "two", true, null, 4.5]
})";

} // namespace

int main() {
    {
        auto parsed = json::parse_document(sample);
        check(parsed.has_value(), "a document parses");
        if (!parsed) return report("document");
        const document &doc = *parsed;

        check_equal(doc.at("name").as<std::string_view>().value_or(""),
                "a name long enough that it cannot sit inside the node", "a long string, kept in the arena");
        check_equal(doc.at("short").as<std::string_view>().value_or(""), "abc", "a short one, kept in the node");
        check(doc.at("short").is_borrowed() == false, "a short string borrows nothing");
        check(doc.at("name").is_borrowed(), "a long one does");

        check_equal(doc.at("nested").at("a").as_array().size(), std::size_t { 3 }, "an array borrowed from the arena");
        check(doc.at("nested").at("a").at(1).as<std::int64_t>() == 2, "and read by index");
        check(doc.at("nested").at("b").as_array().empty(), "an empty array");
        check(doc.at("nested").at("c").as_object().empty(), "an empty object");
        check_equal(doc.at("escaped").as<std::string_view>().value_or(""), "line\nbreak and \"quotes\"",
                "escapes decoded into the arena");
        check(doc.at("deep").at(0).at(0).at(0).at(0).at(0).as<std::int64_t>() == 42, "nesting");
        check_equal(doc.at("mixed").as_array().size(), std::size_t { 5 }, "an array of mixed kinds");

        // Duplicates are coalesced inside the arena run, which only ever shrinks it.
        check(doc.at("dup").as<std::int64_t>() == 2, "the last member under a name wins");
        check_equal(doc.size(), std::size_t { 7 }, "and the duplicate is gone from the count");

        check(doc.blocks() >= 1, "the arena holds at least one block");
    }

    {
        // Moving hands over the chunks without moving them, so the root still points at them.
        auto parsed = json::parse_document(sample);
        document moved = std::move(*parsed);
        check_equal(moved.at("name").as<std::string_view>().value_or(""),
                "a name long enough that it cannot sit inside the node", "a moved document reads the same");
        check(moved.at("deep").at(0).at(0).at(0).at(0).at(0).as<std::int64_t>() == 42, "to the bottom");

        auto another = json::parse_document(std::string { R"({"x":1})" });
        check(another.has_value(), "a second document");
        *another = std::move(moved);
        check(another->at("nested").at("a").at(2).as<std::int64_t>() == 3, "move-assigned over, and still read");
    }

    {
        // A copy out of a document owns everything, so it outlives the arena it came from.
        value survivor;
        {
            auto parsed = json::parse_document(sample);
            check(parsed.has_value(), "a document to copy out of");
            survivor = parsed->owned();
        }
        check_equal(survivor.at("name").as<std::string_view>().value_or(""),
                "a name long enough that it cannot sit inside the node", "an owned copy outlives its document");
        check(!survivor.at("name").is_borrowed(), "and borrows nothing");
        check(survivor.at("nested").at("a").at(2).as<std::int64_t>() == 3, "all the way down");
    }

    {
        // The same text, both ways, must give the same tree.
        const auto owning = json::decode<value>(sample);
        auto borrowing = json::parse_document(sample);
        check(owning.has_value() && borrowing.has_value(), "both trees built");
        check(owning && borrowing && *owning == borrowing->root(), "a borrowed tree equals the owned one");
    }

    {
        // Writing to a borrowed container promotes it, leaving the arena's copy alone.
        auto parsed = json::parse_document(sample);
        value copy = parsed->owned();
        const auto items = copy.at("nested").at("a").as_array();
        check_equal(items.size(), std::size_t { 3 }, "the copy has the elements");
        value &array = copy["nested"]["a"];
        check(!array.is_borrowed(), "an owned copy is not borrowed");
        array.push_back(value { 4 });
        check_equal(array.size(), std::size_t { 4 }, "and can be grown");
        check_equal(parsed->at("nested").at("a").as_array().size(), std::size_t { 3 },
                "while the document it came from is unchanged");
    }

    {
        check(!json::parse_document(std::string { "{} x" }).has_value(), "trailing data is refused");
        check(!json::parse_document(std::string { R"({"a":})" }).has_value(), "a missing value is refused");
        check(!json::parse_document(std::string { "[1,2" }).has_value(), "an unterminated array is refused");
        check(!json::parse_document(std::string { "" }).has_value(), "empty text is refused");
        check(json::parse_document(std::string { "42" }).has_value(), "a bare scalar is a document");
        check(json::parse_document(std::string { "[]" })->root().as_array().empty(), "so is an empty array");
        check(json::parse_document(std::string { R"("just a string")" })->root().is_string(), "so is a string");
    }

    return report("document");
}
