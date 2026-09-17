// Vendored from https://github.com/stephenberry/glaze at 142d9ab6fa9272d779142055b9be482229f94bcb
// (include/glaze/util/inline.hpp) by tools/vendor-glaze.sh. Do not edit: change the script and run
// it again. What differs from upstream, and why, is described there. The licence is beside
// this file.
//
// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// SERPENT_GLZ_DISABLE_ALWAYS_INLINE can be defined to disable forced inlining,
// reducing binary size and compilation time at the cost of peak performance.
#if !defined(SERPENT_GLZ_DISABLE_ALWAYS_INLINE)
#if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
#ifndef SERPENT_GLZ_USE_ALWAYS_INLINE
#define SERPENT_GLZ_USE_ALWAYS_INLINE
#endif
#endif
#endif

// Enable always_inline when optimizing (-O1 or higher) or in release builds (NDEBUG)
// __OPTIMIZE__ is defined by GCC/Clang when any optimization level is enabled
#if defined(SERPENT_GLZ_USE_ALWAYS_INLINE) && (defined(NDEBUG) || defined(__OPTIMIZE__))
#ifndef SERPENT_GLZ_ALWAYS_INLINE
#if defined(_MSC_VER) && !defined(__clang__)
#define SERPENT_GLZ_ALWAYS_INLINE [[msvc::forceinline]] inline
#else
#define SERPENT_GLZ_ALWAYS_INLINE inline __attribute__((always_inline))
#endif
#endif
#endif

#ifndef SERPENT_GLZ_ALWAYS_INLINE
#define SERPENT_GLZ_ALWAYS_INLINE inline
#endif

// IMPORTANT: SERPENT_GLZ_FLATTEN should only be used with extreme care
// It often adds to the binary size and greatly increases compilation times.
// It should only be applied in very specific circumstances.
// It is best to more often rely on the compiler.

#if !defined(SERPENT_GLZ_DISABLE_ALWAYS_INLINE)
#if (defined(__clang__) || defined(__GNUC__)) && defined(NDEBUG)
#ifndef SERPENT_GLZ_FLATTEN
#define SERPENT_GLZ_FLATTEN inline __attribute__((flatten))
#endif
#endif
#endif

#ifndef SERPENT_GLZ_FLATTEN
#define SERPENT_GLZ_FLATTEN inline
#endif

#ifndef SERPENT_GLZ_NO_INLINE
#if defined(__clang__) || defined(__GNUC__)
#define SERPENT_GLZ_NO_INLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define SERPENT_GLZ_NO_INLINE __declspec((noinline))
#endif
#endif
