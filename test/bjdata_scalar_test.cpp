// Scalar decoding. Hex vectors ported from dart-bjdata test/bjdata_test.dart.

#include "check.hpp"

#include <nonstd/bjdata.hpp>

#include <cmath>

using namespace nonstd::bjdata;

namespace {

view parse(std::string_view hex, std::vector<std::byte> &storage) {
    storage = from_hex(hex);
    return view::over(storage);
}

void scalars() {
    std::vector<std::byte> storage;

    check(parse("5a", storage).is_null(), "Z is null");
    check_equal(parse("54", storage).as_bool().value_or(false), true, "T is true");
    check_equal(parse("46", storage).as_bool().value_or(true), false, "F is false");

    check_equal(parse("5500", storage).as_int<int>().value_or(-1), 0, "U 0");
    check_equal(parse("55ff", storage).as_int<int>().value_or(-1), 255, "U 255");
    check_equal(parse("69ff", storage).as_int<int>().value_or(0), -1, "i -1");
    check_equal(parse("750001", storage).as_int<int>().value_or(0), 256, "u 256");
    check_equal(parse("497fff", storage).as_int<int>().value_or(0), -129, "I -129");
    check_equal(parse("6dffffffff", storage).as_int<std::uint32_t>().value_or(0), 4294967295u, "m 4294967295");
    check_equal(parse("4d0000000001000000", storage).as_int<std::uint64_t>().value_or(0), 4294967296ull, "M 4294967296");

    // Narrowing is range checked, not truncated.
    check(!parse("750001", storage).as_int<std::uint8_t>().has_value(), "u 256 does not fit uint8");
    check(!parse("69ff", storage).as_int<unsigned>().has_value(), "i -1 does not fit unsigned");
    check_equal(parse("497fff", storage).as_int<std::int64_t>().value_or(0), -129, "I -129 widens");

    check(std::abs(parse("68003c", storage).as_float<double>().value_or(0.0) - 1.0) < 1e-9, "h 1.0");
    check(std::abs(parse("44182d4454fb210940", storage).as_float<double>().value_or(0.0) - 3.141592653589793) < 1e-12, "D pi");
    // compactTypes means an integer marker may carry what was conceptually a float.
    check(std::abs(parse("5502", storage).as_float<double>().value_or(0.0) - 2.0) < 1e-12, "U 2 reads as a float");
}

void float16_edges() {
    std::vector<std::byte> storage;
    const auto half = [&](std::string_view hex) { return parse(hex, storage).as_float<float>().value_or(-999.0f); };

    check_equal(half("680000"), 0.0f, "h +0");
    check_equal(half("680080"), -0.0f, "h -0");
    check_equal(half("68003c"), 1.0f, "h 1");
    check_equal(half("6800c0"), -2.0f, "h -2");
    check_equal(half("68ff7b"), 65504.0f, "h max normal");
    check(std::abs(half("680004") - 6.103515625e-05f) < 1e-12f, "h min normal");
    check(std::abs(half("680100") - 5.960464477539063e-08f) < 1e-15f, "h min subnormal");
    check(std::abs(half("68ff03") - 6.097555160522461e-05f) < 1e-12f, "h max subnormal");
    check(std::isinf(half("68007c")), "h +inf");
    check(std::isinf(half("6800fc")) && half("6800fc") < 0, "h -inf");
    check(std::isnan(half("68007e")), "h nan");
}

void strings() {
    std::vector<std::byte> storage;

    check_equal(parse("535500", storage).as_string().value_or("?"), "", "S empty");
    check_equal(parse("53550568656c6c6f", storage).as_string().value_or("?"), "hello", "S hello");
    // The length prefix counts UTF-8 bytes, not code units.
    check_equal(parse("53550668c3a96c6c6f", storage).as_string().value_or("?"), "h\xc3\xa9llo", "S heollo utf-8");
    check_equal(parse("53550668c3a96c6c6f", storage).as_string().value_or("").size(), std::size_t { 6 }, "S utf-8 byte length");

    // High precision keeps its raw decimal digits.
    check_equal(parse("48550130", storage).as_string().value_or("?"), "0", "H 0");
    check_equal(parse("4855022d31", storage).as_string().value_or("?"), "-1", "H -1");
    check_equal(parse("4361", storage).as_string().value_or("?"), "a", "C a");

    // A key length may use any integer marker; i is as legal as U.
    check_equal(parse("53690568656c6c6f", storage).as_string().value_or("?"), "hello", "S with an i length");
    check(!parse("53440568656c6c6f", storage).as_string().has_value(), "S with a non-integer length is rejected");
    check(!parse("5369ff68656c6c6f", storage).as_string().has_value(), "S with a negative length is rejected");
}

void poisoning() {
    std::vector<std::byte> storage;

    check(!view {}.is_valid(), "a default view is invalid");
    check(!parse("", storage).is_valid(), "an empty buffer is invalid");
    check(!parse("45", storage).is_valid(), "E is not a value");
    check(!parse("ff", storage).is_valid(), "an unknown byte is not a value");

    // Poison propagates without special casing at each step.
    const auto poisoned = parse("5a", storage)["missing"][3].as_string();
    check(!poisoned.has_value(), "indexing a scalar poisons rather than throwing");

    // Truncation never reads past the bound.
    check(!parse("5355ff68", storage).as_string().has_value(), "truncated string payload");
    check(!parse("75", storage).as_int<int>().has_value(), "truncated uint16 payload");
    check(!parse("4d00000000", storage).as_int<std::uint64_t>().has_value(), "truncated uint64 payload");
}

}// namespace

int main() {
    scalars();
    float16_edges();
    strings();
    poisoning();
    return report("bjdata_scalar");
}
