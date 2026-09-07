#pragma once

#include <concepts>
#include <ranges>
#include <string_view>
#include <type_traits>

#include <cstddef>

namespace serpent::detail {

/** Anything a string_view can be built from, string literals and char arrays included. */
template<typename T>
concept string_like = std::convertible_to<const T &, std::string_view>;

template<typename T>
concept byte_range =
        std::ranges::input_range<T> && std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, std::byte>;

/** A keyed container whose keys are strings: an object, not an array of pairs. */
template<typename T>
concept map_like = std::ranges::input_range<T> && requires {
    typename T::key_type;
    typename T::mapped_type;
} && std::convertible_to<const typename T::key_type &, std::string_view>;

template<typename T>
concept optional_like = requires(const T &value) {
    { value.has_value() } -> std::convertible_to<bool>;
    { *value };
};

} // namespace serpent::detail
