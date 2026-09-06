#pragma once

// Forward declarations for the format-neutral core, so headers here can name the
// customization point without pulling in its definition.

namespace nonstd::serial {

template<typename T, typename = void>
struct serializer;

}// namespace nonstd::serial
