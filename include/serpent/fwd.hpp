#pragma once

// Forward declarations for the format-neutral core, so headers here can name the
// customization point without pulling in its definition.

namespace serpent {

template<typename T, typename = void>
struct serializer;

}// namespace serpent
