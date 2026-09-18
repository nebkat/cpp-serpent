// An indexed reader must answer exactly what a scanning one answers. It is a different way to
// find a value, not a different idea of what a value is - so every question is asked both ways
// and the answers compared, which is the check that would have caught the five bugs this
// library has already had from two code paths disagreeing.

#include "check.hpp"

#include <serpent/json/indexed.hpp>

#include <string>
#include <string_view>

namespace json = serpent::json;

namespace {

constexpr std::string_view document = R"({
    "name": "sunrise",
    "counts": [1, 2, 3],
    "nested": { "deep": { "deeper": [ {"leaf": true}, {"leaf": false} ] } },
    "nothing": null,
    "real": 1.5,
    "escaped\tkey": "with \"quotes\" and é"
})";

/** Everything a reader can be asked, asked of both, compared. */
template<typename Scanning, typename Indexed>
void compare(const Scanning &left, const Indexed &right, const std::string &path) {
    check(left.is_valid() == right.is_valid(), path + ": validity");
    check(left.type() == right.type(), path + ": kind");

    if (left.is_string()) {
        check(left.template as<std::string>() == right.template as<std::string>(), path + ": string");
    } else if (left.is_integer()) {
        check(left.template as<std::int64_t>() == right.template as<std::int64_t>(), path + ": integer");
    } else if (left.is_real()) {
        check(left.template as<double>() == right.template as<double>(), path + ": real");
    } else if (left.is_boolean()) {
        check(left.template as<bool>() == right.template as<bool>(), path + ": boolean");
    } else if (left.is_array()) {
        const auto scanning_range = left.array();
        auto scanning = scanning_range.begin();
        std::size_t position = 0;
        for (const auto &element : right.array()) {
            compare(*scanning, element, path + "/" + std::to_string(position));
            // and the same element reached by index rather than by walking
            compare(*scanning, right[position], path + "[" + std::to_string(position) + "]");
            ++scanning;
            ++position;
        }
        check(position == left.size(), path + ": element count");
    } else if (left.is_object()) {
        std::size_t position = 0;
        for (const auto &member : right.items()) {
            const auto key = member.key_string();
            compare(left[key], member.value, path + "/" + key);
            compare(left[key], right[key], path + ": by key " + key);
            check(member.key_is(key), path + ": key_is agrees with key_string");
            ++position;
        }
        check(position == left.size(), path + ": member count");
    }
}

} // namespace

int main() {
    const std::string text { document };
    const auto index = json::structural_index::over(text);

    check(index.size() > 0, "the index records something");
    check(index.ok(), "and says the document parsed");

    // One entry per value, which is what makes a sibling step a load: no entry stands for
    // anything but a value, and none is missing.
    const auto counted = [](const auto &value, auto &&self) -> std::size_t {
        std::size_t total = 1;
        if (value.is_array()) {
            for (const auto &child : value.array()) total += self(child, self);
        } else if (value.is_object()) {
            for (const auto &member : value.items()) total += self(member.value, self);
        }
        return total;
    };
    check_equal(counted(json::reader::over(text), counted), index.size(), "one entry per value, exactly");
    check(index.root().is_object(), "and its root is the document");

    compare(json::reader::over(text), index.root(), "");

    // A missing member and an out-of-range element are absent, not something else.
    check(!index.root()["nope"].is_valid(), "a missing member is invalid");
    check(!index.root()["counts"][9].is_valid(), "and so is an element past the end");

    // size_hint is the point of having counted already.
    check(index.root()["counts"].size_hint() == std::optional<std::size_t> { 3 }, "an array's length is known");

    // A type decodes from it, the same dispatch every reader makes.
    check(index.root()["counts"].as<std::vector<int>>() == std::vector<int> { 1, 2, 3 },
            "and a type reads out of it");

    // A document that does not parse yields an index that says so, not a partial one.
    const auto broken = json::structural_index::over(R"({"a": [1, 2)");
    check(!broken.ok() && broken.size() == 0, "an unparseable document indexes to nothing");
    check(!broken.root().is_valid(), "and its root is invalid rather than a half-built tree");

    return report("json_indexed");
}
