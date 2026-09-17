# Configuration

Every optimisation that has a plainer way of doing the same thing is behind a switch, with the
plain way kept beside it and producing the same bytes. They are macros, defined in
`serpent/config.hpp`, and CMake options of the same names.

| Switch | Default | On, it | Off, it | Costs |
|---|---|---|---|---|
| `SERPENT_USE_ZMIJ` | on in CMake, off in the headers | writes reals with the bundled Żmij | uses `std::to_chars` | one source file to build; 67 KB of code on arm64 |
| `SERPENT_ZMIJ_OPTIMIZE_SIZE` | `AUTO` | builds Żmij without its table of powers of ten | keeps the table | 36 KB rather than 67 KB, and a real takes about a quarter longer to write. `AUTO` lets Żmij choose, which it does by whether the compiler is optimising for size |
| `SERPENT_BOUNDED_OBJECT_WRITE` | on | writes runs of number and boolean members into room claimed once | asks for room per key and per value | nothing but the code |
| `SERPENT_INTEGER_TABLE` | `1` | `1`: integers two digits at a time from a 400-byte table. `2`: four at a time from a 40 KB one | `0`: `std::to_chars` | the table |
| `SERPENT_USE_FAST_FLOAT` | on | reads reals in one walk with the bundled fast_float | checks the grammar, then `std::from_chars` | 28 KB of code and tables on arm64 - largely a second copy, where the standard library's own `from_chars` is built on fast_float, as libstdc++'s is - and a large header to compile |
| `SERPENT_FORCE_INLINE` | on | marks the few small functions on the path of every value always-inline | leaves inlining to the compiler, whose budget for it is per translation unit and runs out in a large one - the same source then runs two to four times slower | somewhat more code where those functions are used |
| `SERPENT_WIDE_STRING_SCAN` | on | finds what a string must escape eight bytes at a time | looks at every byte | nothing but the code |

The CMake target `serpent::plain` is the library with every one of them off, whatever the build
chose: headers only, and the standard library throughout.

What each is worth is in [Benchmarks](benchmarks.md#what-the-switches-are-worth), and
`tools/bench.sh --ablations` measures the default with each turned off on its own.

A switch changes how fast a document is produced or read, never what it is, and that is tested:
each way of writing an integer or a real, reading a real, or scanning a string is compared with
the plain way over millions of values.

Żmij, Glaze's integer formatting and fast_float are other people's work, kept under
`include/serpent/external/` with their licences and copied from pinned upstream versions by
`tools/vendor-*.sh`; the README there says who wrote what.
