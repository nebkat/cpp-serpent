// The walking reader must answer exactly what the plain one answers, always.
//
// It goes faster by leaving a note of how far a traversal got, so a step can resume rather than
// restart. The note is a memo and never the truth: every handle still knows its own position, so
// anything the memo does not describe falls back to scanning. These are the cases that would
// break if that were not so - a value read twice, two handles held at once, members taken out of
// order, a traversal abandoned half way - and each is checked against the plain reader.

#include "check.hpp"

#include <serpent/json.hpp>
#include <serpent/json/walking.hpp>

#include <string>
#include <string_view>
#include <vector>

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
void compare(Plain left, Walking right, const std::string &path) {
    check(left.is_valid() == right.is_valid(), path + ": validity");
    check(left.type() == right.type(), path + ": kind");

    if (left.is_string()) {
        check(left.as_string() == right.as_string(), path + ": string");
    } else if (left.is_integer()) {
        check(left.template as_int<std::int64_t>() == right.template as_int<std::int64_t>(), path + ": integer");
    } else if (left.is_array()) {
        auto plain = left.array().begin();
        std::size_t index = 0;
        for (auto element : right.array()) {
            compare(*plain, element, path + "/" + std::to_string(index));
            ++plain;
            ++index;
        }
        check_equal(index, left.size(), path + ": element count");
    } else if (left.is_object()) {
        std::size_t seen = 0;
        for (auto member : right.items()) {
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
    json::walk_memo memo;
    const auto walking = json::walking_reader::over(text, memo);
    const auto plain = json::reader::over(text);

    compare(plain, walking, "");

    // Read the same value twice. A consuming cursor could not; a memo must.
    check_equal(walking["rows"][0][1].as_int<int>().value_or(-1), 2, "a value read once");
    check_equal(walking["rows"][0][1].as_int<int>().value_or(-1), 2, "and read again");

    // Two handles held at once, used in the other order.
    const auto rows = walking["rows"];
    const auto after = walking["after"];
    check_equal(after.as_string().value_or("?"), std::string { "still here" }, "the second handle");
    check_equal(rows.size(), std::size_t { 4 }, "and the first is still good");

    // Members out of document order.
    check_equal(walking["after"].as_string().value_or("?"), std::string { "still here" }, "a later member first");
    check_equal(walking["name"].as_string().value_or("?"), std::string { "sunrise" }, "then an earlier one");

    // A traversal abandoned part way must leave the outer one correct.
    std::size_t rows_seen = 0;
    for (auto row : walking["rows"].array()) {
        for (auto value : row.array()) {
            (void)value;
            break; // abandon every inner walk after one element
        }
        ++rows_seen;
    }
    check_equal(rows_seen, std::size_t { 4 }, "every row is still visited when the inner walk is abandoned");

    // Abandoning the outer walk part way, then walking it again from the start.
    std::size_t first_pass = 0;
    for (auto row : walking["rows"].array()) {
        (void)row;
        if (++first_pass == 2) break;
    }
    std::size_t second_pass = 0;
    for (auto row : walking["rows"].array()) {
        (void)row;
        ++second_pass;
    }
    check_equal(second_pass, std::size_t { 4 }, "and the whole thing walks again afterwards");

    // Deeply nested, where the memo is most active.
    std::int64_t deepest = 0;
    for (auto entry : walking["escaped\tkey"].array())
        for (auto member : entry.items())
            for (auto element : member.value.array())
                for (auto inner : element.array())
                    for (auto deeper : inner.array())
                        for (auto leaf : deeper.array()) deepest = leaf.as_int<std::int64_t>().value_or(0);
    check_equal(deepest, std::int64_t { 4 }, "the deepest value, reached through six levels");

    // And a type decodes from it, because it answers what a source answers.
    const auto rows_out = walking["rows"].try_get<std::vector<std::vector<int>>>();
    check(rows_out && rows_out->size() == 4 && (*rows_out)[0][2] == 3, "a type reads out of it");

    return report("json_walking");
}
