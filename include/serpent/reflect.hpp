#pragma once

// A type opts in and the compiler enumerates its fields, instead of a macro listing them.
//
// Needs P2996 reflection, P1306 expansion statements and P3394 annotations together. GCC 16
// has all three, behind -freflection. Everything that does not need reflection, meaning the
// annotations and the identifier-to-key conversion, sits outside the gate and compiles
// everywhere.

#include <serpent/visitor.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <type_traits>

#include <cstddef>

// <meta> is includable whether or not reflection is enabled, but only defines
// __cpp_lib_reflection when it is, which makes it the gate rather than __cpp_reflection.
#if __has_include(<meta>) && defined(__cpp_expansion_statements)
#include <meta>
#endif

#if defined(__cpp_lib_reflection) && defined(__cpp_expansion_statements)
#define SERPENT_HAS_REFLECTION 1
#else
#define SERPENT_HAS_REFLECTION 0
#endif

namespace serpent {

/** Whether this build can enumerate a type's fields for itself. */
inline constexpr bool reflection_available = SERPENT_HAS_REFLECTION != 0;

// Annotations are ordinary values, not parsed strings. Write them qualified -
// [[=serpent::key("dt")]] - which is what lets them be this short.

/**
 * On a field: use this key instead of the identifier.
 *
 * The name is stored as an array rather than a view because an annotation's type has to be
 * structural, and neither a pointer nor a string_view is.
 */
struct key {
    char storage[64] {};
    std::size_t length = 0;

    consteval key(std::string_view name) {
        for (std::size_t index = 0; index < name.size() && index < sizeof(this->storage) - 1; ++index)
            this->storage[index] = name[index];
        this->length = name.size();
    }

    [[nodiscard]] constexpr std::string_view view() const { return { this->storage, this->length }; }
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

/**
 * The opt-in for a type you cannot annotate. Specialize to true_type.
 *
 * A specialization may also carry a naming style. Reflection reads the fields directly, so
 * there is no conversion function to hang a setting on, and the type cannot be annotated -
 * this trait is the only type-level surface the consumer owns:
 *
 *     template<>
 *     struct serpent::enable_reflection<foreign> : std::true_type {
 *         static constexpr naming_style style = naming_style::snake_case;
 *     };
 */
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
    const auto found = std::meta::annotations_of_with_type(entity, ^^A);
    if (found.empty()) return std::nullopt;
    return std::meta::extract<A>(found[0]);
}

template<typename A>
consteval bool has_annotation(std::meta::info entity) {
    return !std::meta::annotations_of_with_type(entity, ^^A).empty();
}

/**
 * The naming rule for a type: an external opt-in that names one, else the type's own
 * annotation.
 *
 * The trait wins because it is the consumer's deliberate override of a type they do not own,
 * and a type that carries both is being adapted by someone other than its author.
 */
template<typename T>
consteval naming_style naming_for() {
    if constexpr (requires { enable_reflection<T>::style; })
        return enable_reflection<T>::style;
    else
        return annotation_of<naming>(^^T).value_or(naming {}).style;
}

/** The wire key for one field: an explicit key, else the type's naming rule. */
template<typename T, std::meta::info Member>
consteval std::string_view field_key() {
    if constexpr (constexpr auto explicit_name = annotation_of<key>(Member); explicit_name.has_value()) {
        return std::define_static_string(explicit_name->view());
    } else {
        constexpr auto style = naming_for<T>();
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

namespace detail {

/**
 * Generates the same member() calls SERPENT_DEFINE_TYPE would, from the type itself.
 *
 * Deliberately not spelled json_convert. As an overload it would tie with a hand-written one
 * on a type that has both, and an ambiguous call makes the convertible_type probe silently
 * false - reporting no conversion at all rather than two. serializer<T> calls this by name
 * instead, after it has looked for the hand-written forms.
 */
template<typename Visitor, typename Object, typename T = std::remove_cvref_t<Object>>
    requires reflected_type<T>
void reflect_convert(Visitor &visitor, Object &value) {
    template for (constexpr auto member :
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
        if constexpr (!detail::has_annotation<skip>(member)) {
            // Bound to a reference first: a splice may not appear in an arbitrary expression.
            auto &field = value.[:member:];
            visitor.member(detail::field_key<T, member>(), field);
        }
    }
}

} // namespace detail

#else

/** Without reflection nothing is reflected, and the manual forms remain the way in. */
template<typename T>
concept reflected_type = false;

namespace detail {

/** Never defined: reflected_type is false, so every call to it is discarded. */
template<typename Visitor, typename Object>
void reflect_convert(Visitor &visitor, Object &value);

} // namespace detail

#endif

} // namespace serpent
