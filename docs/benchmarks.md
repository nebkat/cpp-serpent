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

| | serpent (BJData) | struct-mapping lib (CBOR) | the same (BEVE) | DOM lib (CBOR) |
|---|---:|---:|---:|---:|
| encode | 0.43 ms | 0.19 ms | **0.16 ms** | 4.04 ms |
| decode | 0.45 ms | 0.18 ms | **0.17 ms** | 5.80 ms |
| allocations, encode | 13 | **12** | **12** | 120,026 |
| allocations, decode | **1** | **1** | **1** | 140,025 |
| output size | 844,694 B | **764,692 B** | 828,964 B | 766,100 B |

The second column is the comparison that settles the question, and it is the one this page used
to lack. BEVE is the struct-mapping library's own format: it knows the schema, so a field is a
position and a payload with no key on the wire at all. CBOR is what BJData is — every value
tagged, every key named, the marker chosen from the value rather than fixed by the declared
type. Holding the format constant and varying only the library is the only way to ask whether
the format explains the gap.

### It is not that the other format is cleverer

It reads its own schema-driven BEVE in 0.17 ms and a fully key-tagged CBOR in 0.18 ms — a 5%
difference. Carrying keys and per-value markers costs that library almost nothing, so the 2.6x
it has on us is not the wire format. It is the implementation, and the rest of this section is
what that turned out to mean.

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
not: measured on its own, encode is 0.23 ms; measured in the full suite, after the document
benchmarks have been through the same caches, the same call takes 0.43 ms, while the other
library sits within a few percent of its own figure either way.

**The table above quotes the full-suite number**, because that is what the published harness
prints and what anyone re-running it will see. The isolated figure is the better one for a
process that does nothing else, and the gap between the two is a property of serpent worth
knowing: something in our working set survives other work less well than the alternatives, and
it is not yet understood.

### What is left

The remaining 2.6x on decode, against the same library reading the same kind of format, is two
things — one fixable and one not.

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
| decode | 2.21 ms | **0.56 ms** (4.0x faster) | 7.30 ms (3.3x slower) |
| encode | 1.48 ms | **0.28 ms** (5.3x faster) | 4.80 ms (3.2x slower) |
| allocations, decode | **15** | 10,015 | 70,029 |
| allocations, encode | 17 | **12** | 70,023 |

**This is serpent's own use case and it loses it by 4-5x.** That gap is implementation headroom
rather than an architectural limit: the other library builds each key's `"name":` at compile
time and emits it as one fixed-size copy, writes into a pre-padded buffer by index instead of
through a call, and carries its own number conversion.

Against the DOM library serpent is ahead on every row, and by more in binary, where there is no
text to parse and the difference is almost entirely the object graph the other one builds.

## Reading a whole document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| count every value, citm_catalog.json | 1.16 ms | **0.30 ms** | 0.42 ms | 0.89 ms | 7.35 ms |
| the same, with an index built first | 0.86 ms | | | | |
| sum every coordinate, canada.json | 4.58 ms | 1.63 ms | **1.26 ms** | 1.94 ms | 12.60 ms |
| the same, with an index built first | 2.94 ms | | | | |

A structural scan that returns no values — `json::validate` — takes 0.73 ms on citm, so
traversal costs about 1.6x merely checking the same file.

An [index](reading.md#an-index-for-a-document-read-more-than-once) is the answer when a document
is walked more than once: it costs about what validating costs to build, and takes a third off
every traversal after that.

The indexed parsers keep a lead, and part of it is vectorisation: the on-demand parser ships a
scalar kernel too, and switching SIMD off costs it 2.1–2.5×, so the vector instructions are the
*smaller* half of its advantage. The rest is a per-byte scanning rate within about 25% of that
scalar build.

One property no tuning changes: a lazy reader passes over the bytes of every value you want
twice — once to find it, once to convert it — where an eager parser reads once and keeps the
result. Ask for every value in a document and laziness is a straight loss. Ask for a few and see
the next table.

## Reading part of a document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| first key of citm_catalog.json | **0.09 µs** | 180 µs | 367 µs | 850 µs | 7213 µs |
| last key of the same file | 709 µs | **272 µs** | 370 µs | 856 µs | 7267 µs |
| sum ids, twitter.json | 293 µs | **111 µs** | 145 µs | 781 µs | 3329 µs |

The two citm rows are the same operation on the same file. serpent stops as soon as it finds
the key: 0.09 µs at the front of the document, 709 µs at the back. Everything else pays for the
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

The struct benchmarks show 13-17 allocations, which is container growth rather than per-value:
the sample strings are short enough for the small-string optimization, so they never reach the
allocator. Longer strings would allocate in every library. Reserve the destination and the count
falls to one, because a sink over contiguous storage is
[written into in place](writing.md#sizing-the-destination).

Writing JSON was the exception until recently, at 25,437 allocations for the same work - two
intermediate strings per real, in the number formatting rather than in the writer. Composing
into a fixed buffer took it to 13.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types, and want the most speed | a struct-mapping library is 4-5x quicker |
| traverse whole documents repeatedly | an indexed parser is 3-4x quicker |
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
answer before any timing is printed — twenty-two checks, including our BJData being read back by
the DOM library. A lazy reader that quietly returned nothing would otherwise look extremely
fast, and the whole-document benchmark caught exactly that: asking the on-demand parser for its
root field count only touches the top level, so it had to be replaced with a real recursive
traversal before the row meant anything.

Measured on an Apple M4 Pro, macOS 26.5, `-O3 -DNDEBUG`, best of seven rounds, every table
GCC 16 with `-freflection` — the path serpent means you to use, and one compiler throughout, since
comparing two libraries built by different ones says more about the compilers than the libraries.

The machine was not idle. Absolute figures are therefore a ceiling rather than a best case, and
the run was repeated to make sure they mean something: two runs agreed to 0.7% at the median and
5.7% at the worst. A third, taken under half again as much load, moved the absolute numbers by a
third — and left every ratio in these tables unchanged but the one block it was disturbed in.
Ratios between libraries measured in one process survive a busy machine; single absolute numbers
quoted from one run do not.
