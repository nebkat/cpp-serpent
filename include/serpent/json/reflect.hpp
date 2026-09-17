#pragma once

// Reading a reflected type from JSON by walking the document once.
//
// The ordinary visitor walks the *type* and looks each member up in the document. When the two
// agree about order that is a cursor hit and costs nothing, and when they do not, every lookup
// scans the object from the start again - so a document whose keys arrive in another order costs
// the square of its size. Another implementation's output, a map-based serializer, a schema that
// grew a field in the middle: none of those are exotic.
//
// So this walks the document instead, and hands each key to the member that claims it. The type
// is asked about every key once, which is a run of length comparisons and free at any width this
// library supports; what goes away is looking at the document more than once.
//
// serializer<T>::read finds this by ordinary lookup on the source, so the visitor stays the way
// in for a hand-written conversion, which must stay member-driven because the body is the user's.

#include <array>
#include <bit>
#include <string>
#include <serpent/json/reader.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

namespace serpent::json {

#if SERPENT_HAS_REFLECTION

/**
 * Fills a reflected type from a JSON object, reading the document in one pass.
 *
 * Returns false for anything that is not an object, for a member that will not convert, and for
 * a member the type insists on that the document did not carry. A key the type does not name is
 * ignored, as it is everywhere else.
 */
/**
 * A key's slot, from its length and its ends.
 *
 * Deliberately not a hash over the whole key: the length is known without looking at the bytes
 * and two ends are two loads, which is enough to separate any field set worth having and costs
 * the same whether the name is four characters or forty.
 */
[[nodiscard]] constexpr std::size_t key_slot(std::string_view name, std::size_t mask) noexcept {
    if (name.empty()) return 0;
    const auto first = static_cast<unsigned char>(name.front());
    const auto last = static_cast<unsigned char>(name.back());
    return (name.size() * 131u + first * 7u + last) & mask;
}

/** Reads one member of a type, named by reflection rather than by a runtime index. */
template<typename T, std::meta::info Member>
bool fill_member(const reader &from, T &object) {
    return read_into(from, object.[:Member:]);
}

/** The same for a tagged variant, which is read through the tag that names its alternatives. */
template<typename T, std::meta::info Member, tagged Resolved>
bool fill_tagged_member(const reader &from, T &object) {
    auto wrapper = make_tagged<Resolved>(object.[:Member:]);
    return read_into(from, wrapper);
}

/**
 * Fills a reflected type from a JSON object, reading the document in one pass.
 *
 * Each key goes straight to the member that claims it, so neither the number of members nor the
 * order the document happens to use changes the work. The member walk it replaces was one
 * comparison per member when the two agreed about order and a rescan of the whole object per
 * member when they did not.
 *
 * Returns false for anything that is not an object, for a member that will not convert, and for
 * a member the type insists on that the document did not carry. A key the type does not name is
 * ignored, as everywhere else.
 */
template<typename T>
    requires reflected_type<std::remove_cvref_t<T>>
bool read_reflected(const reader &source, T &value) {
    using type = std::remove_cvref_t<T>;
    if (!source.is_object()) return false;

    static constexpr std::size_t member_count =
            std::define_static_array(serpent::detail::members_including_bases<type>()).size();
    static_assert(member_count <= 64, "a type with more than 64 members needs a wider seen mask");

    static constexpr std::size_t slots = std::bit_ceil(member_count * 2 + 1);
    static constexpr std::size_t slot_mask = slots - 1;
    static constexpr std::uint8_t no_member = 0xFF;

    /** Which member owns each slot, by open addressing: a collision takes the next free one. */
    static constexpr auto slot_owner = [] {
        std::array<std::uint8_t, slots> owners {};
        for (auto &owner : owners) owner = no_member;
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                constexpr std::string_view name = serpent::detail::field_key<type, member>();
                std::size_t slot = key_slot(name, slot_mask);
                while (owners[slot] != no_member) slot = (slot + 1) & slot_mask;
                owners[slot] = static_cast<std::uint8_t>(position);
            }
            ++position;
        }
        return owners;
    }();

    /** Each member's key, so a slot can be confirmed rather than assumed. */
    static constexpr auto member_key = [] {
        std::array<std::string_view, member_count> keys {};
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                keys[position] = serpent::detail::field_key<type, member>();
            }
            ++position;
        }
        return keys;
    }();

    /** Reading one member, by index, so a key found in one step is acted on in one more. */
    using filler = bool (*)(const reader &, type &);
    static constexpr auto fillers = [] {
        std::array<filler, member_count> table {};
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                // A tagged variant is read through the tag that names its alternatives, as the
                // member walk does it; reading it plainly would go back to trying each
                // alternative in turn, which is what the tag exists to stop.
                if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(member);
                        tag.has_value()) {
                    using declared = [:std::meta::type_of(member):];
                    table[position] = &fill_tagged_member<type, member,
                            serpent::detail::resolved_tag<declared, *tag>()>;
                } else {
                    table[position] = &fill_member<type, member>;
                }
            }
            ++position;
        }
        return table;
    }();

    static constexpr std::uint64_t required_mask = [] {
        std::uint64_t mask = 0;
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)
                    && serpent::detail::member_is_required<type, member>()) {
                mask |= std::uint64_t { 1 } << position;
            }
            ++position;
        }
        return mask;
    }();

    std::uint64_t seen = 0;
    bool complete = true;
    // Where the next key is expected, which is where it is when the document was written from
    // this type: one comparison then, and the slot lookup only when that guess is wrong.
    std::size_t expected = 0;

    for (const auto entry : source.items()) {
        // An escaped key is rare and cannot be compared in place, so it is decoded only then.
        std::string decoded;
        std::string_view name = entry.key.contents;
        if (entry.key.escaped) [[unlikely]] {
            decoded = entry.key_string();
            name = decoded;
        }

        // A document written from this type arrives in this order, and so does one written by
        // anyone following the same schema. Left unannotated on purpose: a branch this
        // consistent is what a predictor is best at, and saying so measured no different.
        if (expected < member_count && member_key[expected] == name) {
            seen |= std::uint64_t { 1 } << expected;
            if (!fillers[expected](entry.value, value)) complete = false;
            ++expected;
            continue;
        }

        for (std::size_t slot = key_slot(name, slot_mask);; slot = (slot + 1) & slot_mask) {
            const auto owner = slot_owner[slot];
            if (owner == no_member) break; // a key this type does not name
            if (member_key[owner] == name) {
                seen |= std::uint64_t { 1 } << owner;
                if (!fillers[owner](entry.value, value)) complete = false;
                expected = owner + std::size_t { 1 };
                break;
            }
        }
    }

    if ((seen & required_mask) != required_mask) return false;
    return complete;
}

#endif

} // namespace serpent::json
