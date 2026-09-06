#pragma once

namespace nonstd::bjdata {

/**
 * Maximum container nesting any reader or writer will follow.
 *
 * Bounds recursion so a hostile document cannot exhaust the stack, and is also exactly the
 * number of bits in the emitter's container mask.
 */
inline constexpr int max_depth = 32;

}// namespace nonstd::bjdata
