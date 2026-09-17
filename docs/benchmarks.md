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

Ten thousand structs of five fields, both libraries in one process on one compiler.

| | serpent (BJData) | struct-mapping lib (CBOR) | the same (BEVE) | DOM lib (CBOR) |
|---|---:|---:|---:|---:|
| encode | 0.16 ms | 0.20 ms | **0.15 ms** | 3.57 ms |
| decode | 0.23 ms | 0.18 ms | **0.16 ms** | 5.79 ms |
| allocations, encode | **10** | 12 | 12 | 120,026 |
| allocations, decode | **1** | **1** | **1** | 140,025 |
| output size | 844,694 B | **764,692 B** | 828,964 B | 766,100 B |

BEVE is the struct-mapping library's own format: it knows the schema, so a field is a position
and a payload. CBOR is what BJData is — every value tagged, every key named, the marker chosen
from the value rather than fixed by the declared type — so it is the column that holds the format
constant and varies only the library.

### It was never the format

These rows used to read 0.43 and 0.45 ms, 2.6x behind, and this page used to explain part of that
by the format: BJData's markers are ASCII letters with no structure, where BEVE's tag is bit
fields that dispatch by arithmetic. That was wrong, and the way it was shown to be wrong is worth
keeping. The plainest possible BJData writer and reader were written by hand for this one record
— a constant for each key, a `switch` on the marker, a `memcpy` — producing and reading exactly
the same bytes:

| | plain code, by hand | serpent, then | struct-mapping lib (CBOR) | the same (BEVE) |
|---|---:|---:|---:|---:|
| encode | **0.065 ms** | 0.45 ms | 0.19 ms | 0.15 ms |
| decode | **0.127 ms** | 0.46 ms | 0.14 ms | 0.16 ms |

Plain code over BJData is faster than the fastest library over its own format. Choosing a marker
from the value, naming every key, letters for markers: none of it costs anything that matters.
Everything serpent lost, it lost to its own machinery.

### What the cost actually was

| | 10k records |
|---|---|
| **writing** | |
| a key went through `put(span)`, which in a large translation unit is not inlined: a call, and a copy of run-time width. Now `put_constant`, its width in its type | 451 → 370 µs |
| a marker and its payload were two writes through a `switch`. Now one piece: eight bytes stored, as many kept as the marker says | 370 → 208 µs |
| each key and each value asked for room. Runs of number and boolean members now ask once, as JSON's do | 208 → 176 µs |
| a string's marker and its length were two writes | 176 → 165 µs |
| **reading** | |
| every key was tried against every entry, each value was read through a view, and then stepped over a second time. Now the members are expected as this library writes them and read straight off the cursor, and only what that leaves is matched by name | 457 → 303 µs |
| each typed read looked at the marker in three `switch`es: is it an integer, how wide, load it. Now one, whose arms know the type stored | 303 → 277 µs |
| whether a marker opens a value at all was asked before every value. Now only once the typed read has refused it | 277 → 246 µs |
| a string's length was read three calls deep | 246 → 220 µs |

The first reading row hides the finding that mattered most. The new reader, as first written,
measured 435 µs — no better than the old one. The same source with its half-dozen smallest
functions marked always-inline measured 303. GCC inlines against a budget for the whole
translation unit; beside another library's templates the budget runs out, functions of three
lines become calls, and constant widths become run-time arguments. That is what
[`SERPENT_FORCE_INLINE`](configuration.md) is for, it is why a benchmark that lives in a small
file flatters a library, and it explains a caveat this page used to carry without understanding
it: that serpent measured 0.23 ms on its own and 0.43 ms in the full suite. It was never the
caches. It was the size of the file.

Three ideas were tried, measured, and taken back out because they bought nothing: holding the
cursor by value inside the reader, asking binary32 before binary16 when narrowing a real, and a
short way through the parse of a plain object's header.

### What is left

Writing is level with BEVE and ahead of CBOR. Reading is 1.2x behind CBOR, against a ceiling that
says 0.13 ms is there to be had. A profile of what remains has no single feature: half is the
typed reads themselves, and the rest is spread over a string's length and copy, setting up each
object, and the loop over the sequence.

### Describing a type beats converting it by hand

The figures above are for an annotated type, which is what the library wants you to write. The
same five fields behind a hand-written `json_convert` decode from BJData in 1.19 ms rather than
0.20 ms — **6x** — and encode in 0.26 ms rather than 0.16, because a described type gets a reader
and a writer generated for it, where a hand-written conversion goes through the general
iterators, parsing each entry into a key and a handle and comparing the key at run time.

On decode that gap was wider still until the BJData view learned to step over an element something has already
walked, rather than scanning it a second time to find the next one. JSON has kept such a note
for a while; the binary reader had not, so every container read through a hand-written
conversion was scanned twice over.

Hand-writing is still the right answer when the two directions genuinely differ, or on a
toolchain with no reflection. It is not the right answer for speed.

## Your own types, as JSON

| | serpent | struct-mapping lib | the same, built for size | DOM lib |
|---|---:|---:|---:|---:|
| decode | **0.54 ms** | 0.55 ms | 0.86 ms | 7.50 ms (14x slower) |
| encode | 0.42 ms | **0.28 ms** (1.5x faster) | 0.34 ms | 4.31 ms (10x slower) |
| allocations, decode | **15** | 10,015 | 10,015 | 70,029 |
| allocations, encode | 17 | **12** | **12** | 70,023 |

The answer depends a good deal on what the fields are. Five of one type at a time, ten thousand
records, against the same library as it comes — below 1.0 is serpent ahead:

| | strings | booleans | integers | reals |
|---|---:|---:|---:|---:|
| decode | **0.87x** | **0.72x** | 1.12x | 1.18x |
| encode | 1.35x | 2.16x | **0.62x** | 1.18x |

Decoding is won or lost on everything *around* the conversion, and a reader generated for a type
can expect the bytes it would have written rather than classify them: a key, its quotes, its
colon and the comma before it are one comparison.

### What the switches are worth

Most of what closed this gap is code with a plainer alternative, and every such piece is behind a
switch in [`serpent/config.hpp`](configuration.md) with the plain way kept beside it. The
benchmark is built once per configuration, so what each is worth is measured, not remembered:

| 10k records, µs | default | large | small | plain | struct-mapping lib | the same, for size |
|---|---:|---:|---:|---:|---:|---:|
| decode, mixed | 537 | 546 | 687 | 691 | 553 | 858 |
| encode, mixed | 418 | 412 | 455 | 1,216 | 282 | 343 |
| decode, five integers | 316 | 321 | 337 | 330 | 282 | 470 |
| encode, five integers | 148 | 139 | 152 | 376 | 239 | 109 |
| decode, five reals | 633 | 631 | 985 | 978 | 538 | 1,098 |
| encode, five reals | 419 | 416 | 527 | 2,346 | 354 | 488 |
| decode, five strings | 1,007 | 1,019 | 1,005 | 1,157 | 1,156 | 1,216 |
| encode, five strings | 503 | 501 | 509 | 640 | 372 | 409 |
| decode, five booleans | 137 | 137 | 135 | 137 | 190 | 318 |
| encode, five booleans | 98 | 98 | 97 | 220 | 45 | 45 |

`default` is the library as it comes. `large` swaps the 400-byte integer table for the 40 KB one.
`small` is for an image that counts its flash: Żmij without its table of powers of ten and
`std::from_chars` rather than fast_float. `plain` has every switch off, and is the standard
library throughout.

What each switch does, in the order of what it bought:

- **Reals are written by [Żmij](https://github.com/vitaut/zmij)** rather than `std::to_chars`
  and a re-layout: about 47 ns a member became about 8, at the price of [spelling a very large or
  very small real with an exponent](json.md#formatting).
- **Runs of number and boolean members are written into room claimed once.** Each has a longest
  possible text, so a run of them asks for room once rather than once per key and once per
  value. Where that much room cannot be had in one piece - a fixed buffer near its end - they are
  written one at a time as before, so a document that fits still fits.
- **Integers are written two digits at a time** from a 400-byte table, or four from a 40 KB one,
  which is Glaze's formatter under its licence. The large table is worth another 6% here.
- **Reals are read by [fast_float](https://github.com/fastfloat/fast_float) in its JSON mode**,
  in one walk, where `std::from_chars` accepts more than JSON does and so needs the grammar
  checked first.
- **A string's plain text is found eight bytes at a time**, reading and writing.

What is left on the encode rows is the writer itself. It is a general, stateful writer that
anyone can drive by hand and that checks what it is asked to do — a key outside an object is an
error, not a malformed document — where the other library writes into a pre-sized buffer by
index. A boolean is where that shows most, because there is nothing else in it to cost anything.

One row is not what it seems: the other library's own integer encode is *faster built for size*
(109 µs) than as it comes (239 µs), on every distribution of values tried. That is its object
writer, not its integer formatter, and it is why the integer column flatters serpent.

Against the DOM library serpent is ahead on every row, and by more in binary, where there is no
text to parse and the difference is almost entirely the object graph the other one builds.

## Reading a whole document

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| count every value, citm_catalog.json | 1.24 ms | **0.31 ms** | 0.43 ms | 0.90 ms | 7.39 ms |
| the same, with an index built first | 0.87 ms | | | | |
| sum every coordinate, canada.json | 2.80 ms | 1.62 ms | **1.25 ms** | 1.97 ms | 13.36 ms |
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
| convert your own types, and want the most speed | decoding is level, ahead on strings and booleans; encoding, a struct-mapping library is about 1.5x quicker |
| traverse whole documents repeatedly | an indexed parser is 3-4x quicker |
| want a mutable document object | serpent has none at all |
| need BJData and JSON from one definition | serpent |
| cannot allocate while reading | serpent |

## Reproducing

```bash
tools/bench.sh                      # everything, in every configuration
tools/bench.sh --filter JSON        # only what has JSON in its group or library name
tools/bench.sh --quick              # a few samples each: that it runs, not numbers to quote
```

That configures a release build with GCC 16 if there is one (`CXX=` to choose), builds the
benchmark once for each configuration of the library's switches - `default`, and `plain` with all
of them off - runs each, and lays the results side by side. Timing is
[nanobench](https://nanobench.ankerl.com)'s: each table it prints is relative to serpent, with
the spread of the samples beside every figure, and a figure it marks unstable is one to run again
rather than quote. Each run also writes `results/<configuration>.json` under the build directory,
which is what `tools/bench-summary.py` reads.

By hand, the same thing is

```bash
cmake -S . -B build-bench -DSERPENT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target bench
```

and one program on its own is `build-bench/benchmark/serpent_bench_default --help`.

Off by default: it fetches five libraries and 4.6 MB of corpus.

Every measured computation is run against all of the others first and must produce the same
answer before any timing is printed — twenty-two checks, including our BJData being read back by
the DOM library. A lazy reader that quietly returned nothing would otherwise look extremely
fast, and the whole-document benchmark caught exactly that: asking the on-demand parser for its
root field count only touches the top level, so it had to be replaced with a real recursive
traversal before the row meant anything.

Measured on an Apple M4 Pro, macOS 26.5, `-O3 -DNDEBUG`, medians as nanobench takes them, every table
GCC 16 with `-freflection` — the path serpent means you to use, and one compiler throughout, since
comparing two libraries built by different ones says more about the compilers than the libraries.

The machine was not idle. Absolute figures are therefore a ceiling rather than a best case, and
the run was repeated to make sure they mean something: two runs agreed to 0.7% at the median and
5.7% at the worst. A third, taken under half again as much load, moved the absolute numbers by a
third — and left every ratio in these tables unchanged but the one block it was disturbed in.
Ratios between libraries measured in one process survive a busy machine; single absolute numbers
quoted from one run do not.
