// Nesting: every reader stops walking into containers at max_depth, so a document nested past
// it is refused - by decode and try_decode alike, into a tree or a recursive type, in each
// format - rather than recursed into until the stack runs out. A document exactly at the limit
// still reads, so the readers agree with validate() about where the limit is.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/msgpack.hpp>
#include <serpent/value.hpp>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace serpent;

namespace {

/** A type whose definition recurses, so nothing about the type bounds how deep a read goes. */
struct node {
    std::vector<node> c;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), node> value) {
        visitor.member("c", value.c);
    }
};

std::vector<std::byte> bytes_of(const std::string &text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

// Arrays nested so that the innermost holds a 1 at `depth`, the root being 0.
std::string json_arrays(int depth) { return std::string(depth, '[') + "1" + std::string(depth, ']'); }
std::vector<std::byte> bjdata_arrays(int depth) {
    return bytes_of(std::string(depth, '[') + "U\x01" + std::string(depth, ']'));
}
std::vector<std::byte> msgpack_arrays(int depth) { return bytes_of(std::string(depth, '\x91') + "\x01"); }

// Objects of the recursive type, {"c":[{"c":[ ... ]}]}, `levels` of them.
std::string json_nodes(int levels) {
    std::string out;
    for (int level = 0; level < levels; ++level) out += "{\"c\":[";
    for (int level = 0; level < levels; ++level) out += "]}";
    return out;
}

template<typename Decode, typename TryDecode, typename Document>
void bounded(const char *format, Document (*arrays)(int), Decode decode, TryDecode try_decode) {
    const std::string name { format };
    check(decode(arrays(max_depth)), name + ": nesting to the limit reads");
    check(!decode(arrays(max_depth + 1)), name + ": one past it is refused");
    const auto tried = try_decode(arrays(max_depth + 1));
    check(!tried && tried.error().code() == errc::depth_exceeded, name + ": and try_decode says why");
    // Deep enough to overflow any stack a few hundred bytes a level would, were it recursed into.
    check(!decode(arrays(100'000)), name + ": a hundred thousand levels are refused, not recursed into");
    check(!try_decode(arrays(100'000)), name + ": by try_decode too");
}

void trees() {
    bounded("json", json_arrays, [](const std::string &text) { return json::decode<value>(text).has_value(); },
            [](const std::string &text) { return json::try_decode<value>(text); });
    bounded("bjdata", bjdata_arrays, [](const std::vector<std::byte> &bytes) { return bjdata::decode<value>(bytes).has_value(); },
            [](const std::vector<std::byte> &bytes) { return bjdata::try_decode<value>(bytes); });
    bounded("msgpack", msgpack_arrays, [](const std::vector<std::byte> &bytes) { return msgpack::decode<value>(bytes).has_value(); },
            [](const std::vector<std::byte> &bytes) { return msgpack::try_decode<value>(bytes); });
}

void recursive_types() {
    // Each level is an object and the array in it, two levels of the document.
    check(json::decode<node>(json_nodes(max_depth / 2)).has_value(), "a recursive type to the limit reads");
    check(!json::decode<node>(json_nodes(max_depth / 2 + 1)).has_value(), "and past it is refused");
    check(!json::decode<node>(json_nodes(50'000)).has_value(), "however deep, without recursing into it");
    check(!json::try_decode<node>(json_nodes(50'000)).has_value(), "by try_decode too");

    const auto nested = bjdata::encode(*json::decode<value>(json_nodes(max_depth / 2)));
    check(bjdata::decode<node>(nested).has_value(), "the same in BJData");
}

void walks() {
    // A walk by hand stops at the limit as a decode does: the deepest container has nothing in it.
    const auto deep = json_arrays(max_depth + 1);
    auto root = json::reader::over(deep);
    int levels = 0;
    const json::reader *at = &root;
    std::deque<json::array_iterator> path; // each level's element is referred to by the next

    while (true) {
        auto &step = path.emplace_back(*at);
        if (step == std::default_sentinel || !(*step).is_array()) break;
        at = &*step;
        ++levels;
    }
    check(levels == max_depth, "a walk reaches the limit and no further");
    check(at->nesting() == max_depth, "and says how deep it is");
}

void counts() {
    // A count the rest of the buffer cannot hold is refused at the count, before anything is
    // reserved for it: 2^31 - 1 elements in seven bytes.
    const auto claimed = bytes_of(std::string("[#l\xff\xff\xff\x7f", 7));
    const auto checked = bjdata::validate(claimed);
    check(!checked && checked.error().code() == errc::unexpected_end && checked.error().offset() == 2,
            "a count past the end of the buffer is refused where it stands");
    check(!bjdata::decode<std::vector<int>>(claimed).has_value() && !bjdata::decode<value>(claimed).has_value(),
            "and decodes as nothing");
    check(!bjdata::reader::over(claimed).size_hint().has_value(), "with no size to reserve for");

    // A typed container's elements are their width each; an object's entries a key besides.
    check(!bjdata::validate(bytes_of(std::string("[$l#U\x03\x01\x00\x00\x00\x02\x00\x00\x00", 14))), "typed: three int32 in eight bytes");
    check(bjdata::validate(bytes_of(std::string("[$l#U\x02\x01\x00\x00\x00\x02\x00\x00\x00", 14))).has_value(), "and two in eight is whole");
    check(!bjdata::validate(bytes_of(std::string("{#U\x02U\x01" "aZ", 7))), "an object's count is held to its keys too");
}

} // namespace

int main() {
    trees();
    recursive_types();
    walks();
    counts();
    return report("nesting");
}
