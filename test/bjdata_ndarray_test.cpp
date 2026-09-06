// Dimension-array counts, in every form the grammar allows, viewed as a strided N-D array.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/bjdata/ndarray.hpp>

#include <string>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;

namespace {

std::vector<std::byte> storage;

view parse(std::string_view hex) {
    storage = from_hex(hex);
    return view::over(storage);
}

int element_at(const ndarray_view &array, std::size_t row, std::size_t column) {
    return array.at(row).at(column).value().as_int<int>().value_or(-1);
}

void row_major() {
    // [$U#[U2 U3] then 1..6
    const auto value = parse("5b2455235b550255035d010203040506");
    check(validate(storage).has_value(), "2x3 row-major validates");

    // The flat interface still sees the payload as it is written.
    check_equal(value.size(), std::size_t { 6 }, "flat size is the element product");
    check_equal(value[4].as_int<int>().value_or(0), 5, "flat indexing");

    const auto array = as_ndarray(value);
    check(array.has_value(), "typed dimension count yields an ndarray");
    check_equal(array->rank(), std::size_t { 2 }, "rank 2");
    check_equal(array->shape()[0], std::uint32_t { 2 }, "extent 0");
    check_equal(array->shape()[1], std::uint32_t { 3 }, "extent 1");
    check_equal(array->total(), std::size_t { 6 }, "total");
    check_equal(array->size(), std::size_t { 2 }, "leading extent");
    check(array->is_contiguous(), "row-major root is contiguous");

    check_equal(element_at(*array, 0, 0), 1, "(0,0)");
    check_equal(element_at(*array, 0, 2), 3, "(0,2)");
    check_equal(element_at(*array, 1, 0), 4, "(1,0)");
    check_equal(element_at(*array, 1, 2), 6, "(1,2)");
    check(!array->at(2).is_valid(), "row past the end");
    check(!array->at(0).at(3).is_valid(), "column past the end");

    const auto flat = array->flat<std::uint8_t>();
    check(flat.has_value(), "contiguous flat span");
    check_equal(flat->size(), std::size_t { 6 }, "flat span size");
    check_equal((*flat)[5], std::uint8_t { 6 }, "flat span back");

    // A row of a row-major array is itself contiguous.
    check(array->at(1).is_contiguous(), "row-major slice is contiguous");
    check_equal(array->at(1).flat<std::uint8_t>()->size(), std::size_t { 3 }, "row slice span");
    check_equal((*array->at(1).flat<std::uint8_t>())[0], std::uint8_t { 4 }, "row slice front");
}

void column_major() {
    // [$U#[[U2 U3]] then 1..6: the leftmost index varies fastest.
    const auto value = parse("5b2455235b5b550255035d5d010203040506");
    check(validate(storage).has_value(), "2x3 column-major validates");

    const auto array = as_ndarray(value);
    check(array.has_value(), "column-major yields an ndarray");
    check_equal(array->rank(), std::size_t { 2 }, "column-major rank");
    check_equal(array->shape()[0], std::uint32_t { 2 }, "column-major extent 0");
    check_equal(array->shape()[1], std::uint32_t { 3 }, "column-major extent 1");

    // Same logical shape as the row-major case, different strides over the same bytes.
    check_equal(element_at(*array, 0, 0), 1, "cm (0,0)");
    check_equal(element_at(*array, 0, 1), 3, "cm (0,1)");
    check_equal(element_at(*array, 0, 2), 5, "cm (0,2)");
    check_equal(element_at(*array, 1, 0), 2, "cm (1,0)");
    check_equal(element_at(*array, 1, 1), 4, "cm (1,1)");
    check_equal(element_at(*array, 1, 2), 6, "cm (1,2)");

    check(!array->is_contiguous(), "column-major is not contiguous in iteration order");
    check(!array->flat<std::uint8_t>().has_value(), "flat refuses a non-contiguous layout");
}

void dimension_forms() {
    const std::vector<int> expected { 1, 2, 3, 4, 5, 6 };

    // Optimized dimension list: $U#U2 then two raw extents, with no closing bracket.
    const auto optimized = parse("5b2455235b24552355020203010203040506");
    check(validate(storage).has_value(), "optimized dimension list validates");
    const auto first = as_ndarray(optimized);
    check(first.has_value() && first->rank() == 2, "optimized dimension list rank");
    check_equal(element_at(*first, 1, 2), 6, "optimized dimension list value");

    // Counted dimension list: #U2 then two marked extents, again with no closing bracket.
    const auto counted = parse("5b2455235b23550255025503010203040506");
    check(validate(storage).has_value(), "counted dimension list validates");
    const auto second = as_ndarray(counted);
    check(second.has_value() && second->rank() == 2, "counted dimension list rank");
    check_equal(element_at(*second, 1, 2), 6, "counted dimension list value");

    // Three dimensions, 2x2x2.
    const auto cube = parse("5b2455235b5502550255025d0102030405060708");
    const auto third = as_ndarray(cube);
    check(third.has_value() && third->rank() == 3, "rank 3");
    check_equal(third->at(1).at(0).at(1).value().as_int<int>().value_or(0), 6, "(1,0,1)");

    // A plain integer count is rank 1.
    const auto flat = as_ndarray(parse("5b2455235503010203"));
    check(flat.has_value() && flat->rank() == 1, "plain count is rank 1");
    check_equal(flat->at(2).value().as_int<int>().value_or(0), 3, "rank 1 element");
}

void malformed() {
    const auto rejected = [](std::string_view hex, errc expected, std::string_view what) {
        const auto bytes = from_hex(hex);
        const auto result = validate(bytes);
        check(!result.has_value(), what);
        if (!result) check_equal(result.error().code(), expected, what);
    };

    rejected("5b2455235b5d", errc::invalid_dimensions, "empty dimension list");
    rejected("5b2455235b69ff5d", errc::invalid_dimensions, "negative dimension");
    rejected("5b2455235b2444235502000000000000000000", errc::invalid_dimensions, "float dimension type");
    rejected("5b2455235b5b550255035d5a", errc::invalid_dimensions, "column-major wrapper not closed by ]");
    rejected("5b2455235b5b550255035d", errc::unexpected_end, "truncated column-major wrapper");
    rejected("7b23", errc::unexpected_end, "truncated object count");
    rejected("7b235b550255035d", errc::object_dimension_count, "object counted by a dimension array");

    // Rank above max_dimensions is refused rather than overflowing the extent buffer.
    std::string wide = "5b2455235b";
    for (std::size_t index = 0; index < max_dimensions + 2; ++index) wide += "5501";
    wide += "5d";
    rejected(wide, errc::dimension_overflow, "rank above max_dimensions");

    // A dimension product that overruns the buffer must not produce a span over it.
    const auto lying = parse("5b2455235b55ff55ff5d0102030405");
    check(!as_ndarray(lying).has_value(), "dimensions larger than the payload are refused");
    check(!lying.as_span<std::uint8_t>().has_value(), "oversized count yields no span");
    check(!validate(storage).has_value(), "oversized count fails validation");
}

void truncation() {
    for (const auto *hex : { "5b2455235b550255035d010203040506",
                             "5b2455235b5b550255035d5d010203040506",
                             "5b2455235b24552355020203010203040506" }) {
        const auto whole = from_hex(hex);
        for (std::size_t length = 0; length < whole.size(); ++length) {
            const auto prefix = std::span { whole }.first(length);
            check(!validate(prefix).has_value(), "truncated N-D document fails validation");

            const auto value = view::over(prefix);
            if (const auto array = as_ndarray(value)) {
                for (std::size_t row = 0; row < 4; ++row) {
                    for (std::size_t column = 0; column < 4; ++column) {
                        (void) array->at(row).at(column).value().as_int<int>();
                    }
                }
                (void) array->flat<std::uint8_t>();
            }
        }
    }
}

}// namespace

int main() {
    row_major();
    column_major();
    dimension_forms();
    malformed();
    truncation();
    return report("bjdata_ndarray");
}
