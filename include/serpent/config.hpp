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
