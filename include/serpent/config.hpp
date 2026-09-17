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
 * walk. It brings about 10 KB of tables - which a standard library whose own from_chars is
 * built on fast_float, as libstdc++'s is, has a copy of already.
 */
#ifndef SERPENT_USE_FAST_FLOAT
#define SERPENT_USE_FAST_FLOAT 1
#endif
