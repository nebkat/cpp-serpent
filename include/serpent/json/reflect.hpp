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
    // Built once rather than per entry: an escaped key is rare, and constructing somewhere to
    // put one for every member of every object is not free even when it stays empty.
    std::string decoded;

    for (const auto entry : source.items()) {
        std::string_view name = entry.key.contents;
        if (entry.key.escaped) [[unlikely]] {
            decoded = entry.key_string();
            name = decoded;
        }

        // Each name is compared at the width the compiler knows it to be, which is what makes
        // this worth unrolling: a length test rejects almost every member without looking at a
        // byte, and the comparison that survives it is a fixed-size one the compiler emits
        // inline rather than a call to memcmp with a length it cannot see. Reading the member is
        // inline here too, for the same reason - through a table it would be a call that cannot
        // be.
        //
        // The document is still walked once and each key offered to the type once, so a document
        // whose keys arrive in another order costs no more than this one does.
        bool matched = false;
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view key = serpent::detail::field_key<type, member>();
                if (!matched && name.size() == key.size()
                        && __builtin_memcmp(name.data(), key.data(), key.size()) == 0) {
                    matched = true;
                    seen |= std::uint64_t { 1 } << position;
                    if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(member);
                            tag.has_value()) {
                        // A tagged variant is read through the tag that names its alternatives;
                        // reading it plainly would go back to trying each alternative in turn.
                        using declared = [:std::meta::type_of(member):];
                        auto wrapper = make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(
                                value.[:member:]);
                        if (!read_into(entry.value, wrapper)) complete = false;
                    } else {
                        if (!read_into(entry.value, value.[:member:])) complete = false;
                    }
                }
            }
            ++position;
        }
    }

    if ((seen & required_mask) != required_mask) return false;
    return complete;
}

#endif

} // namespace serpent::json
