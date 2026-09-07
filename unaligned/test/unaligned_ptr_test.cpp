// nonstd::unaligned_ptr / unaligned_ref / unaligned_span.

#include "check.hpp"

#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
#include <iterator>
#include <numeric>
#include <ranges>
#include <vector>

using namespace nonstd;

using const_pointer = unaligned_little_ptr<const std::uint16_t>;
using mutable_pointer = unaligned_little_ptr<std::uint16_t>;

static_assert(std::random_access_iterator<const_pointer>);
static_assert(std::random_access_iterator<mutable_pointer>);
static_assert(std::output_iterator<mutable_pointer, std::uint16_t>);
static_assert(std::sortable<mutable_pointer>);
static_assert(!std::output_iterator<const_pointer, std::uint16_t>);
static_assert(std::ranges::random_access_range<unaligned_little_span<const std::uint16_t>>);
static_assert(std::ranges::view<unaligned_little_span<const std::uint16_t>>);
static_assert(std::is_trivially_copyable_v<const_pointer>);
// A proxy pointer is exactly a pointer; nothing is materialised alongside it.
static_assert(sizeof(const_pointer) == sizeof(void *));

namespace {

alignas(16) std::byte scratch[256] {};

template<typename T, std::size_t Bits = sizeof(T) * 8>
void round_trip(T value, std::string_view what) {
    // Every misalignment the target could ever present, starting at 1 so there is always
    // a preceding byte to prove the store did not spill backwards.
    for (std::size_t skew = 1; skew <= 8; ++skew) {
        std::ranges::fill(scratch, std::byte { 0xA5 });
        const unaligned_little_ptr<T, Bits> pointer { scratch + skew };
        *pointer = value;
        check_equal(static_cast<T>(*pointer), value, what);

        // Neighbouring bytes must be untouched.
        check_equal(static_cast<int>(scratch[skew - 1]), 0xA5, "byte before is intact");
        check_equal(static_cast<int>(scratch[skew + Bits / 8]), 0xA5, "byte after is intact");
    }
}

void widths() {
    round_trip<std::uint8_t>(0xC7, "uint8");
    round_trip<std::int8_t>(-42, "int8");
    round_trip<std::uint16_t>(0xBEEF, "uint16");
    round_trip<std::int16_t>(-12345, "int16");
    round_trip<std::uint32_t>(0xDEADBEEF, "uint32");
    round_trip<std::int32_t>(-1234567, "int32");
    round_trip<std::uint64_t>(0x0123456789ABCDEFull, "uint64");
    round_trip<std::int64_t>(-1234567890123456789ll, "int64");
    round_trip<float>(-2.5f, "float");
    round_trip<double>(3.141592653589793, "double");

    // The odd widths, where the storage is narrower than the value type.
    round_trip<std::uint32_t, 24>(0xABCDEF, "uint24");
    round_trip<std::int32_t, 24>(-1, "int24 sign extension");
    round_trip<std::int32_t, 24>(-8388608, "int24 minimum");
    round_trip<std::int32_t, 24>(8388607, "int24 maximum");
    round_trip<std::uint64_t, 40>(0xABCDEF0123ull, "uint40");
    round_trip<std::int64_t, 40>(-1, "int40 sign extension");
    round_trip<std::uint64_t, 48>(0xABCDEF012345ull, "uint48");
    round_trip<std::int64_t, 56>(-1, "int56 sign extension");
}

void endianness() {
    std::ranges::fill(scratch, std::byte {});
    *unaligned_little_ptr<std::uint32_t> { scratch + 1 } = 0x01020304;
    check_equal(static_cast<int>(scratch[1]), 0x04, "little endian low byte first");
    check_equal(static_cast<int>(scratch[4]), 0x01, "little endian high byte last");

    *unaligned_big_ptr<std::uint32_t> { scratch + 1 } = 0x01020304;
    check_equal(static_cast<int>(scratch[1]), 0x01, "big endian high byte first");
    check_equal(static_cast<int>(scratch[4]), 0x04, "big endian low byte last");

    // The same bytes read back through the other endianness are byte reversed.
    check_equal(static_cast<std::uint32_t>(*unaligned_little_ptr<const std::uint32_t> { scratch + 1 }),
            std::uint32_t { 0x04030201 }, "reading big endian bytes as little endian");
}

void reference_semantics() {
    std::ranges::fill(scratch, std::byte {});
    const unaligned_little_ptr<std::uint16_t> pointer { scratch + 3 };

    *pointer = 100;
    check_equal(static_cast<std::uint16_t>(*pointer), std::uint16_t { 100 }, "assign through the proxy");

    *pointer += 5;
    check_equal(static_cast<std::uint16_t>(*pointer), std::uint16_t { 105 }, "compound add through the proxy");

    ++*pointer;
    check_equal(static_cast<std::uint16_t>(*pointer), std::uint16_t { 106 }, "increment through the proxy");

    *pointer <<= 1;
    check_equal(static_cast<std::uint16_t>(*pointer), std::uint16_t { 212 }, "shift through the proxy");

    pointer[1] = 7;
    check_equal(static_cast<std::uint16_t>(pointer[1]), std::uint16_t { 7 }, "subscript assignment");
    check_equal(static_cast<std::uint16_t>(*pointer), std::uint16_t { 212 }, "subscript did not disturb element 0");

    // Mutable converts to const, exactly as T* converts to const T*.
    const unaligned_little_ptr<const std::uint16_t> readable = pointer;
    check_equal(static_cast<std::uint16_t>(*readable), std::uint16_t { 212 }, "mutable converts to const");
}

void iteration() {
    std::ranges::fill(scratch, std::byte {});
    // Deliberately misaligned so the arithmetic cannot accidentally be aligned.
    const unaligned_little_span<std::uint16_t> span { scratch + 1, 8 };

    std::iota(span.begin(), span.end(), std::uint16_t { 10 });
    check_equal(static_cast<std::uint16_t>(span[0]), std::uint16_t { 10 }, "iota front");
    check_equal(static_cast<std::uint16_t>(span[7]), std::uint16_t { 17 }, "iota back");

    check_equal(span.size(), std::size_t { 8 }, "span size");
    check_equal(span.size_bytes(), std::size_t { 16 }, "span size in bytes");
    check(!span.empty(), "span is not empty");
    check_equal(static_cast<std::uint16_t>(span.front()), std::uint16_t { 10 }, "view_interface front");
    check_equal(static_cast<std::uint16_t>(span.back()), std::uint16_t { 17 }, "view_interface back");

    check_equal(std::distance(span.begin(), span.end()), std::ptrdiff_t { 8 }, "iterator distance");
    check_equal(static_cast<std::uint16_t>(*(span.begin() + 3)), std::uint16_t { 13 }, "random access");
    check(span.begin() < span.end(), "iterator ordering");

    // Standard algorithms work through the proxy reference.
    std::ranges::reverse(span);
    check_equal(static_cast<std::uint16_t>(span[0]), std::uint16_t { 17 }, "ranges::reverse via iter_swap");
    std::ranges::sort(span);
    check_equal(static_cast<std::uint16_t>(span[0]), std::uint16_t { 10 }, "ranges::sort front");
    check_equal(static_cast<std::uint16_t>(span[7]), std::uint16_t { 17 }, "ranges::sort back");
    check_equal(std::ranges::count(span, std::uint16_t { 13 }), std::ptrdiff_t { 1 }, "ranges::count");

    const auto copied =
            std::ranges::to<std::vector<std::uint16_t>>(unaligned_little_span<const std::uint16_t> { span });
    check_equal(copied.size(), std::size_t { 8 }, "ranges::to size");
    check_equal(copied.at(4), std::uint16_t { 14 }, "ranges::to values");

    const auto doubled = std::ranges::to<std::vector<int>>(unaligned_little_span<const std::uint16_t> { span }
            | std::views::transform([](std::uint16_t v) { return v * 2; }));
    check_equal(doubled.at(0), 20, "views::transform over a span");
}

void subranges() {
    std::ranges::fill(scratch, std::byte {});
    const unaligned_little_span<std::uint16_t> span { scratch + 1, 8 };
    std::iota(span.begin(), span.end(), std::uint16_t { 0 });

    check_equal(span.subspan(2).size(), std::size_t { 6 }, "subspan size");
    check_equal(static_cast<std::uint16_t>(span.subspan(2).front()), std::uint16_t { 2 }, "subspan front");
    check_equal(span.subspan(2, 3).size(), std::size_t { 3 }, "subspan length");
    check_equal(span.first(3).size(), std::size_t { 3 }, "first");
    check_equal(static_cast<std::uint16_t>(span.last(2).front()), std::uint16_t { 6 }, "last");

    // Out of range offsets clamp rather than run off the end.
    check_equal(span.subspan(99).size(), std::size_t { 0 }, "subspan past the end clamps");
    check_equal(span.subspan(4, 99).size(), std::size_t { 4 }, "subspan length clamps");
    check_equal(span.last(99).size(), std::size_t { 8 }, "last past the start clamps");

    check_equal(span.bytes().size(), std::size_t { 16 }, "bytes span");
    check_equal(
            static_cast<const void *>(span.bytes().data()), static_cast<const void *>(scratch + 1), "bytes aliases");

    using const_span = unaligned_little_span<const std::uint16_t>;
    const std::span<const std::byte> raw { scratch + 1, 17 };
    check_equal(const_span::decode_TMP(raw).size(), std::size_t { 8 }, "decode_TMP truncates to whole elements");
    check(!const_span::try_from_bytes(raw).has_value(), "try_from_bytes rejects a partial element");
    check(const_span::try_from_bytes(raw.first(16)).has_value(), "try_from_bytes accepts an exact multiple");

    const unaligned_little_span<const std::uint16_t> readable { span };
    check_equal(readable.size(), std::size_t { 8 }, "mutable span converts to const");
}

} // namespace

int main() {
    widths();
    endianness();
    reference_semantics();
    iteration();
    subranges();
    return report("unaligned_ptr");
}
