// Container grammar: unbounded, counted and typed arrays and objects, noop handling,
// zero-copy spans, and the strict strong-type rule. Vectors from dart-bjdata.

#include "check.hpp"

#include <serpent/bjdata.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;

static_assert(std::forward_iterator<array_iterator>);
static_assert(std::forward_iterator<member_iterator>);
static_assert(std::ranges::forward_range<array_range>);
static_assert(std::ranges::forward_range<member_range>);
static_assert(std::ranges::view<array_range>);

namespace {

std::vector<std::byte> storage;

view parse(std::string_view hex) {
    storage = from_hex(hex);
    return view::over(storage);
}

std::vector<long long> integers(const view &value) {
    std::vector<long long> result;
    for (const auto element : value.array()) result.push_back(element.as_int<long long>().value_or(-999));
    return result;
}

void arrays() {
    check(parse("5b5d").is_array(), "[] is an array");
    check_equal(parse("5b5d").size(), std::size_t { 0 }, "[] is empty");
    check(parse("5b5d").array().empty(), "[] iterates empty");

    const auto three = std::vector<long long> { 1, 2, 3 };
    check_equal(integers(parse("5b5501550255035d")), three, "[1,2,3] unbounded");
    check_equal(parse("5b5501550255035d").size(), std::size_t { 3 }, "[1,2,3] size walks");

    // Counted containers have no closing bracket, and noops do not consume the count.
    check_equal(integers(parse("5b23550355014e55024e4e5503")), three, "[#U3 with interleaved noops");
    check_equal(parse("5b23550355014e55024e4e5503").size(), std::size_t { 3 }, "counted size is the header count");
    check_equal(integers(parse("5b4e55014e55024e55034e5d")), three, "unbounded array with noops");

    const auto mixed = parse("5b5a54465501535501615d");
    check_equal(mixed.size(), std::size_t { 5 }, "heterogeneous array size");
    check(mixed[0].is_null(), "element 0 null");
    check_equal(mixed[1].as_bool().value_or(false), true, "element 1 true");
    check_equal(mixed[2].as_bool().value_or(true), false, "element 2 false");
    check_equal(mixed[3].as_int<int>().value_or(0), 1, "element 3 one");
    check_equal(mixed[4].as_string().value_or("?"), "a", "element 4 string");
    check(!mixed[5].is_valid(), "index past the end poisons");

    check(integers(parse("5b7b55016155017d5d")).size() == 1, "array of one object iterates");
    check_equal(parse("5b7b55016155017d5d")[0]["a"].as_int<int>().value_or(0), 1, "nested object through an array");
}

void objects() {
    check(parse("7b7d").is_object(), "{} is an object");
    check_equal(parse("7b7d").size(), std::size_t { 0 }, "{} is empty");

    const auto object = parse("7b5501615501550162550255016355037d");
    check_equal(object.size(), std::size_t { 3 }, "{a,b,c} size");
    check_equal(object["a"].as_int<int>().value_or(0), 1, "lookup a");
    check_equal(object["b"].as_int<int>().value_or(0), 2, "lookup b");
    check_equal(object["c"].as_int<int>().value_or(0), 3, "lookup c");
    check(!object["d"].is_valid(), "missing key poisons");

    std::string keys;
    long long sum = 0;
    for (const auto [key, value] : object.items()) {
        keys += key;
        sum += value.as_int<long long>().value_or(0);
    }
    check_equal(keys, std::string { "abc" }, "structured binding over items preserves order");
    check_equal(sum, 6ll, "structured binding values");

    // Objects skip noops whether or not they carry a strong type.
    const auto noisy = parse("7b2355034e55016155014e4e4e55016255024e5501635503");
    check_equal(noisy.size(), std::size_t { 3 }, "counted object with noops");
    check_equal(noisy["b"].as_int<int>().value_or(0), 2, "counted object lookup through noops");
    check_equal(noisy["c"].as_int<int>().value_or(0), 3, "counted object trailing member");

    // A typed object: keys stay variable width, values are raw.
    const auto typed = parse("7b2455235503550161015501620255016303");
    check_equal(typed.size(), std::size_t { 3 }, "{$U#U3 size");
    check_equal(typed["a"].as_int<int>().value_or(0), 1, "{$U# lookup a");
    check_equal(typed["c"].as_int<int>().value_or(0), 3, "{$U# lookup c");
}

void typed_arrays_and_spans() {
    const auto bytes = parse("5b2455235508ddccbbaa44332211");
    check_equal(bytes.size(), std::size_t { 8 }, "[$U#U8 size");
    check_equal(bytes[0].as_int<int>().value_or(0), 0xdd, "[$U# index 0");
    check_equal(bytes[7].as_int<int>().value_or(0), 0x11, "[$U# index 7");
    check(!bytes[8].is_valid(), "[$U# index past the end");

    const auto span = bytes.as_span<std::uint8_t>();
    check(span.has_value(), "[$U# yields a uint8 span");
    check_equal(span->size(), std::size_t { 8 }, "span size");
    check_equal((*span)[0], std::uint8_t { 0xdd }, "span front");
    check_equal((*span)[7], std::uint8_t { 0x11 }, "span back");
    check(!bytes.as_span<std::uint16_t>().has_value(), "span requires an exact marker match");
    // The span points into the source buffer rather than copying it.
    check_equal(static_cast<const void *>(span->bytes().data()),
                static_cast<const void *>(storage.data() + 6), "span aliases the source buffer");

    const auto words = parse("5b2475235503010002000300");
    const auto word_span = words.as_span<std::uint16_t>();
    check(word_span.has_value(), "[$u# yields a uint16 span");
    check_equal(word_span->size(), std::size_t { 3 }, "uint16 span size");
    check(std::ranges::equal(*word_span, std::vector<std::uint16_t> { 1, 2, 3 }), "uint16 span values");
    check_equal(std::ranges::to<std::vector<std::uint16_t>>(*word_span).at(2), std::uint16_t { 3 }, "ranges::to over a span");

    const auto binary = parse("5b2442235508ddccbbaa44332211");
    check(binary.is_binary(), "[$B# is binary");
    check_equal(binary.as_binary()->size(), std::size_t { 8 }, "binary size");
    check_equal(static_cast<int>(binary.as_binary()->front()), 0xdd, "binary front");
    // The MAC-shaped payload every fleet definition carries.
    const auto mac = parse("5b24422355063ce90e123456");
    check_equal(mac.as_binary()->size(), std::size_t { 6 }, "6 byte binary");
}

void strict_strong_types() {
    // dart-bjdata restricts $ to fixed width types, so S, H, Z, T and F are rejected.
    for (const auto *hex : { "5b2453235501015501610000", "5b245a2355030000", "5b2454235503", "5b2446235503", "5b2448235501" }) {
        const auto value = parse(hex);
        check_equal(value.size(), std::size_t { 0 }, "non fixed width strong type yields no elements");
        check(!validate(storage).has_value(), "non fixed width strong type fails validation");
        if (const auto result = validate(storage); !result) {
            check_equal(result.error().code(), errc::invalid_strong_type, "reported as invalid_strong_type");
        }
    }
}

void validation() {
    check(validate(from_hex("5b5501550255035d")).has_value(), "well formed array validates");
    check(validate(from_hex("7b5501615501550162550255016355037d")).has_value(), "well formed object validates");
    check(validate(from_hex("5b2455235508ddccbbaa44332211")).has_value(), "typed array validates");

    const auto trailing = validate(from_hex("5a5a"));
    check(!trailing.has_value(), "trailing data is rejected");
    check_equal(trailing.error().code(), errc::trailing_data, "trailing_data code");
    check_equal(trailing.error().offset(), std::size_t { 1 }, "trailing_data offset");

    const auto unterminated = validate(from_hex("5b550155"));
    check(!unterminated.has_value(), "unterminated array is rejected");

    const auto extension = validate(from_hex("45"));
    check(!extension.has_value(), "E is rejected outright");
    check_equal(extension.error().code(), errc::extension_unsupported, "extension_unsupported code");

    const auto nested_extension = validate(from_hex("5b45005d"));
    check(!nested_extension.has_value(), "E inside a container is rejected");
    check_equal(nested_extension.error().code(), errc::extension_unsupported, "nested extension_unsupported");

    // A counted container may not be followed by a stray noop.
    check(!validate(from_hex("5b235501550a4e")).has_value(), "trailing noop after a counted array");

    // Nesting deeper than max_depth is refused rather than blowing the stack.
    std::string deep;
    for (int index = 0; index < max_depth + 5; ++index) deep += "5b";
    deep += "5a";
    for (int index = 0; index < max_depth + 5; ++index) deep += "5d";
    const auto too_deep = validate(from_hex(deep));
    check(!too_deep.has_value(), "over deep nesting is rejected");
    check_equal(too_deep.error().code(), errc::depth_exceeded, "depth_exceeded code");

    std::string shallow;
    for (int index = 0; index < 8; ++index) shallow += "5b";
    shallow += "5a";
    for (int index = 0; index < 8; ++index) shallow += "5d";
    check(validate(from_hex(shallow)).has_value(), "moderate nesting is accepted");
}

void truncation() {
    // Every prefix of a good document must fail cleanly, never read out of bounds.
    for (const auto *hex : { "5b5501550255035d",
                             "7b5501615501550162550255016355037d",
                             "5b2455235508ddccbbaa44332211",
                             "5b7b55016155017d5d",
                             "5b2475235503010002000300" }) {
        const auto whole = from_hex(hex);
        for (std::size_t length = 0; length < whole.size(); ++length) {
            const auto prefix = std::span { whole }.first(length);
            check(!validate(prefix).has_value(), "a truncated document fails validation");

            // Traversal of an unvalidated truncated document must still be safe.
            const auto value = view::over(prefix);
            std::size_t seen = 0;
            for (const auto element : value.array()) {
                (void) element.as_int<long long>();
                if (++seen > 64) break;
            }
            for (const auto [key, element] : value.items()) {
                (void) key;
                (void) element.as_int<long long>();
                if (++seen > 64) break;
            }
            (void) value.size();
            (void) value["a"];
            (void) value[0];
        }
    }
}

/** The same bytes, read through the throwing tier rather than the total one. */
void checked_accessors() {
    const auto document = parse("7b55016153550568656c6c6f5501625b2455235503010203550163547d");

    // The happy paths agree with the total accessors.
    check_equal(document.at("a").string(), "hello", "at().string()");
    check_equal(document.at("b").span<std::uint8_t>().size(), std::size_t { 3 }, "at().span()");
    check_equal(document.at("b").at(2).get<int>(), 3, "at().at().get()");
    check_equal(document.at("c").get<bool>(), true, "get<bool>()");
    check_equal(document.at("a").get<std::string_view>(), "hello", "get<string_view>()");
    check_equal(document.at("b").at(0).get<double>(), 1.0, "get<double>() widens an integer");

    const auto throws = [](auto &&action, errc expected, std::string_view what) {
        try {
            action();
        } catch (const error &failure) {
            check_equal(failure.code(), expected, what);
            check(failure.what() != nullptr && *failure.what() != '\0', "the error carries a message");
            return;
        }
        check(false, what);
    };

    throws([&] { return document.at("missing"); }, errc::out_of_range, "at() on a missing key throws");
    throws([&] { return document.at("b").at(9); }, errc::out_of_range, "at() past the end throws");
    throws([&] { return document.at("b").string(); }, errc::type_mismatch, "string() on an array throws");
    throws([&] { return document.at("a").span<std::uint8_t>(); }, errc::type_mismatch, "span() on a string throws");
    throws([&] { return document.at("a").binary(); }, errc::type_mismatch, "binary() on a string throws");
    throws([&] { return document.at("a").get<int>(); }, errc::type_mismatch, "get<int>() on a string throws");

    // An error points at the offending byte.
    try {
        (void) document.at("a").get<int>();
    } catch (const error &failure) {
        check(failure.offset() > 0 && failure.offset() < storage.size(), "the error offset is inside the document");
    }

    // try_get never throws, whatever it is asked for.
    check(!document["a"].try_get<int>().has_value(), "try_get<int>() on a string is empty");
    check_equal(document["a"].try_get<std::string_view>().value_or("?"), "hello", "try_get<string_view>()");
    check(!document["missing"].try_get<bool>().has_value(), "try_get on a poisoned view is empty");

    // A value that will not fit the requested type is refused rather than truncated.
    const auto wide = parse("750001");
    check_equal(wide.get<int>(), 256, "get<int>() of u 256");
    check(!wide.try_get<std::uint8_t>().has_value(), "try_get<uint8_t>() of u 256 is empty");
    throws([&] { return wide.get<std::uint8_t>(); }, errc::type_mismatch, "a value that does not fit throws");
}

}// namespace

int main() {
    arrays();
    objects();
    checked_accessors();
    typed_arrays_and_spans();
    strict_strong_types();
    validation();
    truncation();
    return report("bjdata_container");
}
