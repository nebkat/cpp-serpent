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

## Binary against binary

The comparison that matters if you are here for BJData, and the one with no character lookup
tables in it: the struct-mapping library's tables — digit classification, escape decoding,
integer digit pairs — all live in its JSON text path. Its binary format touches none of them.

Ten thousand structs of five fields.

| | serpent (BJData) | struct-mapping lib (BEVE) | DOM lib (CBOR) |
|---|---:|---:|---:|
| encode | 0.28 ms | **0.09 ms** (3.1x faster) | 3.57 ms |
| decode | 1.30 ms | **0.16 ms** (8.1x faster) | 6.43 ms |
| allocations, decode | **1** | **1** | 140,025 |
| bytes allocated | **560,000** | **560,000** | 10,364,344 |
| output size | 844,694 B | 828,964 B | 766,100 B |

So the gap is not the text parsing and it is not the tables.

**Allocation behaviour is identical** — one allocation, the same 560,000 bytes — because a
sized array states its length, which costs three bytes on this document. See
[counted arrays](bjdata.md#counted-arrays).

**Time is 8.1x on decode and 3.1x on encode**, and one thing has to be said about that number
before the breakdown: it is bigger than it used to be, and not because anything got slower.

Everything in this table is built with the same compiler. Measured under Clang the gap is 5.3x,
under GCC 16 it is 8.1x — the other library is a third quicker on the newer compiler and we are
not. It is written almost entirely as templates resolved at compile time, which the newer
optimiser rewards; our reader is a handful of ordinary functions walking a buffer, which it has
little left to do with. Quoting the smaller figure by measuring the two under different
compilers would have been flattering and wrong.

Where our decode time goes, measured on the generic walk:

| | ns | share |
|---|---:|---:|
| walking the bytes at all — every record skipped, nothing decoded | 508k | 36% |
| iterating the members of each object, above that | 411k | 29% |
| decoding the values | 87k | 6% |
| filling the struct | 376k | 27% |

Decoding values is almost free. The cost is traversal, and the first row is the important one:
**the other library decodes the whole document in less time than it takes us merely to walk
past it.**

Where the compiler can enumerate a type's fields, BJData skips the generic walk entirely and
reads through [a reader generated for that type](reflection.md) — no iterator, no intermediate
handle, one inlined comparison per field against a constant of known length. That is worth
about a third, and it is included in the figure above. What is left is what a self-describing
format costs to walk, one marker and one bounds check at a time, against a parser emitted for
one struct.

## Your own types, as JSON

| | serpent | struct-mapping lib | DOM lib |
|---|---:|---:|---:|
| decode | 3.18 ms | **0.52 ms** (6.1x faster) | 5.58 ms (1.8x slower) |
| encode | 2.80 ms | **0.35 ms** (8.1x faster) | 4.45 ms (1.6x slower) |
| allocations | 15 | 15 | 70,028 |

**This is serpent's own use case and it loses it by 6-8x.** That gap is implementation headroom
rather than an architectural limit: the other library builds each key's `"name":` at compile
time and emits it as one fixed-size copy, writes into a pre-padded buffer by index instead of
through a call, and carries its own number conversion.

Against the DOM library serpent is ahead on every row, and by more in binary, where there is no
text to parse and the difference is almost entirely the object graph the other one builds.

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

Measured on an Apple M4 Pro, macOS 26.5, `-O3 -DNDEBUG`, best of seven rounds. The binary table
is GCC 16 with `-freflection`, which is the path serpent means you to use; the JSON and
document tables are Apple clang 21. Every row within a table is the same compiler — comparing
two libraries built by different ones says more about the compilers than the libraries.
