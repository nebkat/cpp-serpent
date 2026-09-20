#pragma once

// Reading a value whose type is already known.
//
// A reader is for a caller who does not yet know what a value is: it can be asked, copied and read
// more than once. Underneath, each of its scalar reads is one of the functions here - which
// markers a type can be read from, and what the payload under each of them means - and a reader
// generated for a type, which knows what each member is, calls them with no reader in between.
//
// The loads say what a payload is worth, given its marker and how many bytes are there. The
// reads below them do the same at a cursor that stands on a payload, stepping over it when they
// take it; each returns false, leaving the cursor where it was, when the value is not one
// the type can be read from, so the caller can step over whatever is there instead.

#include <serpent/config.hpp>
#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/marker.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace serpent::bjdata::direct {

[[nodiscard]] constexpr std::optional<bool> boolean_under(marker kind) noexcept {
    if (kind == marker::boolean_true) return true;
    if (kind == marker::boolean_false) return false;
    return std::nullopt;
}

/**
 * How many bytes of payload a value took, when it was one the type could be read from.
 *
 * Each of the loads below looks at the marker once: every marker it accepts is an arm that knows
 * the type stored under it, and so the width, how to widen it and whether it fits.
 */
using payload_taken = std::optional<std::size_t>;

/** One arm of a load: the payload as `Stored`, if that many bytes are there and `into` can hold it. */
template<typename Stored, typename T>
SERPENT_ALWAYS_INLINE [[nodiscard]] inline payload_taken load_stored(
        const std::byte *payload, std::size_t available, T &into) noexcept {
    if (available < sizeof(Stored)) return std::nullopt;
    const auto stored = detail::load<Stored>(payload);
    if constexpr (std::integral<T>) {
        if (!std::in_range<T>(stored)) return std::nullopt;
    }
    into = static_cast<T>(stored);
    return sizeof(Stored);
}

/** An integer, from any integer marker whose value the type asked for can hold. */
template<std::integral T>
SERPENT_ALWAYS_INLINE [[nodiscard]] inline payload_taken load_integer(
        marker kind, const std::byte *payload, std::size_t available, T &into) noexcept {
    switch (kind) {
    case marker::uint8:
    case marker::byte: return load_stored<std::uint8_t>(payload, available, into);
    case marker::int8: return load_stored<std::int8_t>(payload, available, into);
    case marker::uint16: return load_stored<std::uint16_t>(payload, available, into);
    case marker::int16: return load_stored<std::int16_t>(payload, available, into);
    case marker::uint32: return load_stored<std::uint32_t>(payload, available, into);
    case marker::int32: return load_stored<std::int32_t>(payload, available, into);
    case marker::uint64: return load_stored<std::uint64_t>(payload, available, into);
    case marker::int64: return load_stored<std::int64_t>(payload, available, into);
    default: return std::nullopt;
    }
}

/** A real, from any real marker - and from any integer, which is how a whole number may arrive. */
template<std::floating_point T>
SERPENT_ALWAYS_INLINE [[nodiscard]] inline payload_taken load_real(
        marker kind, const std::byte *payload, std::size_t available, T &into) noexcept {
    switch (kind) {
    case marker::float64: return load_stored<double>(payload, available, into);
    case marker::float32: return load_stored<float>(payload, available, into);
    case marker::float16: {
        std::uint16_t bits = 0;
        const auto taken = load_stored<std::uint16_t>(payload, available, bits);
        if (taken) into = static_cast<T>(decode_float16(bits));
        return taken;
    }
    default: {
        std::int64_t whole = 0;
        if (kind == marker::uint64) return load_stored<std::uint64_t>(payload, available, into);
        const auto taken = load_integer(kind, payload, available, whole);
        if (taken) into = static_cast<T>(whole);
        return taken;
    }
    }
}

/** A boolean is all marker, so there is nothing at the cursor to step over. */
SERPENT_ALWAYS_INLINE [[nodiscard]] inline bool read(detail::cursor &, marker kind, bool &into) noexcept {
    const auto value = boolean_under(kind);
    if (!value) return false;
    into = *value;
    return true;
}

/** A character: the byte under a C marker, which the writer produces for a char. */
SERPENT_ALWAYS_INLINE [[nodiscard]] inline bool read(detail::cursor &scan, marker kind, char &into) noexcept {
    if (kind != marker::character || scan.remaining() < 1) return false;
    into = static_cast<char>(*scan.position);
    scan.advance(1);
    return true;
}

template<std::integral T>
    requires (!std::same_as<T, bool> && !std::same_as<T, char>)
SERPENT_ALWAYS_INLINE [[nodiscard]] inline bool read(detail::cursor &scan, marker kind, T &into) noexcept {
    const auto taken = load_integer(kind, scan.position, scan.remaining(), into);
    if (taken) scan.advance(*taken);
    return taken.has_value();
}

template<std::floating_point T>
SERPENT_ALWAYS_INLINE [[nodiscard]] inline bool read(detail::cursor &scan, marker kind, T &into) noexcept {
    const auto taken = load_real(kind, scan.position, scan.remaining(), into);
    if (taken) scan.advance(*taken);
    return taken.has_value();
}

/**
 * A string, which always owns its bytes once read: from S, from H, whose digits are text, and
 * from C, which is one character.
 *
 * The length is read once here, where reading the text through a reader and then stepping over
 * the value reads it twice.
 */
[[nodiscard]] inline bool read(detail::cursor &scan, marker kind, std::string &into) {
    if (kind == marker::character) {
        if (scan.remaining() < 1) return false;
        into.assign(1, static_cast<char>(scan.peek()));
        scan.advance(1);
        return true;
    }
    if (kind != marker::string && kind != marker::high_precision) return false;

    const auto text = detail::read_key(scan);
    if (!scan.ok()) return false;
    into.assign(text);
    return true;
}

/** A type one of the reads above handles. */
template<typename T>
concept readable = requires(detail::cursor &scan, marker kind, T &into) {
    { direct::read(scan, kind, into) } -> std::same_as<bool>;
};

} // namespace serpent::bjdata::direct
