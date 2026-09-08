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
        template for (constexpr auto member : std::define_static_array(
                              std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                static constexpr auto encoded = detail::encoded_key<name>;
                if (!matched && static_cast<std::size_t>(scanner.limit - scanner.position) >= encoded.size()
                        && std::memcmp(scanner.position, encoded.data(), encoded.size()) == 0) {
                    matched = true;
                    scanner.advance(encoded.size());
                    kind = value_marker();
                    if (kind == marker::invalid || !take.template operator()<member>(kind)) return false;
                }
            }
        }

        // Not written the way we write it: parse the key properly and match it by name, so a
        // document from another encoder still reads.
        if (!matched) {
            const auto key = detail::read_key(scanner);
            if (!scanner.ok()) return false;
            kind = value_marker();
            if (kind == marker::invalid) return false;

            template for (constexpr auto member : std::define_static_array(
                                  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
                if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                    static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                    if (!matched && detail::key_matches<name>(key)) {
                        matched = true;
                        if (!take.template operator()<member>(kind)) return false;
                    }
                }
            }
        }

        if (!consumed) {
            detail::skip_value(scanner, kind, 1);
            if (!scanner.ok()) return false;
        }
        if (counted && remaining > 0) --remaining;
    }

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

#endif

} // namespace serpent::bjdata
