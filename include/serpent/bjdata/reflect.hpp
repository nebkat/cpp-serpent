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
template<typename T>
    requires reflected_type<T>
bool read_reflected(const view &source, T &value) {
    if (!source.is_object()) return false;

    const auto info = source.container_header();
    if (info.body == nullptr) return false;

    detail::cursor scanner { source.buffer(), info.body };
    std::uint64_t remaining = info.count;
    const bool counted = !info.unbounded;
    bool complete = true;

    while (scanner.ok()) {
        while (scanner.position < scanner.limit && to_marker(*scanner.position) == marker::noop)
            ++scanner.position;

        if (counted) {
            if (remaining == 0) break;
        } else if (scanner.position >= scanner.limit || to_marker(*scanner.position) == marker::object_end) {
            break;
        }
        if (scanner.position >= scanner.limit) break;

        const auto key = detail::read_key(scanner);
        if (!scanner.ok()) return false;

        marker kind = info.element;
        if (kind == marker::invalid) {
            if (!scanner.need(1)) return false;
            kind = to_marker(scanner.peek());
            if (!is_value(kind)) return false;
            scanner.advance(1);
        }

        const view held { kind, source.buffer(), scanner.position };

        // One compile-time comparison per field, each against a constant of known length. The
        // first that matches consumes the entry; anything else is a key this type does not name.
        bool matched = false;
        template for (constexpr auto member : std::define_static_array(
                              std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                if (!matched && detail::key_matches<name>(key)) {
                    matched = true;
                    auto &field = value.[:member:];

                    // The same field handling reflect_convert does; a tagged variant is read
                    // through the wrapper that carries its names, not as a bare variant.
                    if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(member);
                            tag.has_value()) {
                        using declared = [:std::meta::type_of(member):];
                        constexpr serpent::tagged resolved = serpent::detail::resolved_tag<declared, *tag>();
                        auto wrapper = make_tagged<resolved>(field);
                        if (!read_into(held, wrapper)) complete = false;
                    } else {
                        if (!read_into(held, field)) complete = false;
                    }
                }
            }
        }
        (void)matched;

        detail::skip_value(scanner, kind, 1);
        if (!scanner.ok()) return false;
        if (counted && remaining > 0) --remaining;
    }

    return complete;
}

#endif

} // namespace serpent::bjdata
