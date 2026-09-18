#pragma once

#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace serpent {

template<typename T, typename = void>
struct serializer;

/**
 * Writes an enumeration that says what it is on the wire. Defined in reflect.hpp.
 *
 * Declared here so that emit_value can name it: false means the enumeration says nothing, and
 * it goes out as its underlying number.
 */
template<typename Emitter, typename E>
bool emit_mapped_enum(Emitter &out, E value);

/** Reads one value into a destination. Defined in serializer.hpp. */
template<typename Source, typename T>
bool read_into(const Source &source, T &value);

/**
 * On a std::variant field: the key that says which alternative the document holds.
 *
 * Sits on the field rather than on the alternatives, so it works for types you cannot annotate
 * and lets one set of types be tagged differently in different places.
 *
 *     [[= serpent::tagged("kind")]] std::variant<point, circle> body;
 *     [[= serpent::tagged("kind", { "pt", "circ" })]] std::variant<point, circle> body;
 *
 * Without an explicit list the alternatives are named by their own identifiers.
 */
struct tagged {
    static constexpr std::size_t name_capacity = 48;
    static constexpr std::size_t max_alternatives = 12;

    char stored_key[64] {};
    std::size_t key_length = 0;
    char stored_names[max_alternatives][name_capacity] {};
    std::size_t name_lengths[max_alternatives] {};
    std::size_t name_count = 0;

    consteval tagged(std::string_view key) { this->assign(this->stored_key, this->key_length, key, 64); }

    consteval tagged(std::string_view key, std::initializer_list<std::string_view> names) : tagged(key) {
        for (const auto name : names) {
            if (this->name_count >= max_alternatives) break;
            this->assign(
                    this->stored_names[this->name_count], this->name_lengths[this->name_count], name, name_capacity);
            ++this->name_count;
        }
    }

    [[nodiscard]] constexpr std::string_view key() const { return { this->stored_key, this->key_length }; }

    [[nodiscard]] constexpr std::string_view name(std::size_t index) const {
        if (index >= this->name_count) return {};
        return { this->stored_names[index], this->name_lengths[index] };
    }

    /** Fills in a name the annotation did not give, from the alternative's own identifier. */
    consteval void name_alternative(std::size_t index, std::string_view name) {
        if (index >= max_alternatives) return;
        this->assign(this->stored_names[index], this->name_lengths[index], name, name_capacity);
        if (index >= this->name_count) this->name_count = index + 1;
    }

private:
    consteval void assign(char *target, std::size_t &length, std::string_view text, std::size_t capacity) {
        for (std::size_t index = 0; index < text.size() && index + 1 < capacity; ++index)
            target[index] = text[index];
        length = text.size() < capacity ? text.size() : capacity - 1;
    }
};

/**
 * A variant bound to the tag that names its alternatives, so the ordinary value paths can carry
 * it without every writer and reader growing an extra parameter.
 */
template<tagged Tag, typename Variant>
struct tagged_variant {
    Variant &target;
};

/** Deduces the variant's constness from the reference, which naming the type does not. */
template<tagged Tag, typename Variant>
constexpr tagged_variant<Tag, Variant> make_tagged(Variant &target) {
    return tagged_variant<Tag, Variant> { target };
}

} // namespace serpent
