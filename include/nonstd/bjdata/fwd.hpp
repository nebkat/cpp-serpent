#pragma once

// Shared forward declarations, so view.hpp and writer.hpp can name each other's types and
// the serializer without either having to include the other. Mirrors nonstd/json_fwd.hpp.

namespace nonstd::bjdata {

class view;
class writer;

template<typename T, typename = void>
struct serializer;

}// namespace nonstd::bjdata
