// The walk memo: an element already walked to its end is stepped over rather than scanned
// again. It is a cache with a single slot, so the interesting cases are the ones where it must
// *not* answer - a different document, a different value, a walk that stopped early.

#include "check.hpp"

#include <serpent/bjdata.hpp>

#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;

namespace {

struct record {
    int id = 0;
    std::string name;
    std::vector<int> tags;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), record> value) {
        visitor.member("id", value.id);
        visitor.member("name", value.name);
        visitor.member("tags", value.tags);
    }

    friend bool operator==(const record &, const record &) = default;
};

std::vector<record> sample(int count, int seed) {
    std::vector<record> values;
    for (int index = 0; index < count; ++index)
        values.push_back({ seed + index, "name-" + std::to_string(seed + index), { index, index + 1, index + 2 } });
    return values;
}

struct outer {
    std::vector<record> rows;
    int total = 0;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), outer> value) {
        visitor.member("rows", value.rows);
        visitor.member("total", value.total);
    }

    friend bool operator==(const outer &, const outer &) = default;
};

} // namespace

int main() {
    const auto values = sample(40, 100);
    const auto bytes = bjdata::encode(values);

    // The plain case the memo exists for: every record read in full, so every step to the next
    // resumes where the read finished.
    const auto back = bjdata::decode<std::vector<record>>(bytes);
    check(back.has_value() && *back == values, "a sequence of objects round-trips");

    // Reading the same document twice must give the same answer: a note left by the first walk
    // describes a value that is still there, so it is used, and it had better be right.
    const auto again = bjdata::decode<std::vector<record>>(bytes);
    check(again.has_value() && *again == values, "and again, with the first walk's notes standing");

    // A walk that reads only part of each element leaves no usable note, so stepping over the
    // rest must still scan. Every element must still be reached.
    std::size_t counted = 0;
    int last_id = 0;
    for (const auto element : bjdata::view::over(bytes).array()) {
        last_id = element["id"].as<int>().value_or(-1);
        ++counted;
    }
    check_equal(counted, values.size(), "a partial read of each element still reaches them all");
    check_equal(last_id, values.back().id, "and the last element is the last one");

    // Elements read out of order: the note is about one value and must not be taken for another.
    const auto document = bjdata::view::over(bytes);
    check_equal(document[7]["name"].as<std::string_view>().value_or(""), values[7].name, "an element by index");
    check_equal(document[3]["name"].as<std::string_view>().value_or(""), values[3].name, "an earlier one after it");
    check_equal(document[39]["name"].as<std::string_view>().value_or(""), values[39].name, "and the last");

    // Two documents walked in turn through the one ambient memo must not take each other's
    // notes. Same shape, same lengths, different contents - so a note taken for one would land
    // at a plausible position in the other and go unnoticed.
    const auto others = sample(40, 500);
    const auto other_bytes = bjdata::encode(others);
    const auto first_read = bjdata::decode<std::vector<record>>(bytes);
    const auto other_read = bjdata::decode<std::vector<record>>(other_bytes);
    const auto first_again = bjdata::decode<std::vector<record>>(bytes);
    check(first_read && *first_read == values, "the first document reads");
    check(other_read && *other_read == others, "the second reads its own contents");
    check(first_again && *first_again == values, "and the first is unaffected by the second's notes");

    // A nested container: the inner walk leaves a note about the inner value, which must not be
    // mistaken for one about the outer.
    const outer nested { sample(5, 7), 42 };
    const auto nested_bytes = bjdata::encode(nested);
    const auto nested_back = bjdata::decode<outer>(nested_bytes);
    check(nested_back.has_value() && *nested_back == nested, "a container inside an object round-trips");

    // An object whose members are read in an order the document does not use: the cursor misses,
    // every lookup restarts, and the memo must not paper over a wrong position.
    const auto one_bytes = bjdata::encode(values.at(2));
    const auto one = bjdata::view::over(one_bytes);
    check_equal(one["tags"].array().begin() == one["tags"].array().end() ? 0 : 3, 3, "a member out of order");
    check_equal(one["id"].as<int>().value_or(-1), values.at(2).id, "an earlier member after it");
    check_equal(one["name"].as<std::string_view>().value_or(""), values.at(2).name, "and the one between them");

    return report("bjdata_walking");
}
