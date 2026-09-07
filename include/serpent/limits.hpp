#pragma once

namespace serpent {

/** Maximum nesting any reader or writer will follow: bounds recursion, and fits the mask. */
inline constexpr int max_depth = 32;

} // namespace serpent
