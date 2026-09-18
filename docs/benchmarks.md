# Benchmarks

Apple M4 Pro, GCC 16 `-O3 -freflection`, every library in one process, medians by
[nanobench](https://nanobench.ankerl.com). Ten thousand records of five members unless it says
otherwise. Times in µs; **bold** is the fastest of a row. The comparisons are a struct-mapping
library (in its normal build and its build for size), a DOM library, an on-demand parser and two
fast DOM parsers.

```bash
tools/bench.sh                      # everything, in every configuration
tools/bench.sh --filter binary      # only what has "binary" in its name
tools/bench.sh --quick              # that it runs, not numbers to quote
tools/bench.sh --ablations          # the default with each switch off in turn
```

Every measured computation is first checked to produce the same answer in every library.

## Your own types

Ten thousand records of five members: an integer, a string, two reals and a boolean. Binary is
each library's own format - BJData [written for speed](bjdata.md#size-or-speed) for serpent,
BEVE for the struct-mapping library, CBOR for the DOM library - and CBOR through the
struct-mapping library too, which holds the format still.

| | JSON encode | JSON decode | binary encode | binary decode |
|---|---:|---:|---:|---:|
| serpent | 419 | **543** | **117** | **145** |
| serpent, BJData for size | | | 190 | 230 |
| struct-mapping lib | **281** | 555 | 152 | 163 |
| the same, CBOR | | | 210 | 214 |
| the same, built for size | 342 | 860 | | |
| DOM lib | 4,335 | 7,468 | 3,581 | 6,011 |

Decoding, serpent allocates once per string and once for the container; the struct-mapping
library 10,015 times and the DOM library 70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | 1.13 | 2.31 | **0.63** | 1.32 | 1.44 |
| JSON decode | **0.87** | **0.74** | 1.15 | 1.18 | 1.86 |
| binary encode | 1.04 | 1.28 | 1.22 | 1.24 | 1.38 |
| binary decode | **0.78** | 1.43 | **0.98** | **1.00** | 1.21 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 511 | 677 |
| JSON encode | 426 | 1,173 |
| JSON encode, five reals | 415 | 2,203 |
| JSON encode, five integers | 141 | 377 |
| binary decode | 145 | 290 |
| binary encode | 117 | 153 |

## It was never the format

Binary used to be 2.6x behind, and this page used to blame part of that on BJData: a marker per
value, a name per key, letters for markers. So the plainest possible writer and reader were
written by hand for the one record — a constant per key, a `switch` on the marker, a `memcpy` —
producing the same bytes: **65** to encode and **127** to decode, faster than any library here
over any format. Everything serpent lost, it lost to its own machinery, and mostly to one thing:
GCC inlines against a budget for the whole translation unit, and beside another library's
templates the budget runs out. Functions of three lines became calls and constant widths became
run-time arguments; the same source measured 540 and then 303 with six functions marked
always-inline. That is [`SERPENT_FORCE_INLINE`](configuration.md), and it is why a benchmark in a
small file flatters a library.

## Reading a document without a type

| | first key of citm | last key of citm | sum ids, twitter | count every value, citm | sum coordinates, canada |
|---|---:|---:|---:|---:|---:|
| serpent | **0.07** | 688 | 248 | 1,187 | 3,084 |
| serpent, over an index built first | 766 | 763 | 281 | 845 | 3,244 |
| on-demand parser | 178 | **271** | **109** | **310** | 1,608 |
| fast DOM A | 377 | 372 | 143 | 419 | **1,246** |
| fast DOM B | 936 | 861 | 755 | 902 | 1,940 |
| DOM lib | 8,194 | 7,213 | 3,363 | 7,339 | 12,717 |

serpent reads in place and builds nothing, so what it does not look at costs nothing and it never
allocates; walking a whole document it is 2-4x behind parsers that index or build one first.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types | level on JSON decode, ahead on binary in both directions, 1.5x behind on JSON encode |
| traverse whole documents repeatedly | an indexing parser is 2-4x quicker |
| want a mutable document object | serpent has none |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
