// The total accessors must remain usable with -fno-exceptions, as embedded builds often are. Only the checked accessors
// (at, get, string, binary, span) are unavailable, and they are never instantiated here.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/value.hpp>

#include <algorithm>
#include <array>
#include <vector>
#include <serpent/bjdata/ndarray.hpp>
#include <serpent/bjdata/notation.hpp>

using namespace serpent;
using namespace serpent::bjdata;

int main() {
    const auto bytes = from_hex("7b55016153550568656c6c6f5501625b2455235503010203550163547d");

    check(validate(bytes).has_value(), "validates without exceptions");

    const auto document = view::over(bytes);
    check_equal(document["a"].as<std::string_view>().value_or("?"), "hello", "string accessor");
    check_equal(document["b"].as<nonstd::unaligned_little_span<const std::uint8_t>>()->size(), std::size_t { 3 }, "span accessor");
    check_equal(document["c"].as<bool>().value_or(false), true, "bool accessor");
    check(!document["missing"].is_valid(), "missing key poisons rather than throwing");
    check(!document["a"].as<int>().has_value(), "type mismatch yields nullopt");
    check_equal(as_ndarray(document["b"])->rank(), std::size_t { 1 }, "ndarray without exceptions");
    check(!block_notation(bytes).empty(), "block notation without exceptions");

    // The writer has no checked tier at all, so all of it must work here.
    std::vector<std::byte> produced;
    container_sink out { produced };
    writer target { out };
    {
        const auto scope = target.object();
        scope.member("a", "hello");
        scope.member("b", std::vector<int> { 1, 2, 3 });
        scope.member("c", true);
    }
    const auto finished = target.finish();
    check(finished.has_value(), "the writer finishes without exceptions");
    check(validate(produced).has_value(), "and produces a well formed document");

    // Not compared byte for byte against the input: the document above was hand-written with
    // a typed [$U#U3 array, while the writer measures three small ints and correctly finds the
    // generic form one byte smaller.
    const auto reread = view::over(produced);
    check_equal(reread["a"].as<std::string_view>().value_or("?"), "hello", "round-trips the string");
    check_equal(reread["b"].size(), std::size_t { 3 }, "round-trips the array");
    check_equal(reread["c"].as<bool>().value_or(false), true, "round-trips the boolean");

    check_equal(measure(std::vector<int> { 1, 2, 3 }), encode(std::vector<int> { 1, 2, 3 }).size(),
            "measure without exceptions");

    // A fixed buffer latches rather than throwing when it runs out.
    std::array<std::byte, 4> tiny {};
    span_sink small { tiny };
    writer bounded { small };
    bounded.value("a string that will not fit");
    check(!bounded.finish().has_value(), "a bounded sink fails without exceptions");

    // The tree too, for everything that does not have to report a failure. at() is the one
    // accessor it withholds here, as the readers do.
    value built;
    built["count"] = 2;
    built["name"] = "b";
    const auto tree = decode<value>(encode(built));
    check(tree && *tree == built, "a tree round-trips without exceptions");
    check((*tree)["missing"].is_null(), "and an absent member is null rather than a throw");

    return report("no_exceptions");
}
