#pragma once

// MessagePack's format bytes: what each one announces, and how far a value under it reaches.
//
// One byte opens every value. A few hold the whole value in themselves - a small integer, a short
// string's length, a small container's count - and the rest say how many bytes follow and what
// they are. Everything after the first byte is big-endian.

#include <serpent/kind.hpp>
#include <nonstd/unaligned.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace serpent::msgpack {

/** The format bytes that stand for one thing each. The ranges - fixint, fixstr, fixarray, fixmap - are spelled out where they are used. */
namespace format {

inline constexpr std::uint8_t nil = 0xc0;
inline constexpr std::uint8_t never_used = 0xc1; ///< reserved; no value is ever written under it
inline constexpr std::uint8_t boolean_false = 0xc2;
inline constexpr std::uint8_t boolean_true = 0xc3;
inline constexpr std::uint8_t bin8 = 0xc4;
inline constexpr std::uint8_t bin16 = 0xc5;
inline constexpr std::uint8_t bin32 = 0xc6;
inline constexpr std::uint8_t ext8 = 0xc7;
inline constexpr std::uint8_t ext16 = 0xc8;
inline constexpr std::uint8_t ext32 = 0xc9;
inline constexpr std::uint8_t float32 = 0xca;
inline constexpr std::uint8_t float64 = 0xcb;
inline constexpr std::uint8_t uint8 = 0xcc;
inline constexpr std::uint8_t uint16 = 0xcd;
inline constexpr std::uint8_t uint32 = 0xce;
inline constexpr std::uint8_t uint64 = 0xcf;
inline constexpr std::uint8_t int8 = 0xd0;
inline constexpr std::uint8_t int16 = 0xd1;
inline constexpr std::uint8_t int32 = 0xd2;
inline constexpr std::uint8_t int64 = 0xd3;
inline constexpr std::uint8_t fixext1 = 0xd4;
inline constexpr std::uint8_t fixext2 = 0xd5;
inline constexpr std::uint8_t fixext4 = 0xd6;
inline constexpr std::uint8_t fixext8 = 0xd7;
inline constexpr std::uint8_t fixext16 = 0xd8;
inline constexpr std::uint8_t str8 = 0xd9;
inline constexpr std::uint8_t str16 = 0xda;
inline constexpr std::uint8_t str32 = 0xdb;
inline constexpr std::uint8_t array16 = 0xdc;
inline constexpr std::uint8_t array32 = 0xdd;
inline constexpr std::uint8_t map16 = 0xde;
inline constexpr std::uint8_t map32 = 0xdf;

inline constexpr std::uint8_t fixmap = 0x80;   ///< 0x80-0x8f: a map of up to fifteen pairs
inline constexpr std::uint8_t fixarray = 0x90; ///< 0x90-0x9f: an array of up to fifteen elements
inline constexpr std::uint8_t fixstr = 0xa0;   ///< 0xa0-0xbf: a string of up to thirty-one bytes

} // namespace format

/** The extension type MessagePack itself defines: a point in time. Every other negative type is reserved. */
inline constexpr std::int8_t timestamp_type = -1;

/** What a format byte announces, before anything after it has been read. */
enum class family : std::uint8_t {
    invalid,
    nil,
    boolean,
    unsigned_integer,
    signed_integer,
    real,
    string,
    binary,
    extension,
    array,
    map,
};

/**
 * Everything a format byte says about the value it opens.
 *
 * `inline_length` is the length or count when the byte holds it - a fixstr, a fixarray, a fixext
 * - and `length_bytes` how many big-endian bytes hold it otherwise. `fixed_bytes` is how much a
 * scalar carries after the byte, or an extension after its type.
 */
struct shape {
    family type = family::invalid;
    std::uint8_t length_bytes = 0;
    std::uint8_t fixed_bytes = 0;
    std::uint32_t inline_length = 0;
};

[[nodiscard]] constexpr shape shape_of(std::uint8_t head) noexcept {
    if (head <= 0x7f) return { family::unsigned_integer, 0, 0, 0 };
    if (head <= 0x8f) return { family::map, 0, 0, static_cast<std::uint32_t>(head & 0x0fu) };
    if (head <= 0x9f) return { family::array, 0, 0, static_cast<std::uint32_t>(head & 0x0fu) };
    if (head <= 0xbf) return { family::string, 0, 0, static_cast<std::uint32_t>(head & 0x1fu) };
    if (head >= 0xe0) return { family::signed_integer, 0, 0, 0 };
    switch (head) {
    case format::nil: return { family::nil, 0, 0, 0 };
    case format::boolean_false:
    case format::boolean_true: return { family::boolean, 0, 0, 0 };
    case format::bin8: return { family::binary, 1, 0, 0 };
    case format::bin16: return { family::binary, 2, 0, 0 };
    case format::bin32: return { family::binary, 4, 0, 0 };
    case format::ext8: return { family::extension, 1, 0, 0 };
    case format::ext16: return { family::extension, 2, 0, 0 };
    case format::ext32: return { family::extension, 4, 0, 0 };
    case format::float32: return { family::real, 0, 4, 0 };
    case format::float64: return { family::real, 0, 8, 0 };
    case format::uint8: return { family::unsigned_integer, 0, 1, 0 };
    case format::uint16: return { family::unsigned_integer, 0, 2, 0 };
    case format::uint32: return { family::unsigned_integer, 0, 4, 0 };
    case format::uint64: return { family::unsigned_integer, 0, 8, 0 };
    case format::int8: return { family::signed_integer, 0, 1, 0 };
    case format::int16: return { family::signed_integer, 0, 2, 0 };
    case format::int32: return { family::signed_integer, 0, 4, 0 };
    case format::int64: return { family::signed_integer, 0, 8, 0 };
    case format::fixext1: return { family::extension, 0, 0, 1 };
    case format::fixext2: return { family::extension, 0, 0, 2 };
    case format::fixext4: return { family::extension, 0, 0, 4 };
    case format::fixext8: return { family::extension, 0, 0, 8 };
    case format::fixext16: return { family::extension, 0, 0, 16 };
    case format::str8: return { family::string, 1, 0, 0 };
    case format::str16: return { family::string, 2, 0, 0 };
    case format::str32: return { family::string, 4, 0, 0 };
    case format::array16: return { family::array, 2, 0, 0 };
    case format::array32: return { family::array, 4, 0, 0 };
    case format::map16: return { family::map, 2, 0, 0 };
    case format::map32: return { family::map, 4, 0, 0 };
    default: return {};
    }
}

/** The JSON-level shape a family reads as. Binary is an array of bytes, as BJData's is; an extension has none. */
[[nodiscard]] constexpr kind kind_of(family value) noexcept {
    switch (value) {
    case family::nil: return kind::null;
    case family::boolean: return kind::boolean;
    case family::unsigned_integer:
    case family::signed_integer: return kind::integer;
    case family::real: return kind::real;
    case family::string: return kind::string;
    case family::binary:
    case family::array: return kind::array;
    case family::map: return kind::object;
    default: return kind::invalid;
    }
}

/** A big-endian unsigned number `width` bytes wide - one, two, four or eight - at `from`. */
[[nodiscard]] inline std::uint64_t load_big(const std::byte *from, std::size_t width) noexcept {
    switch (width) {
    case 1: return std::to_integer<std::uint8_t>(from[0]);
    case 2: {
        nonstd::unaligned_big<std::uint16_t> stored;
        std::memcpy(stored.data(), from, 2);
        return stored.value();
    }
    case 4: {
        nonstd::unaligned_big<std::uint32_t> stored;
        std::memcpy(stored.data(), from, 4);
        return stored.value();
    }
    case 8: {
        nonstd::unaligned_big<std::uint64_t> stored;
        std::memcpy(stored.data(), from, 8);
        return stored.value();
    }
    default: return 0;
    }
}

/** A value's integer, wherever it stands: in the format byte or in the bytes after it. Nothing for anything not an integer. */
struct integer_value {
    bool negative = false;      ///< `bits` is a two's-complement int64 below zero
    std::uint64_t bits = 0;
};

[[nodiscard]] inline bool load_integer(std::uint8_t head, const std::byte *payload, std::size_t available, integer_value &out) noexcept {
    if (head <= 0x7f) {
        out = { false, head };
        return true;
    }
    if (head >= 0xe0) {
        out = { true, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(head))) };
        return true;
    }
    const auto described = shape_of(head);
    if (described.type != family::unsigned_integer && described.type != family::signed_integer) return false;
    if (available < described.fixed_bytes) return false;
    const std::uint64_t raw = load_big(payload, described.fixed_bytes);
    if (described.type == family::unsigned_integer) {
        out = { false, raw };
        return true;
    }
    // Sign-extended from its own width, so that an int8 of -1 is -1 at sixty-four bits.
    const unsigned shift = 64u - 8u * described.fixed_bytes;
    const auto value = static_cast<std::int64_t>(raw << shift) >> shift;
    out = { value < 0, static_cast<std::uint64_t>(value) };
    return true;
}

} // namespace serpent::msgpack
