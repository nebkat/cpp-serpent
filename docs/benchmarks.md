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
| serpent | 405 | **526** | **120** | **150** |
| serpent, BJData for size |  |  | 188 | 233 |
| struct-mapping lib | **286** | 568 | 154 | 159 |
| the same, CBOR |  |  | 208 | 213 |
| the same, built for size | 350 | 864 |  |  |
| DOM lib | 4,345 | 7,615 | 3,624 | 6,283 |

Decoding, serpent allocates once per string and once for the container; the struct-mapping
library 10,015 times and the DOM library 70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | 1.08 | 1.36 | **0.51** | 1.01 | 1.42 |
| JSON decode | **0.89** | **0.76** | 1.15 | 1.16 | 1.68 |
| binary encode | 1.07 | 1.30 | 1.22 | 1.25 | 1.46 |
| binary decode | **0.86** | 1.44 | **0.98** | **0.96** | 1.18 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 526 | 688 |
| JSON encode | 405 | 1,120 |
| JSON encode, five reals | 357 | 2,031 |
| JSON encode, five integers | 122 | 377 |
| binary decode | 150 | 147 |
| binary encode | 120 | 183 |

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
| serpent | **0.07** | 706 | 252 | 1,193 | 2,808 |
| serpent, over an index built first | 782 | 788 | 282 | 851 | 3,063 |
| on-demand parser | 179 | **277** | **111** | **313** | 1,630 |
| fast DOM A | 369 | 389 | 144 | 418 | **1,260** |
| fast DOM B | 874 | 890 | 760 | 919 | 1,902 |
| DOM lib | 7,305 | 7,423 | 3,421 | 7,479 | 13,035 |

serpent reads in place and builds nothing, so what it does not look at costs nothing and it never
allocates; walking a whole document it is 2-4x behind parsers that index or build one first.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types | level on JSON decode and binary decode, ahead on binary encode, 1.4x behind on JSON encode |
| traverse whole documents repeatedly | an indexing parser is 2-4x quicker |
| want a mutable document object | serpent has none |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
