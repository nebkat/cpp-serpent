#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <type_traits>

#include <cstddef>
#include <cstdint>

static_assert(__cpp_if_consteval >= 202106L, "Requires support for if consteval");
static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big, "Mixed endianness is not supported");

namespace nonstd {

template <typename T, std::size_t Bits = sizeof(T) * 8, std::endian E = std::endian::native>
struct unaligned {
    static_assert(Bits % 8 == 0, "unaligned size must be a multiple of 8");
    static_assert(Bits > 0, "unaligned size must be greater than 0");
    static_assert(std::is_trivially_copyable_v<T>, "unaligned type must be trivially copyable");
    static_assert(!std::is_floating_point_v<T> || (Bits / 8) == sizeof(T), "unaligned floating point types must have the same size as the underlying type");

    using value_type   = T;
    using storage_type = std::array<std::byte, Bits / 8>;

    static constexpr std::size_t storage_bytes  = Bits / 8;
    static constexpr std::size_t value_bytes    = sizeof(T);
    static constexpr std::size_t min_bytes      = std::min(storage_bytes, value_bytes);

    static constexpr std::size_t storage_offset_start =
            (E == std::endian::big) ? (storage_bytes - min_bytes) : 0;
    static constexpr std::size_t storage_offset_end =
            (E == std::endian::big) ? storage_bytes : min_bytes;
    static constexpr std::size_t value_offset_start =
            (std::endian::native == std::endian::big) ? (value_bytes - min_bytes) : 0;
    static constexpr std::size_t value_offset_end =
            (std::endian::native == std::endian::big) ? value_bytes : min_bytes;

    storage_type buffer {};

    // ---------------- construction & conversion ----------------
    constexpr unaligned() = default;
    constexpr unaligned(T v) noexcept { buffer = to_storage(v); }
    constexpr unaligned& operator=(T v) noexcept { buffer = to_storage(v); return *this; }

    [[nodiscard]] constexpr operator T() const noexcept { return value(); }
    [[nodiscard]] constexpr T value() const noexcept { return from_storage(buffer); }

    // ---------------- raw access ----------------
    [[nodiscard]] constexpr std::byte*       data()       noexcept { return buffer.data(); }
    [[nodiscard]] constexpr const std::byte* data() const noexcept { return buffer.data(); }
    [[nodiscard]] constexpr operator       storage_type&() noexcept { return buffer; }
    [[nodiscard]] constexpr operator const storage_type&() const noexcept { return buffer; }
    [[nodiscard]] constexpr       storage_type& storage() noexcept { return buffer; }
    [[nodiscard]] constexpr const storage_type& storage() const noexcept { return buffer; }

    // ---------------- arithmetic/bitwise compound ops ----------------
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator+=(U y) noexcept(noexcept(std::declval<T&>() += static_cast<T>(y))) { *this = static_cast<T>(value() + static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator-=(U y) noexcept(noexcept(std::declval<T&>() -= static_cast<T>(y))) { *this = static_cast<T>(value() - static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator*=(U y) noexcept(noexcept(std::declval<T&>() *= static_cast<T>(y))) { *this = static_cast<T>(value() * static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator/=(U y) noexcept(noexcept(std::declval<T&>() /= static_cast<T>(y))) { *this = static_cast<T>(value() / static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator%=(U y) noexcept(noexcept(std::declval<T&>() %= static_cast<T>(y))) { *this = static_cast<T>(value() % static_cast<T>(y)); return *this; }

    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator&=(U y) noexcept { *this = static_cast<T>(value() & static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator|=(U y) noexcept { *this = static_cast<T>(value() | static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator^=(U y) noexcept { *this = static_cast<T>(value() ^ static_cast<T>(y)); return *this; }

    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator<<=(U y) noexcept { *this = static_cast<T>(value() << static_cast<T>(y)); return *this; }
    template <class U> requires std::convertible_to<U,T>
    constexpr unaligned& operator>>=(U y) noexcept { *this = static_cast<T>(value() >> static_cast<T>(y)); return *this; }

    // ++/--
    constexpr unaligned& operator++() noexcept { *this += static_cast<T>(1); return *this; }
    constexpr unaligned& operator--() noexcept { *this -= static_cast<T>(1); return *this; }
    [[nodiscard]] constexpr T operator++(int) noexcept { T old = value(); ++(*this); return old; }
    [[nodiscard]] constexpr T operator--(int) noexcept { T old = value(); --(*this); return old; }

    // ---------------- conversions: storage <-> T ----------------
    [[nodiscard]] static constexpr storage_type to_storage(T v) noexcept {
        // Convert to an array of bytes
        auto a = std::bit_cast<std::array<std::byte, value_bytes>>(v);

        // Trivial case: if storage and value sizes match, just return the array
        if constexpr (storage_bytes == value_bytes) {
            // Inplace reverse if opposite endianness
            if constexpr (E != std::endian::native) std::ranges::reverse(a);
            return a;
        }

        // Otherwise, we need to populate the storage array
        storage_type s {};
        if constexpr (E == std::endian::native) {
            std::copy(a.begin() + value_offset_start, a.begin() + value_offset_end, s.begin() + storage_offset_start);
        } else {
            std::reverse_copy(a.begin() + value_offset_start, a.begin() + value_offset_end, s.begin() + storage_offset_start);
        }

        // Sign extension for signed types
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && (storage_bytes > value_bytes)) {
            const bool negative = v < T(0);
            if (negative) {
                std::fill(s.begin(), s.begin() + storage_offset_start, std::byte{0xFF});
                std::fill(s.begin() + storage_offset_end, s.end(), std::byte{0xFF});
            }
        }

        return s;
    }

    [[nodiscard]] static constexpr T from_storage(const storage_type& s) noexcept {
        // Trivial case: if storage and value sizes and endianness match
        if constexpr (storage_bytes == value_bytes && E == std::endian::native) return std::bit_cast<T>(s);

        // Otherwise, we need to populate the value array
        std::array<std::byte, value_bytes> a {};
        if constexpr (E == std::endian::native) {
            std::copy(s.begin() + storage_offset_start, s.begin() + storage_offset_end, a.begin() + value_offset_start);
        } else {
            std::reverse_copy(s.begin() + storage_offset_start, s.begin() + storage_offset_end, a.begin() + value_offset_start);
        }

        // Sign extension for signed types
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && (value_bytes > storage_bytes)) {
            const bool negative = [&] {
                if constexpr (E == std::endian::big) {
                    return (s.front() & std::byte { 0x80u }) == std::byte { 0x80u };
                } else {
                    return (s.back() & std::byte { 0x80u }) == std::byte { 0x80u };
                }
            }();
            if (negative) {
                std::fill(a.begin(), a.begin() + value_offset_start, std::byte{0xFF});
                std::fill(a.begin() + value_offset_end, a.end(), std::byte{0xFF});
            }
        }

        return std::bit_cast<T>(a);
    }
};

// --------------------------- convenience aliases -----------------------
using unaligned_uint8_t  = unaligned<std::uint8_t,  8,  std::endian::native>;
using unaligned_uint16_t = unaligned<std::uint16_t, 16, std::endian::native>;
using unaligned_uint24_t = unaligned<std::uint32_t, 24, std::endian::native>;
using unaligned_uint32_t = unaligned<std::uint32_t, 32, std::endian::native>;
using unaligned_uint40_t = unaligned<std::uint64_t, 40, std::endian::native>;
using unaligned_uint48_t = unaligned<std::uint64_t, 48, std::endian::native>;
using unaligned_uint56_t = unaligned<std::uint64_t, 56, std::endian::native>;
using unaligned_uint64_t = unaligned<std::uint64_t, 64, std::endian::native>;

using unaligned_int8_t  = unaligned<std::int8_t,   8,  std::endian::native>;
using unaligned_int16_t = unaligned<std::int16_t, 16, std::endian::native>;
using unaligned_int24_t = unaligned<std::int32_t, 24, std::endian::native>;
using unaligned_int32_t = unaligned<std::int32_t, 32, std::endian::native>;
using unaligned_int40_t = unaligned<std::int64_t, 40, std::endian::native>;
using unaligned_int48_t = unaligned<std::int64_t, 48, std::endian::native>;
using unaligned_int56_t = unaligned<std::int64_t, 56, std::endian::native>;
using unaligned_int64_t = unaligned<std::int64_t, 64, std::endian::native>;

using unaligned_float32_t = unaligned<float, 32, std::endian::native>;
using unaligned_float64_t = unaligned<double, 64, std::endian::native>;

using unaligned_little_uint8_t  = unaligned<std::uint8_t,  8,  std::endian::little>;
using unaligned_little_uint16_t = unaligned<std::uint16_t, 16, std::endian::little>;
using unaligned_little_uint24_t = unaligned<std::uint32_t, 24, std::endian::little>;
using unaligned_little_uint32_t = unaligned<std::uint32_t, 32, std::endian::little>;
using unaligned_little_uint40_t = unaligned<std::uint64_t, 40, std::endian::little>;
using unaligned_little_uint48_t = unaligned<std::uint64_t, 48, std::endian::little>;
using unaligned_little_uint56_t = unaligned<std::uint64_t, 56, std::endian::little>;
using unaligned_little_uint64_t = unaligned<std::uint64_t, 64, std::endian::little>;

using unaligned_little_int8_t  = unaligned<std::int8_t,   8,  std::endian::little>;
using unaligned_little_int16_t = unaligned<std::int16_t, 16, std::endian::little>;
using unaligned_little_int24_t = unaligned<std::int32_t, 24, std::endian::little>;
using unaligned_little_int32_t = unaligned<std::int32_t, 32, std::endian::little>;
using unaligned_little_int40_t = unaligned<std::int64_t, 40, std::endian::little>;
using unaligned_little_int48_t = unaligned<std::int64_t, 48, std::endian::little>;
using unaligned_little_int56_t = unaligned<std::int64_t, 56, std::endian::little>;
using unaligned_little_int64_t = unaligned<std::int64_t, 64, std::endian::little>;

using unaligned_little_float32_t = unaligned<float, 32, std::endian::little>;
using unaligned_little_float64_t = unaligned<double, 64, std::endian::little>;

using unaligned_big_uint8_t  = unaligned<std::uint8_t,  8,  std::endian::big>;
using unaligned_big_uint16_t = unaligned<std::uint16_t, 16, std::endian::big>;
using unaligned_big_uint24_t = unaligned<std::uint32_t, 24, std::endian::big>;
using unaligned_big_uint32_t = unaligned<std::uint32_t, 32, std::endian::big>;
using unaligned_big_uint40_t = unaligned<std::uint64_t, 40, std::endian::big>;
using unaligned_big_uint48_t = unaligned<std::uint64_t, 48, std::endian::big>;
using unaligned_big_uint56_t = unaligned<std::uint64_t, 56, std::endian::big>;
using unaligned_big_uint64_t = unaligned<std::uint64_t, 64, std::endian::big>;

using unaligned_big_int8_t  = unaligned<std::int8_t,   8,  std::endian::big>;
using unaligned_big_int16_t = unaligned<std::int16_t, 16, std::endian::big>;
using unaligned_big_int24_t = unaligned<std::int32_t, 24, std::endian::big>;
using unaligned_big_int32_t = unaligned<std::int32_t, 32, std::endian::big>;
using unaligned_big_int40_t = unaligned<std::int64_t, 40, std::endian::big>;
using unaligned_big_int48_t = unaligned<std::int64_t, 48, std::endian::big>;
using unaligned_big_int56_t = unaligned<std::int64_t, 56, std::endian::big>;
using unaligned_big_int64_t = unaligned<std::int64_t, 64, std::endian::big>;

using unaligned_big_float32_t = unaligned<float, 32, std::endian::big>;
using unaligned_big_float64_t = unaligned<double, 64, std::endian::big>;

template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_little = unaligned<T, Bits, std::endian::little>;

template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_big = unaligned<T, Bits, std::endian::big>;

// ------------------------------ formatting support -----------------------
template <typename T, std::size_t Bits = sizeof(T) * 8, std::endian E = std::endian::native>
auto format_as(unaligned<T, Bits, E> value) {
    return value.value();
}

}// namespace nonstd

// --------------------------- common_type specializations -----------------------
namespace std {

// unaligned <-> aligned
template <typename T, std::size_t Bits, std::endian E, class U>
struct common_type<nonstd::unaligned<T, Bits, E>, U> {
    using type = std::common_type_t<T, U>;
};

// aligned <-> unaligned
template <class T, typename U, std::size_t Bits, std::endian E>
struct common_type<T, nonstd::unaligned<U, Bits, E>> {
    using type = std::common_type_t<T, U>;
};

// unaligned <-> unaligned
template <
    typename T1, std::size_t Bits1, std::endian E1,
    typename T2, std::size_t Bits2, std::endian E2
>
struct common_type<
    nonstd::unaligned<T1, Bits1, E1>,
    nonstd::unaligned<T2, Bits2, E2>
> {
    using type = std::common_type_t<T1, T2>;
};

} // namespace std