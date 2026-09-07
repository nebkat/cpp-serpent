# Benchmarks

Against a mainstream DOM library, on the corpus the published C++ JSON benchmarks use
(`canada.json`, `citm_catalog.json`, `twitter.json`).

!!! warning "Read this before the numbers"

    The two libraries are not the same shape. A DOM library parses a document into a mutable
    object graph you can index, mutate and re-dump any number of times. serpent never builds
    one — it decodes straight into your types, or walks the bytes in place.

    Where you actually want a document object, these numbers are not the question you are
    asking, and serpent does not offer one at all. Where you want your own types, or a few
    fields out of a large payload, this is the comparison that matters.

## Your own types

One struct of five fields, ten thousand of them.

| | time | vs | allocations |
|---|---:|---:|---:|
| decode, JSON | 3.14 ms | **1.7x** | 15 vs 70,028 |
| encode, JSON | 3.42 ms | **1.3x** | 16 vs 70,022 |
| decode, BJData | 1.76 ms | **3.5x** | 15 vs 140,025 |
| encode, BJData | 1.09 ms | **2.6x** | 20 vs 70,026 |

The time difference is modest, because both libraries still have to parse every number and
copy every string. The allocation difference is not: the other library builds a `json` node per
value on the way past, and serpent writes into your struct.

## Reading part of a document

| | time | vs |
|---|---:|---:|
| two fields from citm_catalog.json (1.7 MB), first key | 0.7 µs | **9463x** |
| the same, but the *last* key in the document | 0.71 ms | **9.3x** |
| one field from twitter.json (632 KB) | 0.30 ms | **9.0x** |
| walk every value in citm_catalog.json | 0.73 ms | **9.0x** |

Those two citm rows are the same operation on the same file, and the gap between them is the
whole story: the reader scans forward and stops when it finds the key. Hit the first key and it
touches almost nothing; hit the last and it reads the document through. **9463x is the best
case, not the typical one** — 9x is.

Building a DOM costs the same either way, which is why the other library's number barely moves.

## Reading all of it

| | time | vs |
|---|---:|---:|
| sum every coordinate in canada.json (2.2 MB) | 10.1 ms | **1.15x** |
| sum every id in twitter.json | 0.31 ms | **8.6x** |

canada.json is the honest case for a no-DOM design: it is almost entirely doubles, both
libraries have to parse all of them, and parsing dominates everything else. A 15% edge is what
is left once there is no structure to skip.

twitter.json is mostly strings and objects that the query never looks at, so skipping them is
most of the work saved.

## Allocations

serpent allocates only where a result needs to own memory. Walking a document allocates
nothing at all — every one of the scan and lookup benchmarks above is zero allocations.

The struct benchmarks are 15-20, which is vector growth rather than per-value: the station
names in the sample are short enough for the small-string optimization, so they never reach
the allocator. Longer strings would allocate in both libraries — the ratio would narrow, and
the zero-allocation scans would not change.

## Reproducing

```bash
cmake -S . -B build-bench -DSERPENT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target bench
```

It fetches the comparison library (single header, pinned and hash-checked) and the corpus, so
it is off by default.

Every measured computation is run against the other library first and has to produce the same
answer before any timing is reported — including our BJData being read back by the other
library. A lazy reader that quietly returned nothing would otherwise look extremely fast.

Measured on an Apple M4 Pro, macOS 26.5, Apple clang 21, `-O3 -DNDEBUG`, best of seven rounds.
Your numbers will differ; the command above is the point.
