#pragma once

/**
 * How deep a document may nest, counting the root as depth 0.
 *
 * Every reader and writer stops there, which is what keeps a hostile document from recursing a
 * decode off the end of its stack. Each level costs a few hundred bytes of stack to read into a
 * tree - about 440 for BJData into serpent::value, built for arm64 - so a task with a small stack
 * may want it lower. It cannot be higher: a writer remembers which levels are objects in one
 * 32-bit word.
 */
#ifndef SERPENT_MAX_DEPTH
#define SERPENT_MAX_DEPTH 32
#endif

static_assert(SERPENT_MAX_DEPTH >= 1 && SERPENT_MAX_DEPTH <= 32, "SERPENT_MAX_DEPTH must be from 1 to 32");

namespace serpent {

/** Maximum nesting any reader or writer will follow: bounds recursion, and fits the mask. */
inline constexpr int max_depth = SERPENT_MAX_DEPTH;

} // namespace serpent
