#pragma once

// Every optimisation that has a plainer way of doing the same thing, and the switch that chooses.
//
// Each of these trades something - code that is harder to follow, a table in the image, a source
// file to build - for speed. The plain way stays in the library beside it, produces the same
// bytes, and is what runs when the switch is 0, so any of them can be turned off to see what it
// was worth, or for good if it turns out not to be worth it.
//
// Set them on the compiler's command line, or through the CMake options of the same names.

/**
 * Writes reals with the bundled Żmij rather than std::to_chars.
 *
 * Off unless the build says otherwise, because it is the one switch that needs a source file
 * compiled: the CMake target `serpent` turns it on and builds that file.
 *
 * That file has a switch of its own, SERPENT_ZMIJ_OPTIMIZE_SIZE, which no header here reads:
 * defined to 1 when it is compiled, Żmij computes powers of ten as it goes rather than keeping a
 * table of them, and is somewhat slower: 36 KB of code rather than 67 KB, built for arm64. Left undefined, Żmij chooses that for itself
 * when the compiler is optimising for size. The CMake option of the same name sets it.
 */
/**
 * Whether the compiler has C++26 reflection with expansion statements, which the described-type
 * paths need. <meta> is includable whether or not reflection is enabled, but only defines
 * __cpp_lib_reflection when it is, which makes it the gate rather than __cpp_reflection. Settled
 * here so that every header agrees, whatever order they are included in.
 */
#if __has_include(<meta>) && defined(__cpp_expansion_statements)
#include <meta>
#endif
#if defined(__cpp_lib_reflection) && defined(__cpp_expansion_statements)
#define SERPENT_HAS_REFLECTION 1
#else
#define SERPENT_HAS_REFLECTION 0
#endif

#ifndef SERPENT_USE_ZMIJ
#define SERPENT_USE_ZMIJ 0
#endif

/**
 * Writes consecutive boolean and number members of a described type as one piece of text, into
 * room found once for all of them, rather than finding room for each key and each value.
 */
#ifndef SERPENT_BOUNDED_OBJECT_WRITE
#define SERPENT_BOUNDED_OBJECT_WRITE 1
#endif

/**
 * How an integer is turned into text.
 *
 *   0  std::to_chars
 *   1  two digits at a time from a table of 400 bytes; about twice as fast
 *   2  four digits at a time from a table of 40 KB; about twice as fast again, for an image that
 *      has the room
 *
 * 1 and 2 are Glaze's, kept under external/glaze with its licence.
 */
#ifndef SERPENT_INTEGER_TABLE
#define SERPENT_INTEGER_TABLE 1
#endif

/**
 * Reads reals with the bundled fast_float rather than std::from_chars.
 *
 * std::from_chars accepts more than JSON does, so text has to be walked once to check it and
 * again to convert it; fast_float has a mode that accepts exactly JSON, and does both in one
 * walk. It brings 28 KB of code and tables, built for arm64 - much of which a standard library
 * whose own from_chars is built on fast_float, as libstdc++'s is, has a copy of already.
 */
#ifndef SERPENT_USE_FAST_FLOAT
#define SERPENT_USE_FAST_FLOAT 1
#endif

/**
 * Looks for what a string cannot hold as itself - a quote, a backslash, a control character -
 * eight bytes at a time rather than one, when writing a string and when reading one. Where the
 * target has NEON, a string that has outrun a word goes on sixteen bytes at a time, by a vector
 * compare; the word comes first because most strings end inside it and a word's answer never
 * leaves the integer registers.
 */
#ifndef SERPENT_WIDE_STRING_SCAN
#define SERPENT_WIDE_STRING_SCAN 1
#endif

/**
 * Insists that the few small functions on the path of every value are inlined.
 *
 * A compiler inlines by a budget for the whole translation unit, and in a large one the budget
 * runs out: functions of three lines that vanish in a small program become calls, their constant
 * widths become run-time arguments, and the same source runs two to four times slower for
 * reasons that have nothing to do with it. Those functions are marked SERPENT_ALWAYS_INLINE,
 * which with this off is nothing, and the compiler decides as it would have.
 */
#ifndef SERPENT_FORCE_INLINE
#define SERPENT_FORCE_INLINE 1
#endif

#if SERPENT_FORCE_INLINE && (defined(__GNUC__) || defined(__clang__))
#define SERPENT_ALWAYS_INLINE [[gnu::always_inline]]
#else
#define SERPENT_ALWAYS_INLINE
#endif
