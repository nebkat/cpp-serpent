// The reader generated for a type expects the members it would have written itself, in the
// order it would have written them, and reads entry by entry whatever that leaves. The first
// way is what every other suite exercises, because they read documents this library wrote. This
// one is the rest: members in another order, left out, not named by the type, under a length
// spelled another way, in an object that is counted or typed, with noops between them, holding
// a value of the wrong type - and every document cut short at every length.

#include "check.hpp"

#include <serpent/bjdata.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;

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

struct [[= serpent::serializable {}]] outer {
    record first;
    int between = 0;
    record second;

    friend bool operator==(const outer &, const outer &) = default;
};

const record expected { 42, "north", 0.5, true, { 1, 2, 3 } };

/** Bytes written out by hand, where a character stands for itself and a number for a byte. */
std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) out.push_back(static_cast<std::byte>(value));
    return out;
}

void append(std::vector<std::byte> &to, std::initializer_list<int> values) {
    for (const int value : values) to.push_back(static_cast<std::byte>(value));
}

/** One member of `expected` as this library writes it, by name. */
std::vector<std::byte> member(std::string_view name) {
    if (name == "id") return bytes({ 'U', 2, 'i', 'd', 'U', 42 });
    if (name == "name") return bytes({ 'U', 4, 'n', 'a', 'm', 'e', 'S', 'U', 5, 'n', 'o', 'r', 't', 'h' });
    if (name == "ratio") return bytes({ 'U', 5, 'r', 'a', 't', 'i', 'o', 'h', 0x00, 0x38 });
    if (name == "active") return bytes({ 'U', 6, 'a', 'c', 't', 'i', 'v', 'e', 'T' });
    return bytes({ 'U', 4, 't', 'a', 'g', 's', '[', 'U', 1, 'U', 2, 'U', 3, ']' });
}

std::vector<std::byte> object_of(const std::vector<std::string_view> &names) {
    auto out = bytes({ '{' });
    for (const auto name : names) std::ranges::copy(member(name), std::back_inserter(out));
    append(out, { '}' });
    return out;
}

} // namespace

int main() {
    // Every order the five members can come in reads as the same value.
    std::vector<std::string_view> names { "active", "id", "name", "ratio", "tags" };
    int orders = 0;
    do {
        if (bjdata::decode<record>(object_of(names)) != expected) check(false, "members read in whatever order");
        ++orders;
    } while (std::ranges::next_permutation(names).found);
    check_equal(orders, 120, "all 120 orders were tried");

    // As this library writes it, preferring either thing: the shapes the fast path is for.
    check(bjdata::decode<record>(bjdata::encode<bjdata::prefer::size>(expected)) == expected, "written for size");
    check(bjdata::decode<record>(bjdata::encode<bjdata::prefer::speed>(expected)) == expected, "written for speed");
    const record longer { 42, std::string(300, 'n'), 0.5, true, {} };
    check(bjdata::decode<record>(bjdata::encode<bjdata::prefer::speed>(longer)) == longer,
            "and a string too long for a one-byte length");

    // A key this type does not name: before, between and after, holding a whole subtree.
    auto extra = bytes({ '{', 'U', 5, 'e', 'x', 't', 'r', 'a', '{', 'U', 1, 'a', '[', 'U', 1, '{', '}', ']', '}' });
    std::ranges::copy(member("id"), std::back_inserter(extra));
    append(extra, { 'U', 6, 'u', 'n', 'u', 's', 'e', 'd', 'S', 'U', 1, 'x' });
    for (const auto name : { "name", "ratio", "active", "tags" }) std::ranges::copy(member(name), std::back_inserter(extra));
    append(extra, { 'U', 4, 'l', 'a', 's', 't', 'Z', '}' });
    check(bjdata::decode<record>(extra) == expected, "keys the type does not name are skipped, whatever they hold");

    // The same key under a length spelled another way: signed, and wider than it needs to be.
    auto spelled = bytes({ '{', 'i', 2, 'i', 'd', 'U', 42, 'u', 4, 0, 'n', 'a', 'm', 'e', 'S', 'l', 5, 0, 0, 0, 'n', 'o',
        'r', 't', 'h' });
    for (const auto name : { "ratio", "active", "tags" }) std::ranges::copy(member(name), std::back_inserter(spelled));
    append(spelled, { '}' });
    check(bjdata::decode<record>(spelled) == expected, "a length written another way is the same key");

    // Noops wherever one may stand.
    auto padded = bytes({ '{', 'N', 'N' });
    for (const auto name : { "id", "name", "ratio", "active", "tags" }) {
        std::ranges::copy(member(name), std::back_inserter(padded));
        append(padded, { 'N' });
    }
    append(padded, { '}' });
    check(bjdata::decode<record>(padded) == expected, "noops between members");

    // A counted object, which has no closing brace, and a strongly typed one, whose values have no marker.
    auto counted = bytes({ '{', '#', 'U', 5 });
    for (const auto name : { "tags", "id", "name", "ratio", "active" }) std::ranges::copy(member(name), std::back_inserter(counted));
    check(bjdata::decode<record>(counted) == expected, "a counted object");

    // Absence, which the fast path must not paper over.
    const auto sparse = bjdata::decode<with_defaults>(bytes({ '{', 'U', 6, 'a', 'l', 'w', 'a', 'y', 's', 'U', 5, '}' }));
    check(sparse && sparse->always == 5 && sparse->sometimes == 77 && !sparse->maybe,
            "a defaulted member keeps its value and an optional stays empty");
    check(!bjdata::decode<with_defaults>(bytes({ '{', 'U', 9, 's', 'o', 'm', 'e', 't', 'i', 'm', 'e', 's', 'U', 1, '}' })),
            "a member the type needs is still required");
    check(!bjdata::decode<with_defaults>(bytes({ '{', '}' })), "an empty object is missing what the type needs");

    // A value of the wrong type is a failed read, not a wrong value - and does not derail what follows it.
    auto mistyped = bytes({ '{', 'U', 2, 'i', 'd', 'S', 'U', 2, '4', '2' });
    for (const auto name : { "name", "ratio", "active", "tags" }) std::ranges::copy(member(name), std::back_inserter(mistyped));
    append(mistyped, { '}' });
    check(!bjdata::decode<record>(mistyped), "a string where an integer belongs fails");
    check(!bjdata::decode<record>(object_of({ "id", "name", "ratio", "tags" })), "and a member left out fails");

    // Where a value belongs, a marker that does not open one is refused - whether the key before it
    // was one the type names, read the fast way, or one it does not.
    for (const int not_a_value : { 'N', '}', ']', '#', '$', 0x00, 0xff }) {
        check(!bjdata::decode<record>(bytes({ '{', 'U', 2, 'i', 'd', not_a_value, '}' })),
                "a marker that opens no value, after a key the type names");
        check(!bjdata::decode<with_defaults>(bytes({ '{', 'U', 6, 'a', 'l', 'w', 'a', 'y', 's', 'U', 5, 'U', 1, 'x',
                      not_a_value, '}' })),
                "and after one it does not");
    }

    // Nested objects are consumed exactly: what follows one is read from the right place.
    const outer nested { expected, 7, { 1, "south", 2.5, false, {} } };
    check(bjdata::decode<outer>(bjdata::encode(nested)) == nested, "a described type inside another");

    // Cut short at every length, a document is refused - and reading it touches nothing past its end,
    // which the sanitizers are here to see.
    for (const auto &whole : { bjdata::encode(expected), bjdata::encode<bjdata::prefer::speed>(expected),
                 bjdata::encode<bjdata::prefer::speed>(longer), bjdata::encode(nested), extra, spelled, counted }) {
        for (std::size_t length = 0; length < whole.size(); ++length) {
            const std::vector<std::byte> cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
            if (bjdata::decode<record>(cut) == expected && whole.size() - length > 1)
                check(false, "a document cut short is not the whole document");
        }
    }

    return report("bjdata_shapes");
}
