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
