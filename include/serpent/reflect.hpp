#pragma once

// A type opts in and the compiler enumerates its fields, instead of a macro listing them.
//
// Needs P2996 reflection, P1306 expansion statements and P3394 annotations together. GCC 16
// has all three, behind -freflection. Everything that does not need reflection, meaning the
// annotations and the identifier-to-key conversion, sits outside the gate and compiles
// everywhere.

#include <serpent/concepts.hpp>
#include <serpent/visitor.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <cstddef>

// <meta> is includable whether or not reflection is enabled, but only defines
// __cpp_lib_reflection when it is, which makes it the gate rather than __cpp_reflection.
#if __has_include(<meta>) && defined(__cpp_expansion_statements)
#include <meta>
#include <tuple>
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

/**
 * On an optional field: the key must be present, though its value may be null.
 *
 * An optional is absent-tolerant by default, because that is what the type is for. This is for
 * the case where a document must state a value even if that value is nothing.
 */
struct required {};

/**
 * On a field: let the document leave it out, keeping whatever the member already held.
 *
 * A member that is absent is otherwise an error. This says the type would rather have its
 * default than a failure - for a field added to a format that older writers do not send, or
 * one a partial update is expected to omit.
 */
struct defaulted {};

/** On a type: opt in to reflected serialization. */
struct serializable {};

/**
 * On a type: name it on the wire, under a key that is not one of its members.
 *
 * The key is written before the members and read back as a selector, so alternatives of a
 * variant are told apart by what the document says they are rather than by trying each in turn
 * and seeing which sticks. Without a name the type's own identifier is used.
 *
 *     struct [[= serpent::discriminant("kind")]] circle { int radius; };   // {"kind":"circle",...}
 */
struct discriminant {
    char storage[64] {};
    std::size_t key_length = 0;
    char named[64] {};
    std::size_t name_length = 0;

    consteval discriminant(std::string_view key) { this->copy_key(key); }
    consteval discriminant(std::string_view key, std::string_view name) {
        this->copy_key(key);
        for (std::size_t index = 0; index < name.size() && index < sizeof(this->named) - 1; ++index)
            this->named[index] = name[index];
        this->name_length = name.size();
    }

    [[nodiscard]] constexpr std::string_view key() const { return { this->storage, this->key_length }; }
    [[nodiscard]] constexpr std::string_view name() const { return { this->named, this->name_length }; }

private:
    consteval void copy_key(std::string_view key) {
        for (std::size_t index = 0; index < key.size() && index < sizeof(this->storage) - 1; ++index)
            this->storage[index] = key[index];
        this->key_length = key.size();
    }
};

/**
 * On an enumerator: what it is on the wire, where its identifier will not do.
 *
 * Takes null, a boolean, a whole number, a real or a string, and they may be mixed within one
 * enumeration - a word length is 5, 6, 7, 8 and a stop-bit count is 1, 1.5, 2. An enumerator
 * with no annotation of its own goes by its identifier, under the type's naming rule.
 */
struct as {
    enum class kind : unsigned char { null, boolean, integer, real, text };

    // A variant in all but name. An annotation's value must be a structural type, which rules
    // out std::variant - its storage is private - but not a union whose members are public.
    kind held = kind::text;
    union {
        bool truth;
        std::int64_t whole;
        double number;
        char letters[64];
    };
    std::size_t length = 0;

    consteval as(std::nullptr_t) : held(kind::null), truth(false) {}
    consteval as(bool value) : held(kind::boolean), truth(value) {}
    consteval as(int value) : held(kind::integer), whole(value) {}
    consteval as(long long value) : held(kind::integer), whole(value) {}
    consteval as(unsigned long long value) : held(kind::integer), whole(static_cast<std::int64_t>(value)) {}
    consteval as(double value) : held(kind::real), number(value) {}
    consteval as(const char *text) : held(kind::text), letters {} { this->copy(text); }
    consteval as(std::string_view text) : held(kind::text), letters {} { this->copy(text); }

    [[nodiscard]] constexpr std::string_view text() const { return { this->letters, this->length }; }

private:
    consteval void copy(std::string_view text) {
        for (std::size_t index = 0; index < text.size() && index + 1 < sizeof(this->letters); ++index)
            this->letters[index] = text[index];
        this->length = text.size();
    }
};

/**
 * On an enumerator: the one to fall back to, in both directions.
 *
 * A value on the wire that matches no enumerator reads as this one, and an enumeration value
 * that is not any enumerator - cast in from a number, say - writes as it.
 */
struct fallback {};

/**
 * One enumerator of an enumeration you did not declare: its value, what it is on the wire, and
 * whether it is the one to fall back to.
 *
 * The third argument is spelled the same as the annotation - serpent::fallback {} - because it
 * means the same thing, in both directions.
 */
template<typename E>
struct enum_entry {
    E value {};
    as wire { nullptr };
    bool is_fallback = false;
    bool is_excluded = false;

    consteval enum_entry(E value, as wire) : value(value), wire(wire) {}
    consteval enum_entry(E value, as wire, fallback) : value(value), wire(wire), is_fallback(true) {}

    /** An enumerator that is deliberately not on the wire - a sentinel, a count, a _MAX. */
    consteval enum_entry(E value, skip) : value(value), is_excluded(true) {}
};

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
 * @brief Everything a type says about itself, when its declaration is not yours to annotate.
 *
 * The annotations put that in one place, on the declaration. This is the same place for a type
 * from someone else's header: specializing it is the opt-in, and what it carries is what the
 * annotations carry, under the same names.
 *
 * With nothing but a naming rule, every member is on the wire, as `serializable` alone would
 * have it:
 *
 *     template<>
 *     struct serpent::describe<foreign> {
 *         static constexpr serpent::naming naming { serpent::naming_style::snake_case };
 *     };
 *
 * A `members` table is for when a member needs to say more than its declaration does. It stands
 * in for the per-member annotations, and takes the same ones:
 *
 *     template<>
 *     struct serpent::describe<esp_netif_ip_info_t> {
 *         static constexpr serpent::member_entry members[] {
 *             ^^esp_netif_ip_info_t::ip,
 *             { ^^esp_netif_ip_info_t::netmask, serpent::key("mask") },
 *             { ^^esp_netif_ip_info_t::gw, serpent::defaulted {} },
 *         };
 *     };
 *
 * A table must name every member, so one added upstream cannot quietly stop being written; a
 * type only some of whose members belong on the wire says `static constexpr bool partial = true`.
 * Only non-static data members can be listed - a value that lives behind a pointer, or is
 * computed rather than stored, wants a serializer<T>, because there is no declaration to point
 * at. Listing members needs reflection, since addressing one without naming it is a splice.
 *
 * A `values` table describes an enumeration, a value per enumerator of any of the kinds `as`
 * takes, and one fallback:
 *
 *     template<>
 *     struct serpent::describe<uart_stop_bits_t> {
 *         static constexpr serpent::enum_entry<uart_stop_bits_t> values[] {
 *             { UART_STOP_BITS_1,   1,   serpent::fallback {} },
 *             { UART_STOP_BITS_1_5, 1.5 },
 *             { UART_STOP_BITS_2,   2   },
 *         };
 *     };
 *
 * That one needs no reflection, so it is also the way to map an enumeration on a toolchain
 * that has none.
 */
template<typename T>
struct describe;

/** Whether a type has been described from outside, whatever the description says. */
template<typename T>
concept described = requires { sizeof(describe<T>); };

// ---------------- what an enumerator is on the wire ----------------

namespace detail {

/** Whether two enumerators would be indistinguishable on the wire. */
consteval bool same_wire_form(const as &left, const as &right) {
    if (left.held != right.held) return false;
    switch (left.held) {
    case as::kind::null: return true;
    case as::kind::boolean: return left.truth == right.truth;
    case as::kind::integer: return left.whole == right.whole;
    case as::kind::real: return left.number == right.number;
    case as::kind::text: return left.text() == right.text();
    }
    return false;
}

/** An enumeration that carries a table, whoever declared it. */
template<typename E>
concept tabulated_enum = std::is_enum_v<E> && requires { describe<E>::values; };

/**
 * Writes one enumerator's wire form, whatever kind of value it is.
 *
 * One body for both ways an enumeration can be mapped. The annotated path passes a constant, so
 * the switch folds there and only the branch that value calls for is emitted.
 */
template<typename Emitter>
void emit_wire_form(Emitter &out, const as &form) {
    switch (form.held) {
    case as::kind::null: out.null(); return;
    case as::kind::boolean: out.boolean(form.truth); return;
    case as::kind::integer: out.integer(form.whole); return;
    case as::kind::real: out.real(form.number); return;
    case as::kind::text: out.string(form.text()); return;
    }
}

/** Whether a source holds exactly this wire form. */
template<typename Source>
bool source_is(Source source, const as &form) {
    switch (form.held) {
    case as::kind::null: return source.is_null();
    case as::kind::boolean: return source.as_bool() == form.truth;
    case as::kind::integer: {
        const auto held = source.template as_int<std::int64_t>();
        return held && *held == form.whole;
    }
    case as::kind::real: {
        const auto held = source.template as_float<double>();
        return held && *held == form.number;
    }
    case as::kind::text: {
        const auto held = source.as_string();
        return held && std::string_view { *held } == form.text();
    }
    }
    return false;
}

template<serpent::as Form, typename Emitter>
void emit_wire_form(Emitter &out) {
    emit_wire_form(out, Form);
}

template<serpent::as Form, typename Source>
bool source_is(Source source) {
    return source_is(source, Form);
}

/** Whether two entries of a table would read back as each other. */
template<typename E>
    requires tabulated_enum<E>
consteval bool table_forms_are_distinct() {
    const auto &values = describe<E>::values;
    for (std::size_t first = 0; first < std::size(values); ++first) {
        if (values[first].is_excluded) continue;
        for (std::size_t second = first + 1; second < std::size(values); ++second) {
            if (values[second].is_excluded) continue;
            if (same_wire_form(values[first].wire, values[second].wire)) return false;
        }
    }
    return true;
}

/** Whether a table names at most one enumerator to fall back to. */
template<typename E>
    requires tabulated_enum<E>
consteval bool table_fallback_is_unique() {
    std::size_t count = 0;
    for (const auto &entry : describe<E>::values)
        if (entry.is_fallback) ++count;
    return count <= 1;
}

/** Writes from a table. The fallback entry stands in for a value that is no enumerator. */
template<typename Emitter, typename E>
    requires tabulated_enum<E>
bool emit_table_enum(Emitter &out, E value) {
    for (const auto &entry : describe<E>::values) {
        if (!entry.is_excluded && entry.value == value) {
            emit_wire_form(out, entry.wire);
            return true;
        }
    }
    for (const auto &entry : describe<E>::values) {
        if (entry.is_fallback && !entry.is_excluded) {
            emit_wire_form(out, entry.wire);
            return true;
        }
    }
    return false;
}

/** Reads from a table, taking the fallback entry when nothing matches. */
template<typename Source, typename E>
    requires tabulated_enum<E>
bool read_table_enum(Source source, E &value) {
    if (!source.is_valid()) return false;

    for (const auto &entry : describe<E>::values) {
        if (!entry.is_excluded && source_is(source, entry.wire)) {
            value = entry.value;
            return true;
        }
    }
    for (const auto &entry : describe<E>::values) {
        if (entry.is_fallback && !entry.is_excluded) {
            value = entry.value;
            return true;
        }
    }
    return false;
}

} // namespace detail

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

/**
 * One member of a type you did not declare: which member, and anything an annotation would have
 * said about it.
 *
 * The member is named the way the compiler names it - `^^some_type::field` - so its key comes
 * from the declaration rather than from a string you restate, and one entry serves both
 * directions because the same splice reads and writes.
 *
 * The options are the annotations, spelled the same and meaning the same, in any order:
 * serpent::key to rename it, serpent::required or serpent::defaulted to say whether the
 * document has to carry it.
 */
struct member_entry {
    std::meta::info which {};
    char renamed[64] {};
    std::size_t rename_length = 0;
    bool insisted = false;
    bool optional_in_document = false;
    bool excluded = false;

    consteval member_entry(std::meta::info which) : which(which) {}

    template<typename... Options>
    consteval member_entry(std::meta::info which, Options... options) : which(which) {
        (this->apply(options), ...);
    }

    [[nodiscard]] consteval bool is_renamed() const { return this->rename_length != 0; }
    [[nodiscard]] consteval std::string_view rename() const { return { this->renamed, this->rename_length }; }

private:
    consteval void apply(key name) {
        const auto text = name.view();
        for (std::size_t index = 0; index < text.size() && index + 1 < sizeof(this->renamed); ++index)
            this->renamed[index] = text[index];
        this->rename_length = text.size();
    }
    consteval void apply(required) { this->insisted = true; }
    consteval void apply(defaulted) { this->optional_in_document = true; }
    consteval void apply(skip) { this->excluded = true; }
};

/**
 * Every non-static data member of a type, for a table that wants all of them.
 *
 * A convenience over writing the identifiers out, and nothing more than that: it fills the same
 * sequence by hand-rolling it from the compiler's own list, so there is one walk and no second
 * code path. Narrowing it is ordinary code over an ordinary range - views::filter on
 * identifier_of, say - rather than a vocabulary this library would have to invent:
 *
 *     static constexpr auto members = serpent::all_members_of<T>();
 *
 *     static constexpr auto members = std::define_static_array(
 *             std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())
 *             | std::views::filter([](std::meta::info member) {
 *                   return std::meta::identifier_of(member) != "reserved";
 *               }));
 */
template<typename T>
consteval auto all_members_of() {
    return std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
}


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
    if constexpr (requires { describe<T>::naming; })
        return describe<T>::naming.style;
    else
        return annotation_of<naming>(^^T).value_or(naming {}).style;
}

/**
 * A field's tag, with any name it did not spell out filled in from the alternative's identifier.
 *
 * Resolved here, where reflection is available, so that the writer and reader receive a tag that
 * already knows every name and need not reflect on anything themselves.
 */
template<typename Variant, tagged Base>
consteval tagged resolved_tag() {
    tagged result = Base;
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((result.name(Index).empty() && detail::object_like<std::variant_alternative_t<Index, Variant>>
                         ? result.name_alternative(Index,
                                   std::meta::identifier_of(
                                           std::meta::dealias(^^std::variant_alternative_t<Index, Variant>)))
                         : void()),
                ...);
    }(std::make_index_sequence<std::variant_size_v<Variant>> {});
    return result;
}

/**
 * Whether a document has to carry this member.
 *
 * A plain member must be there, because nothing else can supply it and silently keeping a
 * default is how a half-specified document passes for a whole one. An optional need not be,
 * since absence is precisely what it can represent. Either may say otherwise.
 */
template<typename T, std::meta::info Member>
consteval bool member_is_required() {
    using declared = [:std::meta::type_of(Member):];
    if constexpr (optional_like<declared>) {
        // An optional is absent-tolerant because that is what it represents, so only the member
        // itself can demand its key - a type-wide rule has nothing to say about it.
        return has_annotation<required>(Member);
    } else if constexpr (has_annotation<required>(Member)) {
        return true;
    } else {
        // On the type it means every member, the way naming does. A whole type of fields that may
        // be absent is the ordinary shape of a stored configuration, where a field added in a
        // later version is simply not in a file written before it.
        return !has_annotation<defaulted>(Member) && !has_annotation<defaulted>(^^T);
    }
}

/** The wire key for one field: an explicit key, else the type's naming rule. */
template<typename T, std::meta::info Member>
consteval std::string_view field_key() {
    if constexpr (constexpr auto explicit_name = annotation_of<key>(Member); explicit_name.has_value()) {
        return std::define_static_string(explicit_name->view());
    } else {
        // The rule of the type that declared it, which for a member of a base is the base's own -
        // its keys were settled where it was written, and a derived type must not restyle them.
        constexpr auto style = naming_for<typename[:std::meta::parent_of(Member):]>();
        if constexpr (style == naming_style::as_written) {
            return std::define_static_string(std::meta::identifier_of(Member));
        } else {
            // Promoted to static storage: identifier_of only lives during constant evaluation.
            constexpr auto converted = convert_case(std::meta::identifier_of(Member), style);
            return std::define_static_string(converted.view());
        }
    }
}

/** Whether a description lists the members itself, rather than leaving the compiler to. */
template<typename T>
concept lists_members = requires { describe<T>::members; };

template<typename T>
consteval bool opted_in() {
    // A listed type is read from its table instead, so the two routes never both answer for one
    // type - which is what lets a description say either without saying which it is saying.
    if constexpr (lists_members<T>) return false;
    return has_annotation<serializable>(^^T) || described<T> || has_annotation<discriminant>(^^T);
}

template<typename T>
consteval bool is_discriminated() {
    return has_annotation<discriminant>(^^T);
}

/** The key a discriminated type is named under. */
template<typename T>
consteval std::string_view discriminant_key() {
    return std::define_static_string(annotation_of<discriminant>(^^T)->key());
}

/** What it is called there: the annotation's name, else the type's own identifier. */
template<typename T>
consteval std::string_view discriminant_name() {
    constexpr auto note = annotation_of<discriminant>(^^T);
    if constexpr (note->name_length > 0)
        return std::define_static_string(note->name());
    else
        return std::define_static_string(std::meta::identifier_of(^^T));
}

/** The discriminant key of the first alternative that has one. */
template<typename Variant>
consteval std::string_view first_discriminant_key() {
    std::string_view found {};
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((found.empty() && is_discriminated<std::variant_alternative_t<Index, Variant>>()
                         ? found = discriminant_key<std::variant_alternative_t<Index, Variant>>()
                         : found),
                ...);
    }(std::make_index_sequence<std::variant_size_v<Variant>> {});
    return found;
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

/** Whether a type names itself on the wire under a key of its own. */
template<typename T>
concept discriminated_type = std::is_class_v<T> && detail::is_discriminated<T>();

namespace detail {

// ---------------- what the compiler can check about a type's wire form ----------------
//
// All of this is reflection-only and all of it is a check rather than a capability: the list of
// members, and what each is called on the wire, is knowable here and nowhere else. A mistake in
// any of it would otherwise be found by reading a document that came out wrong.

/**
 * Every member a type has, its bases' included, bases first.
 *
 * nonstatic_data_members_of reports only what a type declares itself, so a type with a base
 * would otherwise be written without its inherited state - a document silently missing the
 * fields it was built on. Bases come first because that is where their members were declared,
 * and because it is the order anyone listing them by hand would write.
 *
 * Only public bases, which falls out of asking from here: a base this library cannot see is one
 * the type did not expose.
 */
template<typename T>
consteval std::vector<std::meta::info> members_including_bases() {
    std::vector<std::meta::info> all;
    template for (constexpr auto base :
            std::define_static_array(std::meta::bases_of(^^T, std::meta::access_context::current()))) {
        using inherited = [:std::meta::type_of(base):];
        for (const auto member : members_including_bases<inherited>()) all.push_back(member);
    }
    for (const auto member : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))
        all.push_back(member);
    return all;
}

/** Every member's key, in declaration order, with the skipped ones left out. */
template<typename T>
consteval std::vector<std::string_view> wire_keys() {
    std::vector<std::string_view> keys;
    template for (constexpr auto member : std::define_static_array(members_including_bases<T>())) {
        if constexpr (!has_annotation<skip>(member)) keys.push_back(field_key<T, member>());
    }
    return keys;
}

/** Whether two members claim the same key, which a rename or a naming rule can do. */
template<typename T>
consteval bool keys_are_distinct() {
    const auto keys = wire_keys<T>();
    for (std::size_t first = 0; first < keys.size(); ++first)
        for (std::size_t second = first + 1; second < keys.size(); ++second)
            if (keys[first] == keys[second]) return false;
    return true;
}

template<typename T>
consteval std::string_view duplicate_key_message() {
    const auto keys = wire_keys<T>();
    std::string repeated;
    for (std::size_t first = 0; first < keys.size(); ++first)
        for (std::size_t second = first + 1; second < keys.size(); ++second)
            if (keys[first] == keys[second] && repeated.find(std::string { keys[first] }) == std::string::npos) {
                if (!repeated.empty()) repeated += ", ";
                repeated += keys[first];
            }
    return std::define_static_string("two members of this type are the same key on the wire: "
            + repeated
            + ". One would overwrite the other reading, and both would be written; rename one with "
              "serpent::key, or leave one out with serpent::skip");
}

/**
 * Whether every annotation on a member can mean something where it is.
 *
 * An annotation that cannot apply is always a mistake about what it does, and silently doing
 * nothing is the worst available answer.
 */
template<typename T>
consteval std::string_view annotation_complaint() {
    std::string complaint;
    template for (constexpr auto member : std::define_static_array(members_including_bases<T>())) {
        constexpr std::string_view name = std::define_static_string(std::meta::identifier_of(member));
        using declared = [:std::meta::type_of(member):];

        if constexpr (has_annotation<required>(member) && has_annotation<defaulted>(member)) {
            complaint += std::string { name } + " is required and defaulted at once; ";
        } else if constexpr (has_annotation<required>(member) && !optional_like<declared>
                && !has_annotation<defaulted>(^^T)) {
            complaint += std::string { name }
                    + " is not an optional and its type is not defaulted, so it is required already - "
                      "serpent::required says only that an optional's key must be stated, or that this "
                      "member is the exception to a defaulted type; ";
        }
        if constexpr (has_annotation<skip>(member) && has_annotation<key>(member)) {
            complaint += std::string { name } + " is skipped and also renamed; ";
        }
        if constexpr (!std::meta::annotations_of_with_type(member, ^^as).empty()) {
            complaint += std::string { name } + " carries serpent::as, which belongs on an enumerator; ";
        }
        if constexpr (!std::meta::annotations_of_with_type(member, ^^naming).empty()) {
            complaint += std::string { name } + " carries serpent::naming, which belongs on the type; ";
        }
    }
    if (complaint.empty()) return {};
    complaint.resize(complaint.size() - 2); // the trailing separator
    return std::define_static_string("an annotation on this type cannot mean anything where it is: " + complaint);
}

template<typename T>
consteval bool annotations_make_sense() {
    return annotation_complaint<T>().empty();
}

// ---------------- a member list for a type that is not yours ----------------

/** A type whose members are listed out of line rather than annotated. */
template<typename T>
concept tabulated_type = lists_members<T>;

/**
 * Whether a member table still accounts for every member of its type.
 *
 * The counterpart of table_names_every_enumerator, for the same reason: a member added upstream,
 * which is what an SDK upgrade does, would otherwise quietly stop being written. Naming a member
 * with serpent::skip counts as accounting for it - that is the deliberate way to keep one off
 * the wire - and a type that only ever wants a subset says `partial`.
 */
template<typename T>
consteval bool table_names_every_member() {
    if constexpr (requires { describe<T>::partial; }) {
        if constexpr (describe<T>::partial) return true;
    }
    for (const auto member :
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())) {
        bool found = false;
        for (const auto &listed : describe<T>::members) found = found || member_entry(listed).which == member;
        if (!found) return false;
    }
    return true;
}

/** The diagnostic for that, naming what the table missed. */
template<typename T>
consteval std::string_view incomplete_member_table_message() {
    std::string missing;
    for (const auto member :
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())) {
        bool found = false;
        for (const auto &listed : describe<T>::members) found = found || member_entry(listed).which == member;
        if (!found) {
            if (!missing.empty()) missing += ", ";
            missing += std::meta::identifier_of(member);
        }
    }
    return std::define_static_string("this serpent::describe table does not name every member of "
            "its type. Not named: "
            + missing
            + ". List them, or say { member, serpent::skip {} } to keep one off the wire "
              "deliberately, or say `static constexpr bool partial = true` if the table is meant "
              "to be a subset");
}

/** What one listed member is called on the wire: its rename, else its declared identifier. */
template<typename T, member_entry Entry>
consteval std::string_view table_key() {
    if constexpr (Entry.is_renamed()) {
        return std::define_static_string(Entry.rename());
    } else {
        constexpr auto style = naming_for<T>();
        if constexpr (style == naming_style::as_written) {
            return std::define_static_string(std::meta::identifier_of(Entry.which));
        } else {
            constexpr auto converted = convert_case(std::meta::identifier_of(Entry.which), style);
            return std::define_static_string(converted.view());
        }
    }
}

/** Whether the document has to carry a listed member, by the same rule an annotated one follows. */
template<typename T, member_entry Entry>
consteval bool table_member_is_required() {
    using declared = [:std::meta::type_of(Entry.which):];
    constexpr bool table_is_defaulted = requires { describe<T>::defaulted; };
    if constexpr (optional_like<declared>) return Entry.insisted;
    else if constexpr (Entry.insisted) return true;
    else return !Entry.optional_in_document && !table_is_defaulted;
}

/** Whether two listed members claim the same key. */
template<typename T>
consteval bool table_keys_are_distinct() {
    std::vector<std::string_view> keys;
    template for (constexpr auto listed : std::define_static_array(describe<T>::members)) {
        constexpr member_entry entry = listed;
        if constexpr (!entry.excluded) keys.push_back(table_key<T, entry>());
    }
    for (std::size_t first = 0; first < keys.size(); ++first)
        for (std::size_t second = first + 1; second < keys.size(); ++second)
            if (keys[first] == keys[second]) return false;
    return true;
}

/**
 * The member() calls for a type whose members are listed rather than annotated.
 *
 * The same shape as the annotated walk, and deliberately so: everything downstream - the strict
 * reading, the constant key framing, the generated readers - sees no difference between a type
 * that named its own members and one that was named from outside.
 */
template<typename Visitor, typename Object, typename T = std::remove_cvref_t<Object>>
    requires tabulated_type<T>
void table_members(Visitor &visitor, Object &value) {
    static_assert(table_keys_are_distinct<T>(),
            "two members of this serpent::describe table are the same key on the wire; rename one "
            "with serpent::key");
    static_assert(table_names_every_member<T>(), incomplete_member_table_message<T>());

    template for (constexpr auto listed : std::define_static_array(describe<T>::members)) {
        // A sequence of plain std::meta::info is a table of members with nothing said about
        // them, which is the common case; member_entry is how one of them says more.
        constexpr member_entry entry = listed;
        if constexpr (!entry.excluded) {
        // Bound to a reference first: a splice may not appear in an arbitrary expression.
        auto &field = value.[:entry.which:];
        static constexpr std::string_view name = table_key<T, entry>();

        if constexpr (table_member_is_required<T, entry>()) {
            if (!visitor.template member_if_present<name>(field)) visitor.missing(name);
        } else {
            std::ignore = visitor.template member_if_present<name>(field);
        }
        }
    }
}

/**
 * Generates the member() calls a hand-written json_convert would, from the type itself.
 *
 * Deliberately not spelled json_convert. As an overload it would tie with a hand-written one
 * on a type that has both, and an ambiguous call makes the convertible_type probe silently
 * false - reporting no conversion at all rather than two. serializer<T> calls this by name
 * instead, after it has looked for the hand-written forms.
 */
/**
 * The member walk itself, with no opt-in requirement.
 *
 * A field tagged with serpent::tagged names alternatives that may never have opted in - that is
 * the point of putting the tag on the field - so naming them there is the opt-in for them.
 */
template<typename Visitor, typename Object, typename T = std::remove_cvref_t<Object>>
void reflect_members(Visitor &visitor, Object &value) {
    static_assert(keys_are_distinct<T>(), duplicate_key_message<T>());
    static_assert(annotations_make_sense<T>(), annotation_complaint<T>());

    if constexpr (detail::is_discriminated<T>()) {
        // Written as though it were a member, read as nothing: the selector is not a field, and
        // by the time a value is being read something has already used it to choose this type.
        if constexpr (!std::remove_cvref_t<Visitor>::is_reading) {
            visitor.member(detail::discriminant_key<T>(), detail::discriminant_name<T>());
        }
    }

    template for (constexpr auto member : std::define_static_array(detail::members_including_bases<T>())) {
        if constexpr (!detail::has_annotation<skip>(member)) {
            // Bound to a reference first: a splice may not appear in an arbitrary expression.
            auto &field = value.[:member:];

            if constexpr (constexpr auto tag = detail::annotation_of<tagged>(member); tag.has_value()) {
                // Taken from the member rather than from the local, which may not be named in a
                // template argument here.
                using declared = [:std::meta::type_of(member):];
                static_assert(detail::variant_like<declared>, "serpent::tagged belongs on a variant field");

                constexpr tagged resolved = detail::resolved_tag<declared, *tag>();
                auto wrapper = make_tagged<resolved>(field);
                visitor.member(detail::field_key<T, member>(), wrapper);
            } else {
                static constexpr std::string_view name = detail::field_key<T, member>();

                // Reading, a member the type insists on has to have been there. Keeping its
                // default instead is how a half-specified document passes for a whole one.
                // Said outright rather than left to member(), which exempts every optional,
                // including one this type has marked required.
                if constexpr (detail::member_is_required<T, member>()) {
                    if (!visitor.template member_if_present<name>(field)) visitor.missing(name);
                } else {
                    std::ignore = visitor.template member_if_present<name>(field);
                }
            }
        }
    }
}

/** Whether an enumeration says how it appears on the wire, rather than going out as a number. */
template<typename T>
consteval bool enum_is_mapped() {
    if constexpr (!std::is_enum_v<T>) {
        return false;
    } else {
        if (has_annotation<serializable>(^^T)) return true;
        bool marked = false;
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((marked = marked
                             || !std::meta::annotations_of_with_type(
                                     std::define_static_array(std::meta::enumerators_of(^^T))[Index], ^^as)
                                     .empty()
                             || !std::meta::annotations_of_with_type(
                                     std::define_static_array(std::meta::enumerators_of(^^T))[Index], ^^fallback)
                                     .empty()),
                    ...);
        }(std::make_index_sequence<std::define_static_array(std::meta::enumerators_of(^^T)).size()> {});
        return marked;
    }
}

/**
 * Whether a table names every enumerator of the type it is for.
 *
 * Reflection can enumerate an enumeration whoever declared it - only *annotating* one needs
 * ownership - so a table written for a foreign type can still be held to its type. Without this
 * an enumerator added upstream, which is what an SDK upgrade does, would quietly start reading
 * and writing as the fallback.
 */
template<typename E>
consteval bool table_names_every_enumerator() {
    bool complete = true;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        bool found = false;
        for (const auto &entry : describe<E>::values)
            found = found || entry.value == std::meta::extract<E>(enumerator);
        complete = complete && found;
    }
    return complete;
}

/**
 * The diagnostic for a table that has fallen behind its type, naming what it missed.
 *
 * Built here rather than written as a literal so the message says which enumerator - the point
 * of the check is an enumerator you did not know had appeared, so being told its name is most of
 * the value.
 */
template<typename E>
consteval std::string_view incomplete_table_message() {
    std::string missing;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        bool found = false;
        for (const auto &entry : describe<E>::values)
            found = found || entry.value == std::meta::extract<E>(enumerator);
        if (!found) {
            if (!missing.empty()) missing += ", ";
            missing += std::meta::identifier_of(enumerator);
        }
    }
    return std::define_static_string("this serpent::describe table does not name every enumerator of "
            "its type. Not named: "
            + missing
            + ". Give each a value, or say { enumerator, serpent::skip {} } to keep it off the wire "
              "deliberately");
}

/** What one enumerator is on the wire: its own annotation, else its identifier. */
template<typename T, std::meta::info Enumerator>
consteval as wire_form() {
    if constexpr (constexpr auto given = annotation_of<as>(Enumerator); given.has_value()) {
        return *given;
    } else {
        constexpr auto style = naming_for<T>();
        if constexpr (style == naming_style::as_written) {
            return as { std::define_static_string(std::meta::identifier_of(Enumerator)) };
        } else {
            constexpr auto converted = convert_case(std::meta::identifier_of(Enumerator), style);
            return as { std::define_static_string(converted.view()) };
        }
    }
}

/** Whether two enumerators of an annotated enumeration collide on the wire. */
template<typename E>
consteval std::string_view annotated_enum_complaint() {
    std::vector<std::string_view> forms;
    std::vector<std::string_view> names;
    std::string complaint;
    std::size_t fallbacks = 0;

    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        constexpr as form = wire_form<E, enumerator>();
        constexpr std::string_view name = std::define_static_string(std::meta::identifier_of(enumerator));
        if constexpr (has_annotation<fallback>(enumerator)) ++fallbacks;

        template for (constexpr auto other : std::define_static_array(std::meta::enumerators_of(^^E))) {
            constexpr as other_form = wire_form<E, other>();
            constexpr std::string_view other_name = std::define_static_string(std::meta::identifier_of(other));
            if constexpr (!has_annotation<skip>(enumerator) && !has_annotation<skip>(other)
                    && std::meta::extract<E>(enumerator) != std::meta::extract<E>(other)
                    && same_wire_form(form, other_form) && name < other_name) {
                complaint += std::string { name } + " and " + std::string { other_name }
                        + " are the same value on the wire; ";
            }
        }
    }
    if (fallbacks > 1) complaint += "more than one enumerator is the fallback; ";
    if (complaint.empty()) return {};
    complaint.resize(complaint.size() - 2); // the trailing separator
    return std::define_static_string("this enumeration cannot be read back as it is written: " + complaint);
}

template<typename E>
consteval bool annotated_enum_is_sound() {
    return annotated_enum_complaint<E>().empty();
}


} // namespace detail

namespace detail {

/** An enumeration whose own enumerators carry annotations. */
template<typename T>
concept annotated_enum = std::is_enum_v<T> && detail::enum_is_mapped<T>();

} // namespace detail

namespace detail {

} // namespace detail

namespace detail {

/** Writes an annotated enumeration. False when the value is no enumerator and none is the fallback. */
template<typename Emitter, typename E>
bool emit_annotated_enum(Emitter &out, E value) {
    if constexpr (!annotated_enum<E>) {
        return false;
    } else {
        bool written = false;
        template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
            // A skipped enumerator is not on the wire, so it is not a candidate either. C APIs
            // alias sentinels onto real values - a _MIN that is also the first real one - and
            // taking it as a match would emit the sentinel's name for the real value.
            if constexpr (!detail::has_annotation<skip>(enumerator)) {
                if (!written && value == std::meta::extract<E>(enumerator)) {
                    written = true;
                    detail::emit_wire_form<detail::wire_form<E, enumerator>()>(out);
                }
            }
        }
        if (!written) {
            template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
                if constexpr (detail::has_annotation<fallback>(enumerator) && !detail::has_annotation<skip>(enumerator)) {
                    if (!written) {
                        written = true;
                        detail::emit_wire_form<detail::wire_form<E, enumerator>()>(out);
                    }
                }
            }
        }
        return written;
    }
}

/** Reads an annotated enumeration, taking the fallback enumerator when nothing matches. */
template<typename Source, typename E>
    requires annotated_enum<E>
bool read_annotated_enum(Source source, E &value) {
    if (!source.is_valid()) return false;

    bool matched = false;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if constexpr (!detail::has_annotation<skip>(enumerator)) {
            if (!matched && detail::source_is<detail::wire_form<E, enumerator>()>(source)) {
                matched = true;
                value = std::meta::extract<E>(enumerator);
            }
        }
    }
    if (!matched) {
        template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
            if constexpr (detail::has_annotation<fallback>(enumerator) && !detail::has_annotation<skip>(enumerator)) {
                if (!matched) {
                    matched = true;
                    value = std::meta::extract<E>(enumerator);
                }
            }
        }
    }
    return matched;
}

/** The opted-in entry point, which is what serializer<T> looks for. */
template<typename Visitor, typename Object, typename T = std::remove_cvref_t<Object>>
    requires reflected_type<T>
void reflect_convert(Visitor &visitor, Object &value) {
    reflect_members(visitor, value);
}

/** The same, for a type named from outside rather than annotated. */
template<typename Visitor, typename Object, typename T = std::remove_cvref_t<Object>>
    requires tabulated_type<T>
void table_convert(Visitor &visitor, Object &value) {
    table_members(visitor, value);
}

} // namespace detail

#else

/** Without reflection nothing is reflected, and the manual forms remain the way in. */
template<typename T>
concept reflected_type = false;

template<typename T>
concept discriminated_type = false;

namespace detail {

/** A member table needs splicing, so without reflection there is no such thing. */
template<typename T>
concept tabulated_type = false;

/** Never defined: the concept above is false, so every call to these is discarded. */
template<typename Visitor, typename Object>
void table_convert(Visitor &visitor, Object &value);

template<typename Visitor, typename Object>
void table_members(Visitor &visitor, Object &value);

/** Without reflection an enumeration cannot annotate itself; a table is still open to it. */
template<typename T>
concept annotated_enum = false;

/** Nothing can be checked against: without reflection the enumerator list is not knowable. */
template<typename E>
consteval bool table_names_every_enumerator() {
    return true;
}

/** Never read: the check above always passes here, so the assertion it belongs to never fires. */
template<typename E>
consteval std::string_view incomplete_table_message() {
    return {};
}

/** An annotated enumeration cannot exist here, so there is nothing to find fault with. */
template<typename E>
consteval bool annotated_enum_is_sound() {
    return true;
}

template<typename E>
consteval std::string_view annotated_enum_complaint() {
    return {};
}

/** Never defined: annotated_enum is false, so every call to these is discarded. */
template<typename Emitter, typename E>
bool emit_annotated_enum(Emitter &out, E value);

template<typename Source, typename E>
bool read_annotated_enum(Source source, E &value);

// Never defined: the concepts above are false, so every call to these is discarded. They exist
// so that the discarded branches still name something.
template<typename Visitor, typename Object>
void reflect_convert(Visitor &visitor, Object &value);

template<typename T>
consteval std::string_view discriminant_key();

template<typename T>
consteval std::string_view discriminant_name();

template<typename Visitor, typename Object>
void reflect_members(Visitor &visitor, Object &value);

template<typename Variant>
consteval std::string_view first_discriminant_key();

} // namespace detail

#endif

/**
 * An enumeration that says what it is on the wire rather than going out as a number.
 *
 * Either by annotating its enumerators, or by a serpent::describe table where the
 * declaration is not yours to annotate.
 */
template<typename T>
concept mapped_enum = detail::annotated_enum<T> || detail::tabulated_enum<T>;

/** Writes a mapped enumeration. False when the value is no enumerator and none is the fallback. */
template<typename Emitter, typename E>
bool emit_mapped_enum(Emitter &out, E value) {
    static_assert(!(detail::tabulated_enum<E> && detail::annotated_enum<E>),
            "this enumeration is annotated and also has a serpent::describe table. The table "
            "would be used and the annotations would do nothing; remove whichever of the two you "
            "did not mean");
    if constexpr (detail::tabulated_enum<E>) {
#if SERPENT_HAS_REFLECTION
        static_assert(detail::table_names_every_enumerator<E>(), detail::incomplete_table_message<E>());
#endif
        static_assert(detail::table_forms_are_distinct<E>(),
                "two entries of this serpent::describe table are the same value on the wire, so "
                "one could never be read back");
        static_assert(detail::table_fallback_is_unique<E>(),
                "more than one entry of this serpent::describe table is the fallback");
    }
#if SERPENT_HAS_REFLECTION
    else {
        static_assert(detail::annotated_enum_is_sound<E>(), detail::annotated_enum_complaint<E>());
    }
#endif
    if constexpr (detail::tabulated_enum<E>) {
        return detail::emit_table_enum(out, value);
    } else if constexpr (detail::annotated_enum<E>) {
        return detail::emit_annotated_enum(out, value);
    } else {
        return false;
    }
}

/** Reads a mapped enumeration, taking the fallback when nothing matches. */
template<typename Source, typename E>
    requires mapped_enum<E>
bool read_mapped_enum(Source source, E &value) {
    static_assert(!(detail::tabulated_enum<E> && detail::annotated_enum<E>),
            "this enumeration is annotated and also has a serpent::describe table. The table "
            "would be used and the annotations would do nothing; remove whichever of the two you "
            "did not mean");
    if constexpr (detail::tabulated_enum<E>) {
#if SERPENT_HAS_REFLECTION
        static_assert(detail::table_names_every_enumerator<E>(), detail::incomplete_table_message<E>());
#endif
        static_assert(detail::table_forms_are_distinct<E>(),
                "two entries of this serpent::describe table are the same value on the wire, so "
                "one could never be read back");
        static_assert(detail::table_fallback_is_unique<E>(),
                "more than one entry of this serpent::describe table is the fallback");
    }
#if SERPENT_HAS_REFLECTION
    else {
        static_assert(detail::annotated_enum_is_sound<E>(), detail::annotated_enum_complaint<E>());
    }
#endif
    if constexpr (detail::tabulated_enum<E>) {
        return detail::read_table_enum(source, value);
    } else {
        return detail::read_annotated_enum(source, value);
    }
}

} // namespace serpent
