#pragma once

#include <serpent/emitter.hpp>
#include <serpent/fwd.hpp>
#include <serpent/reflect.hpp>
#include <serpent/visitor.hpp>

#include <algorithm>
#include <concepts>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace serpent {

/** A type that names its fields once, for both directions and every format. */
template<typename T>
concept convertible_type = requires(detail::convert_probe &visitor, const T &value) { json_convert(visitor, value); };

/**
 * @brief The dispatch point for user types, specializable for types you cannot add
 *        functions to.
 *
 * Resolved in order: a single json_convert, then the to_json / from_json pair for types whose
 * two directions genuinely differ, then reflection.
 *
 * A type may only offer one of them. Silently preferring the hand-written one would leave an
 * annotation on the type doing nothing, so having both is diagnosed instead of ranked. The
 * exception is a specialization of this template, which replaces the whole thing and never
 * reaches these checks - that is what specializing it means.
 */
template<typename T, typename>
struct serializer {
    /**
     * Writes to any writer.
     *
     * to_json is found by ADL against whichever writer is passed, so a type may overload it
     * per writer or declare one `auto &` template.
     */
    template<typename Writer>
    static void write(Writer &out, const T &value) {
        static_assert(!(reflected_type<T> && (convertible_type<T> || requires { to_json(out, value); })),
                "this type is opted in to reflection and also has a hand-written conversion. The "
                "hand-written one would be used and the annotation would do nothing; remove "
                "whichever of the two you did not mean");

        if constexpr (convertible_type<T>) {
            write_visitor<Writer> visitor { out };
            const auto scope = out.object();
            json_convert(visitor, value);
        } else if constexpr (requires { to_json(out, value); }) {
            to_json(out, value);
        } else if constexpr (reflected_type<T>) {
            write_visitor<Writer> visitor { out };
            const auto scope = out.object();
            detail::reflect_convert(visitor, value);
        } else {
            static_assert(always_false<Writer>,
                    "no json_convert for this type, no to_json overload accepting this writer, "
                    "and it is not opted in to reflection; add one of those, or specialize "
                    "serpent::serializer<T>");
        }
    }

    /**
     * Writes only the members, into an object the caller has already opened.
     *
     * What lets a tag share an object with the value it names, rather than nesting it.
     */
    template<typename Writer>
    static void write_members(Writer &out, const T &value) {
        write_visitor<Writer> visitor { out };
        if constexpr (convertible_type<T>) {
            json_convert(visitor, value);
        } else if constexpr (reflection_available) {
            detail::reflect_members(visitor, value);
        } else {
            static_assert(always_false<Writer>, "this type has no members to write");
        }
    }

    /** Reads the members from an object the caller has already identified. */
    template<typename Source>
    static bool read_members(Source source, T &value) {
        if (!source.is_object()) return false;
        read_visitor<Source> visitor { source };
        if constexpr (convertible_type<T>) {
            json_convert(visitor, value);
        } else if constexpr (reflection_available) {
            detail::reflect_members(visitor, value);
        } else {
            static_assert(always_false<Source>, "this type has no members to read");
        }
        return visitor.ok();
    }

    /** Reads from any source offering the reader interface. Resolved as to_json is. */
    template<typename Source>
    static bool read(Source source, T &value) {
        static_assert(!(reflected_type<T> && (convertible_type<T> || requires { from_json(source, value); })),
                "this type is opted in to reflection and also has a hand-written conversion. The "
                "hand-written one would be used and the annotation would do nothing; remove "
                "whichever of the two you did not mean");

        if constexpr (convertible_type<T>) {
            if (!source.is_object()) return false;
            read_visitor<Source> visitor { source };
            json_convert(visitor, value);
            return visitor.ok();
        } else if constexpr (requires { from_json(source, value); }) {
            return from_json(source, value);
        } else if constexpr (reflected_type<T>) {
            if (!source.is_object()) return false;
            read_visitor<Source> visitor { source };
            detail::reflect_convert(visitor, value);
            return visitor.ok();
        } else {
            static_assert(always_false<Source>,
                    "no json_convert for this type, no from_json overload accepting this source, "
                    "and it is not opted in to reflection; add one of those, or specialize "
                    "serpent::serializer<T>");
            return false;
        }
    }
};

/**
 * Recovers a sum type by asking each alternative, in declaration order, whether the value fits.
 *
 * The first that accepts it wins, so order is the tie-break where more than one could. A
 * custom type only accepts an object and a number only accepts a number, which separates most
 * alternatives on its own; two alternatives of the same shape are decided by their order.
 */
template<typename Source, typename T>
bool read_discriminating(Source source, T &value) {
    // Outside a sum type an object with none of the expected keys decodes to defaults, which is
    // what "a missing key keeps its value" means. That answer is useless for telling
    // alternatives apart, so here an object has to name at least one member to be believed.
    constexpr bool has_members = convertible_type<T> || reflected_type<T>;
    if constexpr (has_members) {
        if (!source.is_object()) return false;
        read_visitor<Source> visitor { source };
        if constexpr (convertible_type<T>) {
            json_convert(visitor, value);
        } else {
            detail::reflect_convert(visitor, value);
        }
        return visitor.ok() && visitor.matched() > 0;
    } else {
        return read_into(source, value);
    }
}

/**
 * Whether every alternative written as an object names itself, and under the same key.
 *
 * Alternatives of other shapes are not required to: a number already says what it is.
 */
template<typename Variant, std::size_t... Index>
consteval bool all_discriminated(std::index_sequence<Index...>) {
    constexpr bool every_object_named = ((!detail::object_like<std::variant_alternative_t<Index, Variant>>
                                                 || discriminated_type<std::variant_alternative_t<Index, Variant>>)
            && ...);
    constexpr bool any_object_named = ((detail::object_like<std::variant_alternative_t<Index, Variant>>
                                               && discriminated_type<std::variant_alternative_t<Index, Variant>>)
            || ...);
    if constexpr (!every_object_named || !any_object_named) {
        return false;
    } else {
        constexpr std::string_view key = detail::first_discriminant_key<Variant>();
        return ((!discriminated_type<std::variant_alternative_t<Index, Variant>>
                        || detail::discriminant_key<std::variant_alternative_t<Index, Variant>>() == key)
                && ...);
    }
}

/**
 * Picks the alternative the document names, rather than trying each one.
 *
 * Unambiguous where trying cannot be: two types with the same members are the same shape, and
 * only a name distinguishes them. Alternatives that are not objects keep the untagged path.
 */
template<typename Source, typename Variant, std::size_t... Index>
bool read_named_alternative(Source source, Variant &value, std::index_sequence<Index...>) {
    if (!source.is_valid()) return false;

    if (source.is_object()) {
        constexpr std::string_view key = detail::first_discriminant_key<Variant>();
        if (const auto named = source[key].as_string()) {
            const auto take = [&]<std::size_t Which>() {
                using alternative = std::variant_alternative_t<Which, Variant>;
                if constexpr (!discriminated_type<alternative>) {
                    return false;
                } else {
                    if (std::string_view { *named } != detail::discriminant_name<alternative>()) return false;
                    alternative candidate {};
                    if (!read_into(source, candidate)) return false;
                    value = std::move(candidate);
                    return true;
                }
            };
            return (take.template operator()<Index>() || ...);
        }
    }

    const auto untagged = [&]<std::size_t Which>() {
        using alternative = std::variant_alternative_t<Which, Variant>;
        if constexpr (detail::object_like<alternative>) {
            return false;
        } else {
            alternative candidate {};
            if (!read_into(source, candidate)) return false;
            value = std::move(candidate);
            return true;
        }
    };
    return (untagged.template operator()<Index>() || ...);
}

/**
 * Reads a variant whose field named the key and the alternatives.
 *
 * Only the alternatives written as objects carry the name. Anything else - a number, a string,
 * an array - already says what it is, so it is recovered the way an untagged variant is.
 */
template<tagged Tag, typename Source, typename Variant, std::size_t... Index>
bool read_tagged(Source source, Variant &value, std::index_sequence<Index...>) {
    if (!source.is_valid()) return false;

    if (source.is_object()) {
        if (const auto named = source[Tag.key()].as_string()) {
            const auto take = [&]<std::size_t Which>() {
                using alternative = std::variant_alternative_t<Which, Variant>;
                if constexpr (!detail::object_like<alternative>) {
                    return false;
                } else {
                    if (std::string_view { *named } != Tag.name(Which)) return false;
                    alternative candidate {};
                    if (!serializer<alternative>::read_members(source, candidate)) return false;
                    value = std::move(candidate);
                    return true;
                }
            };
            // A name was given, so it decides. One that matches nothing is an error rather than
            // a reason to start guessing.
            return (take.template operator()<Index>() || ...);
        }
    }

    const auto untagged = [&]<std::size_t Which>() {
        using alternative = std::variant_alternative_t<Which, Variant>;
        if constexpr (detail::object_like<alternative>) {
            return false;
        } else {
            alternative candidate {};
            if (!read_into(source, candidate)) return false;
            value = std::move(candidate);
            return true;
        }
    };
    return (untagged.template operator()<Index>() || ...);
}

template<tagged Tag, typename Source, typename Variant>
bool read_into(Source source, tagged_variant<Tag, Variant> wrapper) {
    return read_tagged<Tag>(source, wrapper.target, std::make_index_sequence<std::variant_size_v<Variant>> {});
}

template<typename Source, typename Variant, std::size_t... Index>
bool read_alternative(Source source, Variant &value, std::index_sequence<Index...>) {
    if constexpr (all_discriminated<Variant>(std::index_sequence<Index...> {})) {
        return read_named_alternative(source, value, std::index_sequence<Index...> {});
    }

    const auto attempt = [&]<std::size_t Which>() {
        std::variant_alternative_t<Which, Variant> candidate {};
        if (!read_discriminating(source, candidate)) return false;
        value = std::move(candidate);
        return true;
    };
    return (attempt.template operator()<Index>() || ...);
}

/** Reads one value into a destination, handling optionals and containers along the way. */
template<typename Source, typename T>
bool read_into(Source source, T &value) {
    if constexpr (std::same_as<T, std::monostate>) {
        return source.is_valid() && source.is_null();
    } else if constexpr (detail::optional_like<T>) {
        if (!source.is_valid() || source.is_null()) {
            value.reset();
            return true;
        }
        std::remove_cvref_t<decltype(*value)> item {};
        if (!read_into(source, item)) return false;
        value = std::move(item);
        return true;
    } else if constexpr (detail::variant_like<T>) {
        return read_alternative(source, value, std::make_index_sequence<std::variant_size_v<T>> {});
    } else if constexpr (detail::byte_range<T>) {
        if constexpr (requires(T &target) { target.clear(); }) value.clear();
        if constexpr (requires { source.as_binary(); }) {
            const auto bytes = source.as_binary();
            if (!bytes) return false;
            if constexpr (requires(T &target) { target.assign(bytes->begin(), bytes->end()); }) {
                value.assign(bytes->begin(), bytes->end());
            } else {
                if (bytes->size() != std::ranges::size(value)) return false;
                std::ranges::copy(*bytes, std::ranges::begin(value));
            }
            return true;
        } else {
            // A source with no binary type of its own carries it as an array of integers.
            if (!source.is_array()) return false;
            std::size_t index = 0;
            for (const auto element : source.array()) {
                const auto octet = element.template as_int<std::uint8_t>();
                if (!octet) return false;
                if constexpr (requires(T &target) { target.push_back(std::byte {}); }) {
                    value.push_back(static_cast<std::byte>(*octet));
                } else {
                    if (index >= std::ranges::size(value)) return false;
                    *(std::ranges::begin(value) + static_cast<std::ptrdiff_t>(index)) = static_cast<std::byte>(*octet);
                }
                ++index;
            }
            if constexpr (!requires(T &target) { target.push_back(std::byte {}); }) {
                return index == std::ranges::size(value);
            }
            return true;
        }
    } else if constexpr (requires(T &target) {
                             target.clear();
                             target.emplace_back();
                         }) {
        if (!source.is_array()) return false;
        value.clear();
        // A document that states its length lets the container be sized once rather than grown.
        if constexpr (requires { source.size_hint(); } && requires(T &target) { target.reserve(std::size_t {}); }) {
            if (const auto hint = source.size_hint()) value.reserve(*hint);
        }
        for (const auto element : source.array()) {
            auto &slot = value.emplace_back();
            if (!read_into(element, slot)) return false;
        }
        return true;
    } else if constexpr (requires(T &target) {
                             target.clear();
                             target.emplace(typename T::key_type {}, typename T::mapped_type {});
                         }) {
        if (!source.is_object()) return false;
        value.clear();
        for (const auto entry : source.items()) {
            typename T::mapped_type slot {};
            if (!read_into(entry.value, slot)) return false;
            // A key that is already a view of the source is used as-is; one that has to be
            // decoded is materialised.
            if constexpr (std::constructible_from<typename T::key_type, decltype(entry.key)>) {
                value.emplace(typename T::key_type { entry.key }, std::move(slot));
            } else {
                value.emplace(typename T::key_type { entry.key_string() }, std::move(slot));
            }
        }
        return true;
    } else {
        auto found = source.template try_get<T>();
        if (!found) return false;
        value = std::move(*found);
        return true;
    }
}

} // namespace serpent

// ---------------- member listing ----------------
//
// Field names cannot be recovered without reflection, so listing them in a macro is the only
// option.

#define SERPENT_EXPAND(x) x
#define SERPENT_MEMBER(name) visitor.member(#name, value.name);

// clang-format off
#define SERPENT_PASTE1(step, v1) step(v1)
#define SERPENT_PASTE2(step, v1, v2) step(v1) SERPENT_PASTE1(step, v2)
#define SERPENT_PASTE3(step, v1, v2, v3) step(v1) SERPENT_PASTE2(step, v2, v3)
#define SERPENT_PASTE4(step, v1, v2, v3, v4) step(v1) SERPENT_PASTE3(step, v2, v3, v4)
#define SERPENT_PASTE5(step, v1, v2, v3, v4, v5) step(v1) SERPENT_PASTE4(step, v2, v3, v4, v5)
#define SERPENT_PASTE6(step, v1, v2, v3, v4, v5, v6) step(v1) SERPENT_PASTE5(step, v2, v3, v4, v5, v6)
#define SERPENT_PASTE7(step, v1, v2, v3, v4, v5, v6, v7) step(v1) SERPENT_PASTE6(step, v2, v3, v4, v5, v6, v7)
#define SERPENT_PASTE8(step, v1, v2, v3, v4, v5, v6, v7, v8) step(v1) SERPENT_PASTE7(step, v2, v3, v4, v5, v6, v7, v8)
#define SERPENT_PASTE9(step, v1, v2, v3, v4, v5, v6, v7, v8, v9) step(v1) SERPENT_PASTE8(step, v2, v3, v4, v5, v6, v7, v8, v9)
#define SERPENT_PASTE10(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10) step(v1) SERPENT_PASTE9(step, v2, v3, v4, v5, v6, v7, v8, v9, v10)
#define SERPENT_PASTE11(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11) step(v1) SERPENT_PASTE10(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11)
#define SERPENT_PASTE12(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12) step(v1) SERPENT_PASTE11(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12)
#define SERPENT_PASTE13(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13) step(v1) SERPENT_PASTE12(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13)
#define SERPENT_PASTE14(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14) step(v1) SERPENT_PASTE13(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14)
#define SERPENT_PASTE15(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) step(v1) SERPENT_PASTE14(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15)
#define SERPENT_PASTE16(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16) step(v1) SERPENT_PASTE15(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16)
#define SERPENT_PASTE17(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17) step(v1) SERPENT_PASTE16(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17)
#define SERPENT_PASTE18(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18) step(v1) SERPENT_PASTE17(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18)
#define SERPENT_PASTE19(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19) step(v1) SERPENT_PASTE18(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19)
#define SERPENT_PASTE20(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20) step(v1) SERPENT_PASTE19(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20)
#define SERPENT_PASTE21(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21) step(v1) SERPENT_PASTE20(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21)
#define SERPENT_PASTE22(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22) step(v1) SERPENT_PASTE21(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22)
#define SERPENT_PASTE23(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23) step(v1) SERPENT_PASTE22(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23)
#define SERPENT_PASTE24(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24) step(v1) SERPENT_PASTE23(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24)
#define SERPENT_PASTE25(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25) step(v1) SERPENT_PASTE24(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25)
#define SERPENT_PASTE26(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26) step(v1) SERPENT_PASTE25(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26)
#define SERPENT_PASTE27(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27) step(v1) SERPENT_PASTE26(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27)
#define SERPENT_PASTE28(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28) step(v1) SERPENT_PASTE27(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28)
#define SERPENT_PASTE29(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29) step(v1) SERPENT_PASTE28(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29)
#define SERPENT_PASTE30(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30) step(v1) SERPENT_PASTE29(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30)
#define SERPENT_PASTE31(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31) step(v1) SERPENT_PASTE30(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31)
#define SERPENT_PASTE32(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32) step(v1) SERPENT_PASTE31(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32)
// clang-format on

#define SERPENT_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, \
        _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, NAME, ...) \
    NAME
#define SERPENT_FOR_EACH(step, ...) \
    SERPENT_EXPAND(SERPENT_SELECT(__VA_ARGS__, SERPENT_PASTE32, SERPENT_PASTE31, SERPENT_PASTE30, SERPENT_PASTE29, \
            SERPENT_PASTE28, SERPENT_PASTE27, SERPENT_PASTE26, SERPENT_PASTE25, SERPENT_PASTE24, SERPENT_PASTE23, \
            SERPENT_PASTE22, SERPENT_PASTE21, SERPENT_PASTE20, SERPENT_PASTE19, SERPENT_PASTE18, SERPENT_PASTE17, \
            SERPENT_PASTE16, SERPENT_PASTE15, SERPENT_PASTE14, SERPENT_PASTE13, SERPENT_PASTE12, SERPENT_PASTE11, \
            SERPENT_PASTE10, SERPENT_PASTE9, SERPENT_PASTE8, SERPENT_PASTE7, SERPENT_PASTE6, SERPENT_PASTE5, \
            SERPENT_PASTE4, SERPENT_PASTE3, SERPENT_PASTE2, SERPENT_PASTE1)(step, __VA_ARGS__))

/**
 * Defines both directions as a free function. Place beside the type, in its namespace.
 *
 * For a type whose definition you do not control and whose fields you cannot annotate. A type
 * that is yours to annotate wants [[= serpent::serializable {}]] instead, which needs no list
 * of members and stays right when one is added.
 */
#define SERPENT_DEFINE_TYPE_NON_INTRUSIVE(Type, ...) \
    inline void json_convert(auto &visitor, ::serpent::conversion_object_t<decltype(visitor), Type> value) { \
        SERPENT_FOR_EACH(SERPENT_MEMBER, __VA_ARGS__) \
    }
