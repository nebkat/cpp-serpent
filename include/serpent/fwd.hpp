#pragma once

namespace serpent {

template<typename T, typename = void>
struct serializer;

/** Reads one value into a destination. Defined in serializer.hpp. */
template<typename Source, typename T>
bool read_into(Source source, T &value);

} // namespace serpent
