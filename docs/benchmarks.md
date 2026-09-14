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

Ten thousand structs of five fields, both libraries in one process on one compiler.

| | serpent (BJData) | struct-mapping lib (BEVE) | its CBOR | its MessagePack |
|---|---:|---:|---:|---:|
| encode | 0.23 ms | **0.09 ms** | 0.20 ms | 0.09 ms |
| decode | 0.32 ms | **0.16 ms** | 0.18 ms | 0.08 ms |
| allocations, decode | **1** | **1** | 1 | 1 |
| output size | 844,694 B | 828,964 B | 764,692 B | 358,964 B |

The last two columns are the same library writing *value-directed* formats — the kind BJData is,
where a marker is chosen per value rather than fixed by the declared type. They are the fairer
comparison, and against its CBOR we are within 1.2x on encode.

### It is not that the other format is cleverer

The obvious explanation is wrong, and the byte traces say so. One record:

```
BJData  73 B   {U·9 timestamp m ····      U·7 celsius h ··   …
BEVE    83 B   ·· ·$ timestamp q ········    ·· celsius a ········ …
CBOR    69 B   ·  i timestamp ·a eS·· …
```

BEVE is the **least** compact of the four and the fastest. It writes the tag its C++ type
dictates and a full-width payload — `uint64_t` is always eight bytes. BJData chooses a marker
from the *value*: that `uint64` became a four-byte `m`, that `double` a two-byte `h`. CBOR and
MessagePack narrow too and come out smaller still, because their type and length share one byte
where BJData spends a marker plus a length.

And choosing those markers is free. Encoding with `compact_types` off, so every marker comes
from the type exactly as BEVE does, measures the same to within noise. The decisions were never
the cost.

### What the cost actually was

Four things, each found by measurement and each now fixed:

| | was |
|---|---|
| every record in a sequence was parsed **twice** — read, then skipped again to find the next | 1030 → 540 µs |
| a key was framed at run time: length marker, length, bytes, as separate writes | 625 → 276 µs |
| `put()` was too large to inline, so a one-byte marker became a call, and the copy inside it a call to `memcpy` | 273 → 237 µs |
| a string's length prefix was read once for the text and again to step over it | 540 → 442 µs |

None of that was the format. It was a reader built from handles that did not know what the
caller wanted, and a writer assembling constants a byte at a time.

### One measurement caveat worth knowing

serpent's timings move with what the process did beforehand and the comparison library's do
not: in a run doing nothing else, encode is 0.23 ms; in one that has already worked through the
document benchmarks above, the same call measures 0.40 ms, while the other library sits at
0.09 ms in both. The figures here are from the quiet run. Something in our working set survives
less well across other work, and it is not yet understood — worth knowing if your own use is
occasional rather than in a tight loop, because the cold number is the honest one there.

### What is left

The remaining ~2.7x is two things, one fixable and one not.

**Fixable:** the other library turns a key into a field index with a compile-time perfect hash,
then verifies with a fixed-length compare and dispatches through a jump table. serpent compares
against each field's name in turn. For five fields that is a few compares against one hash — worth
something, at the cost of a good deal of machinery.

**Not fixable:** BEVE packs type, width and signedness into the bits of one tag byte, so
`width = 1 << (tag >> 5)` and dispatch is arithmetic. BJData markers are ASCII letters chosen
per value — `'U'`, `'i'`, `'m'`, `'D'` — with no structure to exploit, so every value costs a
lookup where BEVE costs a shift. That is the format, and it is the price of a wire encoding you
can read in a hex dump.

## Your own types, as JSON

| | serpent | struct-mapping lib | DOM lib |
|---|---:|---:|---:|
| decode | 3.78 ms | **0.63 ms** (6.0x faster) | 7.35 ms (1.9x slower) |
| encode | 1.51 ms | **0.29 ms** (5.3x faster) | 4.88 ms (3.2x slower) |
| allocations, decode | **15** | 10,015 | 70,029 |
| allocations, encode | **13** | 12 | 70,023 |

**This is serpent's own use case and it loses it by 5-6x.** That gap is implementation headroom
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

The struct benchmarks show 13-15 allocations, which is container growth rather than per-value:
the sample strings are short enough for the small-string optimization, so they never reach the
allocator. Longer strings would allocate in every library.

Writing JSON was the exception until recently, at 25,437 allocations for the same work - two
intermediate strings per real, in the number formatting rather than in the writer. Composing
into a fixed buffer took it to 13, and the encode itself from 2.66 ms to 1.51 ms.

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
