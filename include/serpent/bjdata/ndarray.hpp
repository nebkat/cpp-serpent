#pragma once

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/view.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <optional>
#include <span>

#include <cstddef>
#include <cstdint>

namespace serpent::bjdata {

/**
 * @brief A multi-dimensional view over a typed array counted by a dimension array.
 *
 * Extents and strides are materialised on construction, so peeling a dimension is pure
 * arithmetic on a pointer. That also makes the column-major form free: it is a different
 * set of strides over the same bytes rather than a permuted copy of them.
 *
 * Requires a fixed-width strong type, which is what makes the arithmetic possible; an
 * untyped container counted by dimensions is still traversable through view::array().
 */
class ndarray_view {
    marker element = marker::invalid;
    std::span<const std::byte> source {};
    const std::byte *base = nullptr;
    std::uint32_t extents[max_dimensions] {};
    std::uint32_t strides[max_dimensions] {};   ///< in elements, not bytes; bounded by the buffer size
    std::uint8_t order = 0;

    [[nodiscard]] constexpr const std::byte *limit() const noexcept { return this->source.data() + this->source.size(); }

public:
    ndarray_view() = default;

    [[nodiscard]] bool is_valid() const noexcept { return this->element != marker::invalid; }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }

    [[nodiscard]] marker element_type() const noexcept { return this->element; }
    [[nodiscard]] std::size_t rank() const noexcept { return this->order; }
    [[nodiscard]] std::span<const std::uint32_t> shape() const noexcept { return { this->extents, this->order }; }

    /** Extent of the leading dimension, i.e. how many sub-slices at() yields. */
    [[nodiscard]] std::size_t size() const noexcept { return this->order == 0 ? 1 : this->extents[0]; }

    [[nodiscard]] std::size_t total() const noexcept {
        std::size_t product = 1;
        for (std::size_t index = 0; index < this->order; ++index) product *= this->extents[index];
        return product;
    }

    /** Whether the elements are adjacent in iteration order, so flat() may alias them. */
    [[nodiscard]] bool is_contiguous() const noexcept {
        if (this->order == 0) return true;
        std::size_t expected = 1;
        for (std::size_t index = this->order; index-- > 0;) {
            if (this->strides[index] != expected) return false;
            expected *= this->extents[index];
        }
        return true;
    }

    /** Peels the leading dimension, yielding the slice at that index. */
    [[nodiscard]] ndarray_view at(std::size_t index) const noexcept {
        if (this->order == 0 || index >= this->extents[0]) return {};

        const auto width = payload_width(this->element);
        const auto *moved = this->base + index * this->strides[0] * width;
        if (moved < this->base || moved >= this->limit()) return {};

        ndarray_view slice;
        slice.element = this->element;
        slice.source = this->source;
        slice.base = moved;
        slice.order = static_cast<std::uint8_t>(this->order - 1);
        for (std::size_t position = 0; position < slice.order; ++position) {
            slice.extents[position] = this->extents[position + 1];
            slice.strides[position] = this->strides[position + 1];
        }
        return slice;
    }

    /** The scalar this slice has been narrowed down to, once every dimension is peeled. */
    [[nodiscard]] view value() const noexcept {
        if (this->order != 0 || !this->is_valid()) return {};
        return view { this->element, this->source, this->base };
    }

    /** The whole payload in place, when the slice is contiguous and T matches exactly. */
    template<typename T>
    [[nodiscard]] std::optional<nonstd::unaligned_little_span<const T>> flat() const noexcept {
        static_assert(strong_type_for<T>() != marker::invalid, "T does not correspond to a BJData strong type");
        if (!this->is_valid() || this->element != strong_type_for<T>() || !this->is_contiguous()) return std::nullopt;

        const auto width = payload_width(this->element);
        const auto count = this->total();
        if (count > static_cast<std::uint64_t>(this->limit() - this->base) / width) return std::nullopt;
        return nonstd::unaligned_little_span<const T> { this->base, count };
    }

    friend std::optional<ndarray_view> as_ndarray(const view &value) noexcept;
};

/**
 * Views a typed array as an N-D array. A plain integer count gives rank 1, a dimension
 * array gives its declared rank. Returns nullopt for an untyped or malformed container.
 */
[[nodiscard]] inline std::optional<ndarray_view> as_ndarray(const view &value) noexcept {
    if (!value.is_array()) return std::nullopt;

    const auto info = value.container_header();
    if (!info.typed() || info.body == nullptr) return std::nullopt;

    const auto width = payload_width(info.element);
    const auto *bound = value.buffer().data() + value.buffer().size();
    if (width == 0 || info.count > static_cast<std::uint64_t>(bound - info.body) / width) return std::nullopt;

    ndarray_view result;
    result.element = info.element;
    result.source = value.buffer();
    result.base = info.body;

    if (info.rank == 0) {
        result.order = 1;
        result.extents[0] = static_cast<std::uint32_t>(info.count);
        result.strides[0] = 1;
        return result;
    }

    result.order = static_cast<std::uint8_t>(info.rank);
    for (std::size_t index = 0; index < info.rank; ++index) result.extents[index] = info.extents[index];

    if (info.column_major) {
        // Leftmost dimension varies fastest; the logical shape is unchanged.
        result.strides[0] = 1;
        for (std::size_t index = 1; index < info.rank; ++index) {
            result.strides[index] = result.strides[index - 1] * result.extents[index - 1];
        }
    } else {
        result.strides[info.rank - 1] = 1;
        for (std::size_t index = info.rank - 1; index-- > 0;) {
            result.strides[index] = result.strides[index + 1] * result.extents[index + 1];
        }
    }
    return result;
}

}// namespace serpent::bjdata
