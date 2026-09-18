// The reader must answer exactly what a tree built from the same document answers, always.
//
// It goes faster by leaving a note of how far a traversal of a value got, so the step past that
// value can resume rather than restart. The note is never the truth: every handle still knows
// its own position, so anything the note does not cover falls back to scanning. These are the
// cases that would break if that were not so - a value read twice, two handles held at once,
// members taken out of order, a traversal abandoned half way - each checked against the tree.

#include "check.hpp"

#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>

namespace json = serpent::json;

namespace {

constexpr std::string_view document = R"({
    "name": "sunrise",
    "rows": [[1, 2, 3], [4, 5], [], [6]],
    "nested": { "deep": { "deeper": [ {"leaf": 1}, {"leaf": 2} ] } },
    "after": "still here",
    "escaped\tkey": [ {"a": [1, [2, [3, [4]]]]} ]
})";

/** Everything, asked of both, compared. */
template<typename Plain, typename Walking>
void compare(const Plain &left, const Walking &right, const std::string &path) {
    check(left.is_valid() == right.is_valid(), path + ": validity");
    check(left.type() == right.type(), path + ": kind");

    if (left.is_string()) {
        check(left.template as<std::string>() == right.template as<std::string>(), path + ": string");
    } else if (left.is_integer()) {
        check(left.template as<std::int64_t>() == right.template as<std::int64_t>(), path + ": integer");
    } else if (left.is_array()) {
        std::size_t index = 0;
        for (const auto &element : right.array()) {
            compare(left[index], element, path + "/" + std::to_string(index));
            ++index;
        }
        check_equal(index, left.size(), path + ": element count");
    } else if (left.is_object()) {
        std::size_t seen = 0;
        for (const auto &member : right.items()) {
            const auto key = member.key_string();
            compare(left[key], member.value, path + "/" + key);
            check(member.key_is(key), path + ": key_is agrees with key_string");
            ++seen;
        }
        check_equal(seen, left.size(), path + ": member count");
    }
}

} // namespace

int main() {
    const std::string text { document };
    const auto tree = json::decode<serpent::value>(text).value();
    const serpent::value_reader plain { tree };
    const auto walking = json::reader::over(text);

    compare(plain, walking, "");

    // Read the same value twice. A consuming cursor could not; a memo must.
    check_equal(walking["rows"][0][1].as<int>().value_or(-1), 2, "a value read once");
    check_equal(walking["rows"][0][1].as<int>().value_or(-1), 2, "and read again");

    // Two handles held at once, used in the other order.
    const auto rows_handle = walking["rows"];
    const auto after = walking["after"];
    check_equal(after.as<std::string>().value_or("?"), std::string { "still here" }, "the second handle");
    check_equal(rows_handle.size(), std::size_t { 4 }, "and the first is still good");

    // Members out of document order.
    check_equal(walking["after"].as<std::string>().value_or("?"), std::string { "still here" }, "a later member first");
    check_equal(walking["name"].as<std::string>().value_or("?"), std::string { "sunrise" }, "then an earlier one");

    // A traversal abandoned part way must leave the outer one correct.
    std::size_t rows_seen = 0;
    for (const auto &row : walking["rows"].array()) {
        for (const auto &value : row.array()) {
            std::ignore = value;
            break; // abandon every inner walk after one element
        }
        ++rows_seen;
    }
    check_equal(rows_seen, std::size_t { 4 }, "every row is still visited when the inner walk is abandoned");

    // Abandoning the outer walk part way, then walking it again from the start.
    std::size_t first_pass = 0;
    for (const auto &row : walking["rows"].array()) {
        std::ignore = row;
        if (++first_pass == 2) break;
    }
    std::size_t second_pass = 0;
    for (const auto &row : walking["rows"].array()) {
        std::ignore = row;
        ++second_pass;
    }
    check_equal(second_pass, std::size_t { 4 }, "and the whole thing walks again afterwards");

    // Deeply nested, where the notes are most active.
    std::int64_t deepest = 0;
    for (const auto &entry : walking["escaped\tkey"].array())
        for (const auto &member : entry.items())
            for (const auto &element : member.value.array())
                for (const auto &inner : element.array())
                    for (const auto &deeper : inner.array())
                        for (const auto &leaf : deeper.array()) deepest = leaf.as<std::int64_t>().value_or(0);
    check_equal(deepest, std::int64_t { 4 }, "the deepest value, reached through six levels");

    // And a type decodes from it, because it answers what a source answers.
    const auto rows_out = walking["rows"].as<std::vector<std::vector<int>>>();
    check(rows_out && rows_out->size() == 4 && (*rows_out)[0][2] == 3, "a type reads out of it");

    // Two documents walked at once, their iterators interleaved, are none of each other's business.
    const std::string other = R"({"name": [[9, 9], [9]], "rows": "not an array"})";
    const auto second = json::reader::over(other);
    const auto first_rows = walking["rows"];
    const auto second_names = second["name"];
    const auto rows_range = first_rows.array();
    const auto names_range = second_names.array();
    auto rows = rows_range.begin();
    auto names = names_range.begin();
    std::size_t rows_walked = 0, names_seen = 0;
    while (rows != std::default_sentinel || names != std::default_sentinel) {
        if (rows != std::default_sentinel) {
            ++rows_walked;
            ++rows;
        }
        if (names != std::default_sentinel) {
            ++names_seen;
            ++names;
        }
    }
    check_equal(rows_walked, std::size_t { 4 }, "the first document's rows, interleaved with");
    check_equal(names_seen, std::size_t { 2 }, "the other document's names");
    check(second["rows"].is_string(), "a key that means something else in the other document");

    return report("json_walking");
}
