#pragma once

// A type opts in and the compiler enumerates its fields, instead of a macro listing them.
//
// UNVERIFIED: the binding to std::meta needs P2996, P1306 and P3394, which no available
// toolchain implements, so it follows the papers rather than a working compiler - expect to
// adjust spellings. Everything that does not need reflection, meaning the annotations and the
// identifier-to-key conversion, sits outside the gate and is tested.

#include <serpent/visitor.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <type_traits>

#include <cstddef>

#if defined(__cpp_reflection) && __cpp_reflection >= 202411L && defined(__cpp_impl_reflection_annotations)
#define SERPENT_HAS_REFLECTION 1
#include <meta>
#else
#define SERPENT_HAS_REFLECTION 0
#endif

namespace serpent {

/** Whether this build can enumerate a type's fields for itself. */
inline constexpr bool reflection_available = SERPENT_HAS_REFLECTION != 0;

// Annotations are ordinary values, not parsed strings. Write them qualified -
// [[=serpent::key("dt")]] - which is what lets them be this short.

/** On a field: use this key instead of the identifier. */
struct key {
    std::string_view name;
};

/** On a field: leave it out of the document entirely. */
struct skip {};

/** On a type: opt in to reflected serialization. */
struct serializable {};

enum class naming_style {
    as_written,
    snake_case,
    screaming_snake_case,
    kebab_case,
    camel_case,
    pascal_case,
};

/** On a type: derive every key from the field identifier by this rule. */
struct naming {
    naming_style style = naming_style::as_written;
};

/** The opt-in for a type you cannot annotate. Specialize to true_type. */
template<typename T>
struct enable_reflection : std::false_type {};

// ---------------- key naming ----------------

namespace detail {

/** A key built during constant evaluation. Fixed capacity; a longer identifier truncates. */
struct name_buffer {
    std::array<char, 96> storage {};
    std::size_t length = 0;

    constexpr void push(char value) noexcept {
        if (this->length < this->storage.size()) this->storage[this->length++] = value;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view { this->storage.data(), this->length };
    }
};

[[nodiscard]] constexpr bool is_upper(char value) noexcept { return value >= 'A' && value <= 'Z'; }
[[nodiscard]] constexpr bool is_lower(char value) noexcept { return value >= 'a' && value <= 'z'; }
[[nodiscard]] constexpr bool is_digit(char value) noexcept { return value >= '0' && value <= '9'; }

[[nodiscard]] constexpr char lowered(char value) noexcept {
    return is_upper(value) ? static_cast<char>(value - 'A' + 'a') : value;
}
[[nodiscard]] constexpr char uppered(char value) noexcept {
    return is_lower(value) ? static_cast<char>(value - 'a' + 'A') : value;
}

/**
 * Rewrites a C++ identifier as a wire key.
 *
 * Word boundaries are an underscore or hyphen, a lower-to-upper transition, or a
 * letter-to-digit transition, so both ip_address and ipAddress split the same way.
 */
[[nodiscard]] constexpr name_buffer convert_case(std::string_view identifier, naming_style style) noexcept {
    name_buffer result;
    if (style == naming_style::as_written) {
        for (const char value : identifier)
            result.push(value);
        return result;
    }

    bool starting_word = true;
    bool any_word_emitted = false;

    for (std::size_t index = 0; index < identifier.size(); ++index) {
        const char value = identifier[index];

        if (value == '_' || value == '-') {
            starting_word = true;
            continue;
        }
        if (index > 0) {
            const char previous = identifier[index - 1];
            if ((is_upper(value) && is_lower(previous)) || (is_digit(value) && !is_digit(previous))) {
                starting_word = true;
            }
        }

        if (starting_word && any_word_emitted) {
            switch (style) {
            case naming_style::snake_case:
            case naming_style::screaming_snake_case: result.push('_'); break;
            case naming_style::kebab_case: result.push('-'); break;
            default: break;
            }
        }

        switch (style) {
        case naming_style::snake_case:
        case naming_style::kebab_case: result.push(lowered(value)); break;
        case naming_style::screaming_snake_case: result.push(uppered(value)); break;
        case naming_style::camel_case:
            result.push(starting_word && any_word_emitted ? uppered(value) : lowered(value));
            break;
        case naming_style::pascal_case: result.push(starting_word ? uppered(value) : lowered(value)); break;
        default: result.push(value); break;
        }

        if (starting_word) {
            starting_word = false;
            any_word_emitted = true;
        }
    }
    return result;
}

} // namespace detail

// ---------------- the reflected conversion ----------------

#if SERPENT_HAS_REFLECTION

namespace detail {

/** The annotation of type A attached to an entity, if there is one. */
template<typename A>
consteval std::optional<A> annotation_of(std::meta::info entity) {
    for (const auto note : std::meta::annotations_of(entity)) {
        if (std::meta::type_of(note) == std::meta::dealias(^^A)) return std::meta::extract<A>(note);
    }
    return std::nullopt;
}

template<typename A>
consteval bool has_annotation(std::meta::info entity) {
    return annotation_of<A>(entity).has_value();
}

/** The wire key for one field: an explicit key, else the type's naming rule. */
template<typename T, std::meta::info Member>
consteval std::string_view field_key() {
    if constexpr (constexpr auto explicit_name = annotation_of<key>(Member); explicit_name.has_value()) {
        return std::define_static_string(explicit_name->name);
    } else {
        constexpr auto style = annotation_of<naming>(^^T).value_or(naming {}).style;
        if constexpr (style == naming_style::as_written) {
            return std::define_static_string(std::meta::identifier_of(Member));
        } else {
            // Promoted to static storage: identifier_of only lives during constant evaluation.
            constexpr auto converted = convert_case(std::meta::identifier_of(Member), style);
            return std::define_static_string(converted.view());
        }
    }
}

template<typename T>
consteval bool opted_in() {
    return has_annotation<serializable>(^^T) || enable_reflection<T>::value;
}

} // namespace detail

/**
 * A type whose fields this library may enumerate.
 *
 * Deliberately opt-in. Reflecting every aggregate that merely lacks a json_convert would
 * turn any struct that happens to be serializable into a wire-format commitment, silently.
 */
template<typename T>
concept reflected_type = std::is_class_v<T> && detail::opted_in<T>();

/**
 * Generates the same member() calls SERPENT_DEFINE_TYPE would, from the type itself.
 *
 * Found by ordinary unqualified lookup from convertible_type in serializer.hpp, which is why
 * this header is included before it.
 */
template<typename T>
    requires reflected_type<T>
void json_convert(auto &visitor, conversion_object_t<decltype(visitor), T> value) {
    constexpr auto members =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));

    template for (constexpr auto member : members) {
        if constexpr (!detail::has_annotation<skip>(member)) {
            visitor.member(detail::field_key<T, member>(), value.[:member:]);
        }
    }
}

#else

/** Without reflection nothing is reflected, and the macro forms remain the way in. */
template<typename T>
concept reflected_type = false;

#endif

} // namespace serpent
