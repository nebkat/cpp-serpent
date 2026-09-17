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

### Describing a type beats converting it by hand

The figures above are for an annotated type, which is what the library wants you to write. The
same five fields behind a hand-written `json_convert` decode from BJData in 1.47 ms rather than
0.45 ms — **3.2x** — because a described type gets a reader generated for it that walks the
document once, where a hand-written conversion goes through the general iterators, parsing each
entry into a key and a handle and comparing the key at run time.

That gap was 4.4x until the BJData view learned to step over an element something has already
walked, rather than scanning it a second time to find the next one. JSON has kept such a note
for a while; the binary reader had not, so every container read through a hand-written
conversion was scanned twice over.

Hand-writing is still the right answer when the two directions genuinely differ, or on a
toolchain with no reflection. It is not the right answer for speed.

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
| decode | 0.64 ms | **0.56 ms** (1.2x faster) | 7.56 ms (12x slower) |
| encode | 1.21 ms | **0.28 ms** (4.4x faster) | 4.85 ms (4.0x slower) |
| allocations, decode | **15** | 10,015 | 70,029 |
| allocations, encode | 17 | **12** | 70,023 |

The answer depends a good deal on what the fields are. Five of one type at a time, ten thousand
records, against the same library — below 1.0 is serpent ahead:

| | strings | booleans | integers | reals |
|---|---:|---:|---:|---:|
| decode | **0.57x** | **0.69x** | 1.29x | 1.68x |
| encode | 1.73x | 4.90x | 2.27x | 6.27x |

Decoding is won or lost on everything *around* the conversion, and a reader generated for a type
can expect the bytes it would have written rather than classify them: a key, its quotes, its
colon and the comma before it are one comparison. Where the conversion itself is most of the
work - a real - the two libraries converge on the cost of the conversion, and serpent's is
`std::from_chars` behind a grammar check where the other library has its own parser.

Encoding is the other way round. What is left there is almost entirely number formatting:
serpent uses `std::to_chars`, and for a real lays the digits out again to match the reference
implementation byte for byte, which together cost about 40 ns where the other library's own
table-driven formatter costs 6. The record above holds two reals, and they are two thirds of
its encode time. Closing that means shipping a float formatter, which this library has chosen
not to do; hand-written appends to a `std::string` with `std::to_chars` measure within a fifth
of serpent on every row, so the machinery around the conversion is no longer the cost. That gap is implementation headroom
rather than an architectural limit: the other library builds each key's `"name":` at compile
time and emits it as one fixed-size copy, writes into a pre-padded buffer by index instead of
through a call, and carries its own number conversion.

Against the DOM library serpent is ahead on every row, and by more in binary, where there is no
text to parse and the difference is almost entirely the object graph the other one builds.

## Reading a whole document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| count every value, citm_catalog.json | 1.24 ms | **0.31 ms** | 0.43 ms | 0.90 ms | 7.39 ms |
| the same, with an index built first | 0.87 ms | | | | |
| sum every coordinate, canada.json | 3.69 ms | 1.63 ms | **1.26 ms** | 1.92 ms | 13.26 ms |
| the same, with an index built first | 2.97 ms | | | | |

A structural scan that returns no values — `json::validate` — takes 0.74 ms on citm, so
traversal costs about 1.7x merely checking the same file.

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
| first key of citm_catalog.json | **0.09 µs** | 181 µs | 369 µs | 873 µs | 7304 µs |
| last key of the same file | 729 µs | **274 µs** | 370 µs | 867 µs | 7279 µs |
| sum ids, twitter.json | 297 µs | **114 µs** | 145 µs | 785 µs | 3375 µs |

The two citm rows are the same operation on the same file. serpent stops as soon as it finds
the key: 0.09 µs at the front of the document, 729 µs at the back. Everything else pays for the
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
| convert your own types, and want the most speed | decoding is level, ahead on strings and booleans; encoding reals, a struct-mapping library is 6x quicker |
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
