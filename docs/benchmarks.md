# Benchmarks

Against four other C++ JSON libraries on the corpus the published comparisons use
(`canada.json`, `citm_catalog.json`, `twitter.json`).

!!! warning "serpent is not the fastest library here, and these numbers say so"

    A struct-mapping library beats it on your own types, and two indexed parsers beat it on
    whole documents, in some cases by a lot. What it wins is partial reads and allocation
    behaviour. The table is below either way — a benchmark page that only showed the wins
    would not be worth publishing.

The comparisons are a DOM library, a struct-mapping library, a lazy on-demand parser, and two
fast DOM parsers. Every measured computation is cross-checked against all of them before any
timing is reported.

## Your own types

Ten thousand structs of five fields, converted directly to and from your type.

| | serpent | struct-mapping lib | DOM lib |
|---|---:|---:|---:|
| decode, JSON | 3.15 ms | **0.51 ms** (6.2x faster) | 5.48 ms (1.7x slower) |
| encode, JSON | 3.36 ms | **0.34 ms** (9.8x faster) | 4.43 ms (1.3x slower) |
| decode, BJData | 1.73 ms | — | 6.08 ms (3.5x slower) |
| encode, BJData | 1.06 ms | — | 2.76 ms (2.6x slower) |
| allocations | 15 | 15 | 70,028 |

**This is serpent's own use case and it loses it by 6-10x.** The struct-mapping library reaches
the same allocation count and is far quicker at the parsing itself. That gap is implementation
headroom, not an architectural limit — nothing about the design requires it.

Against the DOM library serpent is ahead on every row, and by more in the binary format, where
there is no text to parse and the difference is almost entirely the object graph the other one
builds.

## Reading a whole document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| sum every coordinate, canada.json | 10.2 ms | 1.96 ms | **1.21 ms** | 1.82 ms | 11.5 ms |
| count every value, citm_catalog.json | 4.83 ms | **0.32 ms** | 0.40 ms | 0.97 ms | 6.61 ms |

**serpent is 5-15x slower than the indexed parsers here.** The reason is structural: they index
or materialize the document once and then walk pointers, while serpent re-scans the bytes on
every step. Traverse a document completely and you pay that scan over and over; they pay it
once.

A structural scan that returns no values — `json::validate` — takes 0.72 ms on the same file,
so most of the 4.83 ms is the repeated re-walking, not the parsing.

## Reading part of a document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| first key of citm_catalog.json | **0.08 µs** | 160 µs | 352 µs | 918 µs | 6436 µs |
| last key of the same file | 721 µs | **223 µs** | 352 µs | 921 µs | 6407 µs |
| sum ids, twitter.json | 303 µs | **91 µs** | 135 µs | 859 µs | 2626 µs |

The two citm rows are the same operation on the same file. serpent stops as soon as it finds
the key: 0.08 µs at the front of the document, 721 µs at the back. Everything else pays for the
whole document either way, which is why their numbers barely move between the rows.

So the win is real but narrow: **reach a field early and nothing else is close; read the whole
thing and serpent is last but one.**

## Allocations

Every scan and lookup above is **zero allocations** in serpent. Nothing is allocated to read a
document, only to hold what you ask it to keep.

The other counts need a caveat, and the table marks them: the on-demand and fast-DOM libraries
allocate through `malloc`, which the counter replaces `operator new` to measure and therefore
cannot see. Their real figure is not zero — it is unmeasured. Only serpent, the struct-mapping
library and the DOM library are counted.

The struct benchmarks show 15-20 allocations, which is container growth rather than per-value:
the sample strings are short enough for the small-string optimization, so they never reach the
allocator. Longer strings would allocate in every library.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types, and want the most speed | a struct-mapping library is 6-10x quicker |
| traverse whole documents repeatedly | an indexed parser is 5-15x quicker |
| want a mutable document object | serpent has none at all |
| need BJData and JSON from one definition | serpent |
| cannot allocate while reading | serpent |

## Reproducing

```bash
cmake -S . -B build-bench -DSERPENT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target bench
```

Off by default: it fetches four libraries and 4.6 MB of corpus.

Every measured computation is run against all of the others first and must produce the same
answer before any timing is printed — nineteen checks, including our BJData being read back by
the DOM library. A lazy reader that quietly returned nothing would otherwise look extremely
fast, and the whole-document benchmark caught exactly that: asking the on-demand parser for its
root field count only touches the top level, so it had to be replaced with a real recursive
traversal before the row meant anything.

Measured on an Apple M4 Pro, macOS 26.5, Apple clang 21, `-O3 -DNDEBUG`, best of seven rounds.
