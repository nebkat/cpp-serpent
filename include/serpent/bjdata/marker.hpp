#pragma once

#include <serpent/limits.hpp>

#include <array>
#include <bit>
#include <concepts>
#include <type_traits>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <initializer_list>

namespace serpent::bjdata {

/**
 * @brief BJData type markers.
 *
 * Values are the specification's ASCII bytes. Multi-byte numeric payloads are always
 * little-endian, which is the defining divergence from UBJSON.
 */
enum class marker : unsigned char {
    invalid = 0,

    null = 'Z',
    boolean_true = 'T',
    boolean_false = 'F',
    noop = 'N',

    uint8 = 'U',
    int8 = 'i',
    uint16 = 'u',
    int16 = 'I',
    uint32 = 'm',
    int32 = 'l',
    uint64 = 'M',
    int64 = 'L',

    float16 = 'h',
    float32 = 'd',
    float64 = 'D',

    character = 'C',
    byte = 'B',
    string = 'S',
    high_precision = 'H',

    array_begin = '[',
    array_end = ']',
    object_begin = '{',
    object_end = '}',
    strong_type = '$',
    count = '#',

    extension = 'E',
};

/**
 * The valid markers as two bit masks, derived from the enum rather than written out.
 *
 * Recognising one is a membership test on a sparse set of ASCII bytes. A switch compiles to a
 * jump table and a bounds check, which is a load and a branch per call, and this is called
 * several times for every member of every object.
 */
inline constexpr struct marker_set {
    std::uint64_t low = 0; ///< bytes 0x20 - 0x5f
    std::uint64_t high = 0; ///< bytes 0x60 - 0x9f

    constexpr void add(marker value) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x60)
            this->low |= std::uint64_t { 1 } << (byte - 0x20);
        else
            this->high |= std::uint64_t { 1 } << (byte - 0x60);
    }
} valid_markers = [] {
    marker_set set;
    for (const marker value : { marker::null, marker::boolean_true, marker::boolean_false, marker::noop, marker::uint8,
                 marker::int8, marker::uint16, marker::int16, marker::uint32, marker::int32, marker::uint64,
                 marker::int64, marker::float16, marker::float32, marker::float64, marker::character, marker::byte,
                 marker::string, marker::high_precision, marker::array_begin, marker::array_end, marker::object_begin,
                 marker::object_end, marker::strong_type, marker::count, marker::extension })
        set.add(value);
    return set;
}();

[[nodiscard]] constexpr marker to_marker(std::byte value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    const auto index = static_cast<unsigned>(byte) - 0x20u;
    if (index >= 0x80u) return marker::invalid;
    const std::uint64_t mask = index < 64 ? valid_markers.low >> index : valid_markers.high >> (index - 64);
    return (mask & 1) != 0 ? static_cast<marker>(value) : marker::invalid;
}

/** Width of a marker's payload in bytes, or variable_width when it is length-prefixed. */
inline constexpr std::size_t variable_width = static_cast<std::size_t>(-1);

[[nodiscard]] constexpr std::size_t payload_width(marker value) noexcept {
    switch (value) {
    case marker::null:
    case marker::boolean_true:
    case marker::boolean_false:
    case marker::noop: return 0;
    case marker::uint8:
    case marker::int8:
    case marker::character:
    case marker::byte: return 1;
    case marker::uint16:
    case marker::int16:
    case marker::float16: return 2;
    case marker::uint32:
    case marker::int32:
    case marker::float32: return 4;
    case marker::uint64:
    case marker::int64:
    case marker::float64: return 8;
    default: return variable_width;
    }
}

/** A marker that opens a value, as opposed to a structural or reserved one. */
[[nodiscard]] constexpr bool is_value(marker value) noexcept {
    switch (value) {
    case marker::null:
    case marker::boolean_true:
    case marker::boolean_false:
    case marker::uint8:
    case marker::int8:
    case marker::uint16:
    case marker::int16:
    case marker::uint32:
    case marker::int32:
    case marker::uint64:
    case marker::int64:
    case marker::float16:
    case marker::float32:
    case marker::float64:
    case marker::character:
    case marker::byte:
    case marker::string:
    case marker::high_precision:
    case marker::array_begin:
    case marker::object_begin: return true;
    default: return false;
    }
}

/** The eight integer markers, which are also exactly the legal length and count prefixes. */
[[nodiscard]] constexpr bool is_integer(marker value) noexcept {
    switch (value) {
    case marker::uint8:
    case marker::int8:
    case marker::uint16:
    case marker::int16:
    case marker::uint32:
    case marker::int32:
    case marker::uint64:
    case marker::int64: return true;
    default: return false;
    }
}

[[nodiscard]] constexpr bool is_signed_integer(marker value) noexcept {
    switch (value) {
    case marker::int8:
    case marker::int16:
    case marker::int32:
    case marker::int64: return true;
    default: return false;
    }
}

[[nodiscard]] constexpr bool is_float(marker value) noexcept {
    return value == marker::float16 || value == marker::float32 || value == marker::float64;
}

/**
 * Markers legal after '$'. Note that S, H, Z, T and F are excluded: a strong type must be
 * fixed width, so a typed container is always O(1) to index and to skip.
 */
[[nodiscard]] constexpr bool is_strong_type(marker value) noexcept {
    return is_integer(value) || is_float(value) || value == marker::character || value == marker::byte;
}

/** Decodes an IEEE-754 binary16 bit pattern. Written out rather than relying on _Float16. */
[[nodiscard]] constexpr float decode_float16(std::uint16_t bits) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 15) << 31;
    const std::uint32_t exponent = (bits >> 10) & 0x1Fu;
    const std::uint32_t mantissa = bits & 0x3FFu;

    if (exponent == 0) {
        if (mantissa == 0) return std::bit_cast<float>(sign);

        std::uint32_t shift = 0;
        std::uint32_t value = mantissa;
        while ((value & 0x400u) == 0) {
            value <<= 1;
            ++shift;
        }
        return std::bit_cast<float>(sign | ((113u - shift) << 23) | ((value & 0x3FFu) << 13));
    }

    if (exponent == 0x1Fu) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));

    return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
}

/** `value >> shift`, rounded to nearest with ties going to the even value. */
[[nodiscard]] constexpr std::uint32_t shift_round_to_nearest_even(std::uint32_t value, std::uint32_t shift) noexcept {
    if (shift > 24) return 0;
    const std::uint32_t quotient = value >> shift;
    const std::uint32_t remainder = value & ((1u << shift) - 1u);
    const std::uint32_t half = 1u << (shift - 1u);
    if (remainder > half || (remainder == half && (quotient & 1u) != 0)) return quotient + 1u;
    return quotient;
}

/**
 * Encodes an IEEE-754 binary16 bit pattern, rounding to nearest even.
 *
 * The inverse of decode_float16, and it must stay exactly so: fits_float16 is defined as the
 * composition of the two, which is what decides whether a value may be narrowed.
 */
[[nodiscard]] constexpr std::uint16_t encode_float16(float value) noexcept {
    std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits & 0x80000000u) >> 16;
    bits &= 0x7FFFFFFFu;

    // Inf, NaN, or a magnitude too large for binary16.
    if (bits >= 0x47800000u) return static_cast<std::uint16_t>(sign | (bits > 0x7F800000u ? 0x7E00u : 0x7C00u));

    if (bits >= 0x38800000u) {
        const std::uint32_t mantissa_odd = (bits >> 13) & 1u;
        return static_cast<std::uint16_t>(sign | ((bits - 0x38000000u + 0xFFFu + mantissa_odd) >> 13));
    }

    // Subnormal binary16, or an underflow to signed zero. A binary32 that is itself subnormal
    // is far below the binary16 subnormal range.
    const std::uint32_t exponent = bits >> 23;
    if (exponent == 0) return static_cast<std::uint16_t>(sign);
    return static_cast<std::uint16_t>(
            sign | shift_round_to_nearest_even(0x800000u | (bits & 0x7FFFFFu), 126u - exponent));
}

/** Whether a value survives a trip through binary32 unchanged. NaN counts as surviving. */
[[nodiscard]] constexpr bool fits_float32(double value) noexcept {
    return value != value || static_cast<double>(static_cast<float>(value)) == value;
}

/** Whether a value survives a trip through binary16 unchanged. NaN counts as surviving. */
[[nodiscard]] constexpr bool fits_float16(double value) noexcept {
    return value != value || static_cast<double>(decode_float16(encode_float16(static_cast<float>(value)))) == value;
}

/**
 * The narrowest integer marker holding every value in [minimum, maximum].
 *
 * First match wins and unsigned is preferred at every width, so 200 is U and never i. A
 * single value is integer_marker(v, v).
 */
[[nodiscard]] constexpr marker integer_marker(std::int64_t minimum, std::int64_t maximum) noexcept {
    if (minimum >= 0 && maximum <= 255) return marker::uint8;
    if (minimum >= -128 && maximum <= 127) return marker::int8;
    if (minimum >= 0 && maximum <= 65535) return marker::uint16;
    if (minimum >= -32768 && maximum <= 32767) return marker::int16;
    if (minimum >= 0 && maximum <= 4294967295LL) return marker::uint32;
    if (minimum >= -2147483648LL && maximum <= 2147483647LL) return marker::int32;
    if (minimum >= 0) return marker::uint64;
    return marker::int64;
}

/** The minimum >= 0 branch of the ladder, for values that do not fit a signed 64-bit range. */
[[nodiscard]] constexpr marker integer_marker(std::uint64_t maximum) noexcept {
    if (maximum <= 255) return marker::uint8;
    if (maximum <= 65535) return marker::uint16;
    if (maximum <= 4294967295ULL) return marker::uint32;
    return marker::uint64;
}

/** The narrowest float marker that holds every value with no change to any of them. */
[[nodiscard]] constexpr marker float_marker(bool all_fit_float16, bool all_fit_float32) noexcept {
    if (all_fit_float16) return marker::float16;
    if (all_fit_float32) return marker::float32;
    return marker::float64;
}

[[nodiscard]] constexpr marker float_marker(double value) noexcept {
    return float_marker(fits_float16(value), fits_float32(value));
}

/** Maximum rank of a dimension-array count. */
inline constexpr std::size_t max_dimensions = 8;

/** The strong type whose packed layout is exactly T, or invalid when there is none. */
template<typename T>
[[nodiscard]] consteval marker strong_type_for() noexcept {
    using bare = std::remove_cv_t<T>;
    if constexpr (std::same_as<bare, std::uint8_t>)
        return marker::uint8;
    else if constexpr (std::same_as<bare, std::int8_t>)
        return marker::int8;
    else if constexpr (std::same_as<bare, std::uint16_t>)
        return marker::uint16;
    else if constexpr (std::same_as<bare, std::int16_t>)
        return marker::int16;
    else if constexpr (std::same_as<bare, std::uint32_t>)
        return marker::uint32;
    else if constexpr (std::same_as<bare, std::int32_t>)
        return marker::int32;
    else if constexpr (std::same_as<bare, std::uint64_t>)
        return marker::uint64;
    else if constexpr (std::same_as<bare, std::int64_t>)
        return marker::int64;
    else if constexpr (std::same_as<bare, float>)
        return marker::float32;
    else if constexpr (std::same_as<bare, double>)
        return marker::float64;
    else if constexpr (std::same_as<bare, char>)
        return marker::character;
    else if constexpr (std::same_as<bare, std::byte>)
        return marker::byte;
    else
        return marker::invalid;
}

namespace detail {

/**
 * A key exactly as it appears on the wire: its length marker, its length, then its bytes.
 *
 * One definition, used by the writer to emit a constant key in a single piece and by the
 * reflected reader to recognise one without parsing it. They cannot drift apart.
 */
template<const std::string_view &Name>
inline constexpr auto encoded_key = [] {
    constexpr auto kind =
            integer_marker(static_cast<std::int64_t>(Name.size()), static_cast<std::int64_t>(Name.size()));
    std::array<std::byte, 1 + payload_width(kind) + Name.size()> bytes {};
    bytes[0] = static_cast<std::byte>(kind);
    for (std::size_t index = 0; index < payload_width(kind); ++index)
        bytes[1 + index] = static_cast<std::byte>((Name.size() >> (8 * index)) & 0xFF);
    for (std::size_t index = 0; index < Name.size(); ++index)
        bytes[1 + payload_width(kind) + index] = static_cast<std::byte>(Name[index]);
    return bytes;
}();

} // namespace detail

} // namespace serpent::bjdata
