#pragma once

// A scalar as JSON text, written at a pointer.
//
// The one definition of what a boolean, an integer or a real looks like as text. The writer's own
// value functions use these, and so does the writer generated for a described type, which puts
// several members into room it has claimed once for all of them - so the two cannot come to
// differ about how a number is spelled.
//
// Each is given room for at least widest_text<T> characters, and returns where it stopped.

#include <serpent/real_format.hpp>

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <limits>

namespace serpent::json {

/**
 * The most characters a value of this type can take as text, or zero where there is no limit -
 * a string, a container - or where how it is written is not decided here.
 */
template<typename T>
inline constexpr std::size_t widest_text = 0;

template<>
inline constexpr std::size_t widest_text<bool> = 5; // false

/** Every digit the type can hold, one more for where digits10 rounds down, and a sign. */
template<std::integral T>
inline constexpr std::size_t widest_text<T> = std::numeric_limits<T>::digits10 + 2;

template<std::floating_point T>
inline constexpr std::size_t widest_text<T> = serpent::detail::real_text_capacity;

[[nodiscard]] inline char *write_text(char *to, bool value) noexcept {
    if (value) {
        std::memcpy(to, "true", 4);
        return to + 4;
    }
    std::memcpy(to, "false", 5);
    return to + 5;
}

template<std::integral T>
    requires (!std::same_as<T, bool>)
[[nodiscard]] char *write_text(char *to, T value) noexcept {
    return std::to_chars(to, to + widest_text<T>, value).ptr;
}

/** JSON has no NaN or infinity, so a value that is not finite is written as null. */
template<std::floating_point T>
[[nodiscard]] char *write_text(char *to, T value) noexcept {
    if (!std::isfinite(value)) {
        std::memcpy(to, "null", 4);
        return to + 4;
    }
    return to + serpent::detail::write_real(to, static_cast<double>(value));
}

} // namespace serpent::json
