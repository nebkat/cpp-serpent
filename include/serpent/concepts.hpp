#pragma once

#include <concepts>
#include <variant>
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

/**
 * A keyed container whose key is not a string, so it cannot be an object.
 *
 * It travels as a sequence of two-element arrays instead - the only shape available, since a
 * document's keys are text and this one's are not.
 */
template<typename T>
concept keyed_but_not_an_object = std::ranges::input_range<T> && requires {
    typename T::key_type;
    typename T::mapped_type;
} && !map_like<T>;

/** A pair, which is a two-element array wherever one is needed. */
template<typename T>
concept pair_like = requires(T &value) {
    typename std::remove_cvref_t<decltype(value.first)>;
    typename std::remove_cvref_t<decltype(value.second)>;
} && !std::ranges::input_range<T> && !requires { typename T::key_type; };

template<typename T>
concept optional_like = requires(const T &value) {
    { value.has_value() } -> std::convertible_to<bool>;
    { *value };
};

/** A container that can be cleared and grown one element at a time. */
template<typename T>
concept back_insertable = requires(T &target) {
    target.clear();
    target.emplace_back();
};

/** A keyed container that can be cleared and filled. */
template<typename T>
concept keyed_insertable = requires(T &target) {
    target.clear();
    target.emplace(typename T::key_type {}, typename T::mapped_type {});
};

/**
 * A type filled from the shape of the document alone, with no help from the type itself.
 *
 * These need no customization, so asking a reader for one must not go looking for a
 * json_convert that was never going to exist.
 */
/**
 * A sum type: one of several alternatives, which one being a run-time question.
 *
 * There is no tag on the wire. A value already says what it is, so the alternative is
 * recovered by asking which one the value fits.
 */
template<typename T>
concept variant_like = requires(const T &value) {
    std::variant_size<T>::value;
    { value.index() } -> std::convertible_to<std::size_t>;
};

/**
 * A type written as an object of its own members, rather than as a value of some built-in
 * shape. Only these can carry a tag, because only these have somewhere to put it.
 */
template<typename T>
concept object_like = std::is_class_v<T> && !string_like<T> && !optional_like<T> && !variant_like<T> && !byte_range<T>
        && !map_like<T> && !std::ranges::input_range<T>;

/**
 * How to name an element of a range while walking it.
 *
 * A container hands out references to what it holds, and those are taken as they are. A range
 * whose reference is a prvalue - std::vector<bool>'s bit proxy, or a view that computes its
 * elements - hands out something that is not the element type at all, so there the element is
 * materialised and everything downstream sees the type the range says it holds.
 */
template<typename R>
using range_element_t = std::conditional_t<std::is_reference_v<std::ranges::range_reference_t<R>>,
        std::ranges::range_reference_t<R>,
        std::ranges::range_value_t<R>>;

template<typename T>
concept structurally_readable = optional_like<T> || variant_like<T> || byte_range<T> || back_insertable<T>
        || keyed_insertable<T> || pair_like<T> || std::is_enum_v<T>;

} // namespace serpent::detail
