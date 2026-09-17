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
#include <charconv>
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
/**
 * A member's key exactly as it appears in a document: quoted, and followed by its colon.
 *
 * One constant, so recognising a key is one comparison of known width rather than a string
 * scan, a copy and a search for the colon after it. A document with space around the colon
 * misses here and is read by the general path below, which is what makes the guess safe.
 */
template<const std::string_view &Name>
inline constexpr auto quoted_key = [] {
    std::array<char, Name.size() + 3> framed {};
    framed[0] = '"';
    for (std::size_t index = 0; index < Name.size(); ++index) framed[index + 1] = Name[index];
    framed[Name.size() + 1] = '"';
    framed[Name.size() + 2] = ':';
    return framed;
}();

/**
 * Fills a reflected type from a JSON object, reading the document in one pass.
 *
 * Walks the document itself rather than through the member iterator. The iterator is written for
 * a reader that knows nothing about the type: it scans each key, builds a handle for the value
 * and hands both back, and then scans the value again to find the entry after it. None of that
 * is needed for a type whose members the compiler can enumerate - the keys are constants, and so
 * is the punctuation around them - so this expects those bytes instead of classifying them.
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

    const auto text = source.document();
    auto *const notes = source.notes();
    scanner::cursor scan { text, source.data() + 1 }; // past the brace

    std::uint64_t seen = 0;
    bool complete = true;
    std::string decoded;
    bool first = true;

    while (true) {
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return false;
        if (scan.peek() == '}') {
            scan.advance(1);
            break;
        }
        if (!first) {
            if (scan.peek() != ',') return false;
            scan.advance(1);
            scanner::skip_whitespace(scan);
            if (!scan.available(1)) return false;
        }
        first = false;

        // Reads whatever value stands at the cursor into one member, and steps over it. The
        // value says where it ended - a scalar as it converts, a container as it is walked - so
        // stepping over it is usually a move rather than a second scan.
        const auto take = [&]<std::meta::info Member>() {
            using field = std::remove_cvref_t<typename [:std::meta::type_of(Member):]>;

            // The value of a member whose type the compiler knows, read where it stands. The
            // general path builds a handle, asks it what it is holding and converts through it,
            // which for a scalar is more work than the conversion. These are the types a schema
            // is mostly made of; anything else still goes the long way round below.
            if constexpr (std::same_as<field, bool>) {
                if (scan.available(4) && __builtin_memcmp(scan.position, "true", 4) == 0) {
                    value.[:Member:] = true;
                    scan.advance(4);
                    return;
                }
                if (scan.available(5) && __builtin_memcmp(scan.position, "false", 5) == 0) {
                    value.[:Member:] = false;
                    scan.advance(5);
                    return;
                }
                complete = false;
                scanner::skip_value(scan, 1);
                return;
            } else if constexpr (std::same_as<field, std::string>) {
                if (scan.available(1) && scan.peek() == '"') {
                    const auto scanned = scanner::scan_string(scan);
                    if (!scan.ok()) return;
                    if (!scanned.escaped) {
                        value.[:Member:].assign(scanned.contents);
                    } else {
                        auto &into = value.[:Member:];
                        into.clear();
                        into.reserve(scanner::decoded_length(scanned));
                        scanner::decode_string(scanned, [&](char one) { into.push_back(one); });
                    }
                    return;
                }
                complete = false;
                scanner::skip_value(scan, 1);
                return;
            } else if constexpr ((std::integral<field> || std::floating_point<field>)
                    && !std::same_as<field, char>) {
                const auto digits = scanner::scan_number(scan);
                if (!scan.ok()) return;
                // Same rule the handle applies: a real is a number and would convert, so an
                // integer member refuses the point or exponent that makes it one.
                if constexpr (std::integral<field>) {
                    if (digits.find_first_of(".eE") != std::string_view::npos) {
                        complete = false;
                        return;
                    }
                }
                const auto *const last = digits.data() + digits.size();
                if constexpr (std::floating_point<field>) {
                    double parsed = 0;
                    if (std::from_chars(digits.data(), last, parsed).ec != std::errc {}) complete = false;
                    else value.[:Member:] = static_cast<field>(parsed);
                } else if constexpr (std::is_signed_v<field>) {
                    std::int64_t parsed = 0;
                    if (std::from_chars(digits.data(), last, parsed).ec != std::errc {}
                            || !std::in_range<field>(parsed))
                        complete = false;
                    else value.[:Member:] = static_cast<field>(parsed);
                } else {
                    std::uint64_t parsed = 0;
                    if (digits.starts_with('-') || std::from_chars(digits.data(), last, parsed).ec != std::errc {}
                            || !std::in_range<field>(parsed))
                        complete = false;
                    else value.[:Member:] = static_cast<field>(parsed);
                }
                return;
            }

            const char *const at = scan.position;
            const reader held { text, at, notes };
            if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member);
                    tag.has_value()) {
                using declared = [:std::meta::type_of(Member):];
                auto wrapper = make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(value.[:Member:]);
                if (!read_into(held, wrapper)) complete = false;
            } else {
                if (!read_into(held, value.[:Member:])) complete = false;
            }
            if (notes != nullptr && notes->describes(text.data(), at)) scan.position = notes->reached;
            else scanner::skip_value(scan, 1);
        };

        // The key as this type would have written it, punctuation and all.
        bool matched = false;
        std::size_t position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<type, member>();
                static constexpr auto &framed = quoted_key<name>;
                if (!matched && scan.available(framed.size())
                        && __builtin_memcmp(scan.position, framed.data(), framed.size()) == 0) {
                    matched = true;
                    seen |= std::uint64_t { 1 } << position;
                    scan.advance(framed.size());
                    scanner::skip_whitespace(scan);
                    if (!scan.available(1)) return false;
                    take.template operator()<member>();
                }
            }
            ++position;
        }
        if (!scan.ok()) return false;
        if (matched) continue;

        // Written some other way, or a key this type does not name: scan it properly and match
        // it by name, so a document from another encoder still reads.
        if (scan.peek() != '"') return false;
        const auto key = scanner::scan_string(scan);
        if (!scan.ok()) return false;
        std::string_view name = key.contents;
        if (key.escaped) [[unlikely]] {
            decoded.clear();
            scanner::decode_string(key, [&](char one) { decoded.push_back(one); });
            name = decoded;
        }
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() != ':') return false;
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) return false;

        position = 0;
        template for (constexpr auto member :
                std::define_static_array(serpent::detail::members_including_bases<type>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view expected = serpent::detail::field_key<type, member>();
                if (!matched && name.size() == expected.size()
                        && __builtin_memcmp(name.data(), expected.data(), expected.size()) == 0) {
                    matched = true;
                    seen |= std::uint64_t { 1 } << position;
                    take.template operator()<member>();
                }
            }
            ++position;
        }
        if (!matched) scanner::skip_value(scan, 1);
        if (!scan.ok()) return false;
    }

    if ((seen & required_mask) != required_mask) return false;
    return complete;
}

#endif

} // namespace serpent::json
