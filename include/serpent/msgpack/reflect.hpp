#pragma once

// A described type knows how many members it writes before it writes any, so its map is counted
// from the start and goes straight to the sink rather than being gathered.

#include <serpent/config.hpp>
#include <serpent/msgpack/writer.hpp>
#include <serpent/reflect.hpp>
#include <serpent/visitor.hpp>

namespace serpent::msgpack {

#if SERPENT_HAS_REFLECTION

/**
 * Writes a described type as a counted map.
 *
 * Found by ordinary lookup from serializer<T>::write, as BJData's is. The count is every member
 * not skipped, less those left out while empty, and the name of a discriminated type; were a
 * member ever to write other than one key, the writer would fail on closing rather than send a
 * map that says one thing and holds another.
 */
template<reflected_type T>
void write_reflected(writer &out, const T &value) {
    constexpr std::size_t members
            = serpent::detail::wire_keys<T>().size() + (serpent::detail::is_discriminated<T>() ? 1 : 0);
    write_visitor<writer> visitor { out };
    const auto scope = out.object(members - serpent::detail::omitted_count(value));
    serpent::detail::reflect_members(visitor, value);
}

#endif

} // namespace serpent::msgpack
