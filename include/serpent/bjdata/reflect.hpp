#pragma once

// Reading a reflected type from BJData without going through the member iterator.
//
// The generic path is written in terms of handles: an iterator that parses an entry into a key
// and a view, a visitor that compares the key against a name it is handed at run time. That is
// the right shape for a reader that knows nothing about the type. For a type whose fields the
// compiler can enumerate, none of it is necessary - the keys are constants, their lengths are
// constants, and the destination of each is known - so this walks the bytes once and assigns
// straight into the fields.
//
// Binary only. JSON stays on the generic path, where the reader is worth reading.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/view.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/config.hpp>
#include <serpent/member_runs.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace serpent::bjdata {

#if SERPENT_HAS_REFLECTION

namespace detail {

/** Compares against a name whose length is known, which is what makes it worth inlining. */
template<std::string_view const &Name>
[[nodiscard]] inline bool key_matches(std::string_view key) noexcept {
    return key.size() == Name.size() && std::memcmp(key.data(), Name.data(), Name.size()) == 0;
}

} // namespace detail

/**
 * Fills a reflected type from an object, in one pass over its members.
 *
 * Found by argument-dependent lookup from serializer<T>::read, so a source that has no such
 * function - json::reader - simply does not take this path.
 */
namespace detail {

/**
 * Reads an object's members, leaving the cursor immediately after it.
 *
 * The cursor starts on the byte after the opening brace. Consuming the object rather than
 * merely reading it is what lets a sequence of these be walked once instead of twice.
 */
template<typename T>
    requires reflected_type<T>
bool read_object_body(detail::cursor &scanner, const std::span<const std::byte> buffer, T &value) {
    const auto info = detail::parse_object_prefix(scanner);
    if (!scanner.ok() || info.body == nullptr) return false;

    std::uint64_t remaining = info.count;
    const bool counted = !info.unbounded;
    bool complete = true;

    // One bit per member, set as it is read, so that what the type insists on can be checked
    // once at the end. An OR per member and a compare per object, nothing per byte.
    static constexpr std::size_t member_count =
            std::define_static_array(serpent::detail::members_including_bases<T>())
                    .size();
    static_assert(member_count <= 64, "a type with more than 64 members needs a wider seen mask");

    static constexpr std::uint64_t required_mask = [] {
        std::uint64_t mask = 0;
        std::size_t position = 0;
        template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<T>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)
                    && serpent::detail::member_is_required<T, member>()) {
                mask |= std::uint64_t { 1 } << position;
            }
            ++position;
        }
        return mask;
    }();
    std::uint64_t seen = 0;

    while (scanner.ok()) {
        while (scanner.position < scanner.limit && to_marker(*scanner.position) == marker::noop)
            ++scanner.position;

        if (counted) {
            if (remaining == 0) break;
            if (scanner.position >= scanner.limit) return false;
        } else if (scanner.position >= scanner.limit) {
            return false;
        } else if (to_marker(*scanner.position) == marker::object_end) {
            ++scanner.position; // consumed, so the caller resumes after the object
            break;
        }

        // Reading a value into a field, given the marker that precedes it. One definition,
        // reached either by recognising the encoded key or by parsing it.
        bool consumed = false;
        const auto take = [&]<std::meta::info Member>(marker kind) {
            const view held { kind, buffer, scanner.position };
            auto &field = value.[:Member:];

            // A string is the one field whose length prefix would otherwise be read twice: once
            // for the text, and again by skip_value to step over it.
            using field_type = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::same_as<field_type, std::string>) {
                if (kind == marker::string) {
                    const auto length = detail::read_length(scanner);
                    if (!scanner.ok() || !scanner.need(length)) return false;
                    field.assign(reinterpret_cast<const char *>(scanner.position), static_cast<std::size_t>(length));
                    scanner.advance(length);
                    consumed = true;
                    return true;
                }
            }

            // The same field handling reflect_convert does; a tagged variant is read through the
            // wrapper that carries its names, not as a bare variant.
            if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member); tag.has_value()) {
                using declared = [:std::meta::type_of(Member):];
                constexpr serpent::tagged resolved = serpent::detail::resolved_tag<declared, *tag>();
                auto wrapper = make_tagged<resolved>(field);
                if (!read_into(held, wrapper)) complete = false;
            } else {
                if (!read_into(held, field)) complete = false;
            }
            return true;
        };

        // Reads the marker that introduces a value, which a strongly typed object omits.
        const auto value_marker = [&]() -> marker {
            if (info.element != marker::invalid) return info.element;
            if (!scanner.need(1)) return marker::invalid;
            const auto kind = to_marker(scanner.peek());
            if (!is_value(kind)) return marker::invalid;
            scanner.advance(1);
            return kind;
        };

        // The key exactly as we would have written it, compared whole. A hit skips parsing the
        // length marker, the length and the bytes separately.
        bool matched = false;
        marker kind = marker::invalid;
        std::size_t position = 0;
        template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<T>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                static constexpr auto encoded = detail::encoded_key<name>;

                // A length under 128 is the same byte whether its marker calls it uint8 or int8,
                // so both spellings are recognised at once: the length and the name come from
                // one constant, and either marker is allowed in front. Any other legal spelling
                // of the same key - a length written wider than it needs to be, say - misses
                // here and is parsed below, which is what makes the guess safe to make.
                static constexpr bool short_length = encoded.size() == name.size() + 2 && name.size() < 128;
                const bool lead_matches = short_length
                        ? (*scanner.position == static_cast<std::byte>(marker::uint8)
                                  || *scanner.position == static_cast<std::byte>(marker::int8))
                        : *scanner.position == encoded[0];

                if (!matched && static_cast<std::size_t>(scanner.limit - scanner.position) >= encoded.size()
                        && lead_matches
                        && std::memcmp(scanner.position + 1, encoded.data() + 1, encoded.size() - 1) == 0) {
                    matched = true;
                    seen |= std::uint64_t { 1 } << position;
                    scanner.advance(encoded.size());
                    kind = value_marker();
                    if (kind == marker::invalid || !take.template operator()<member>(kind)) return false;
                }
            }
            ++position;
        }

        // Not written the way we write it: parse the key properly and match it by name, so a
        // document from another encoder still reads.
        if (!matched) {
            position = 0;
            const auto key = detail::read_key(scanner);
            if (!scanner.ok()) return false;
            kind = value_marker();
            if (kind == marker::invalid) return false;

            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<T>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                    static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                    if (!matched && detail::key_matches<name>(key)) {
                        matched = true;
                        seen |= std::uint64_t { 1 } << position;
                        if (!take.template operator()<member>(kind)) return false;
                    }
                    ++position;
                }
            }
        }

        if (!consumed) {
            detail::skip_value(scanner, kind, 1);
            if (!scanner.ok()) return false;
        }
        if (counted && remaining > 0) --remaining;
    }

    // A member the type insists on has to have been there; keeping its default instead is how a
    // half-specified document passes for a whole one.
    if ((seen & required_mask) != required_mask) return false;
    return complete;
}

} // namespace detail

/**
 * Fills a reflected type from an object.
 *
 * Found by argument-dependent lookup from serializer<T>::read, so a source that has no such
 * function - json::reader - simply does not take this path.
 */
template<typename T>
    requires reflected_type<T>
bool read_reflected(const view &source, T &value) {
    if (!source.is_object()) return false;
    detail::cursor scanner { source.buffer(), source.data() };
    return detail::read_object_body(scanner, source.buffer(), value);
}

/**
 * Fills a container of reflected objects, walking the document once.
 *
 * Without this the elements are read through the array iterator, which knows nothing of what
 * the reader above consumed and skips each element again to find the next - so every record is
 * parsed twice. Returns nothing for a shape it does not handle, leaving the generic path to it.
 */
template<typename C, typename T = std::remove_cvref_t<typename C::value_type>>
    requires reflected_type<T> && requires(C &out) {
        out.clear();
        out.emplace_back();
    }
std::optional<bool> read_sequence(const view &source, C &out) {
    if (!source.is_array()) return std::nullopt;

    const auto info = source.container_header();
    if (info.body == nullptr) return std::nullopt;
    // A typed array of anything but objects is not a sequence of these.
    if (info.typed() && info.element != marker::object_begin) return std::nullopt;

    detail::cursor scanner { source.buffer(), info.body };
    std::uint64_t remaining = info.count;
    const bool counted = !info.unbounded;

    out.clear();
    if (counted) out.reserve(static_cast<std::size_t>(info.count));

    while (scanner.ok()) {
        while (scanner.position < scanner.limit && to_marker(*scanner.position) == marker::noop)
            ++scanner.position;

        if (counted) {
            if (remaining == 0) break;
            if (scanner.position >= scanner.limit) return false;
        } else if (scanner.position >= scanner.limit) {
            return false;
        } else if (to_marker(*scanner.position) == marker::array_end) {
            ++scanner.position;
            break;
        }

        if (!info.typed()) {
            if (to_marker(*scanner.position) != marker::object_begin) return false;
            ++scanner.position;
        }

        auto &slot = out.emplace_back();
        if (!detail::read_object_body(scanner, source.buffer(), slot)) return false;
        if (counted && remaining > 0) --remaining;
    }
    return true;
}

#if SERPENT_BOUNDED_OBJECT_WRITE

/** The most bytes one member can take - key, marker and payload - or zero if it has no limit. */
template<typename T, std::meta::info Member>
struct widest_member {
    static constexpr std::size_t value = [] () -> std::size_t {
        if (serpent::detail::has_annotation<skip>(Member)) return 0;
        if (serpent::detail::annotation_of<tagged>(Member).has_value()) return 0;

        using field = std::remove_cvref_t<typename [:std::meta::type_of(Member):]>;
        if (!std::integral<field> && !std::floating_point<field>) return 0;

        static constexpr std::string_view name = serpent::detail::field_key<T, Member>();
        return detail::encoded_key<name>.size() + plain_writer::widest_marked;
    }();
};

/**
 * Writes one object of a described type.
 *
 * A boolean or a number is a marker and at most eight bytes, and its key is a constant, so a run
 * of such members has a longest possible length: room for the run is claimed once and keys and
 * values are stored into it one after another, where each key and each value would otherwise
 * ask for room of its own. Any other member - a string, a container, another described type -
 * is written the usual way, and the run ends before it and another may begin after. Which
 * members are in which run is settled when this is compiled - see member_runs.hpp.
 */
template<writer_options Options, typename T>
class object_writer {
    using runs = serpent::detail::member_runs<T, widest_member>;
    using writer = basic_writer<Options>;

    writer &out;
    const T &value;

public:
    object_writer(writer &out, const T &value) noexcept : out(out), value(value) {}

    void write_members() {
        template for (constexpr auto member : runs::members) {
            constexpr std::size_t position = runs::position_of(member);

            if constexpr (serpent::detail::has_annotation<skip>(member)) {
                // Not written at all.
            } else if constexpr (runs::widest[position] == 0) {
                this->write_member<member>();
            } else if constexpr (runs::begins_run(position)) {
                // Writes every member of the run, so the rest of it have nothing left to do here.
                if (!this->write_run<position, runs::run_end(position)>())
                    this->write_run_one_at_a_time<position, runs::run_end(position)>();
            }
        }
    }

private:
    /** One member, the usual way: its key, then whatever writes its value. */
    template<std::meta::info Member>
    void write_member() {
        static constexpr std::string_view name = serpent::detail::field_key<T, Member>();
        const auto &field = this->value.[:Member:];

        this->out.template key_literal<name>();
        if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member); tag.has_value()) {
            using declared = [:std::meta::type_of(Member):];
            static_assert(serpent::detail::variant_like<declared>, "serpent::tagged belongs on a variant field");
            this->out.value(make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field));
        } else {
            this->out.value(field);
        }
    }

    /** The members from `First` up to `Last` stored one after another, if room for them can be had. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] bool write_run() {
        return this->out.compose_members(runs::widest_run(First, Last), [this](char *const to) {
            char *at = to;
            template for (constexpr auto member : runs::members) {
                if constexpr (runs::within(member, First, Last)) {
                    static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                    static constexpr auto &key = detail::encoded_key<name>;
                    std::memcpy(at, key.data(), key.size());
                    at = writer::write_marked(at + key.size(), writer::marked_form(this->value.[:member:]));
                }
            }
            return static_cast<std::size_t>(at - to);
        });
    }

    /** The same members when that room could not be had: each the usual way. */
    template<std::size_t First, std::size_t Last>
    void write_run_one_at_a_time() {
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last)) this->write_member<member>();
        }
    }
};

/**
 * Writes a described type as a BJData object.
 *
 * Found by ordinary lookup from serializer<T>::write, as read_reflected is, so that a writer with
 * no such function does not take this path.
 */
template<writer_options Options, reflected_type T>
    requires (!serpent::detail::is_discriminated<T>())
void write_reflected(basic_writer<Options> &out, const T &value) {
    static_assert(serpent::detail::keys_are_distinct<T>(), serpent::detail::duplicate_key_message<T>());
    static_assert(serpent::detail::annotations_make_sense<T>(), serpent::detail::annotation_complaint<T>());
    const auto scope = out.object();
    object_writer<Options, T> { out, value }.write_members();
}

#endif // SERPENT_BOUNDED_OBJECT_WRITE

#endif

} // namespace serpent::bjdata
