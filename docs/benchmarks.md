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
| serpent | **246** | **482** | **123** | **147** |
| serpent, BJData for size |  |  | 196 | 231 |
| struct-mapping lib | 285 | 560 | 157 | 163 |
| the same, CBOR |  |  | 210 | 203 |
| the same, built for size | 344 | 866 |  |  |
| DOM lib | 4,326 | 7,496 | 3,578 | 5,952 |

Decoding, serpent allocates once per string and once for the container; the struct-mapping
library 10,015 times and the DOM library 70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | **0.74** | 1.38 | **0.50** | 1.03 | **0.83** |
| JSON decode | **0.84** | **0.76** | 1.09 | 1.08 | 1.31 |
| binary encode | 1.07 | **0.93** | **0.99** | 1.01 | 1.06 |
| binary decode | **0.79** | **0.90** | **0.84** | **0.96** | 1.19 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 482 | 710 |
| JSON encode | 246 | 1,247 |
| JSON encode, five reals | 363 | 2,397 |
| JSON encode, five integers | 119 | 408 |
| binary decode | 147 | 141 |
| binary encode | 123 | 173 |

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
| serpent | **0.06** | 706 | 251 | 806 | 2,723 |
| serpent, over an index built first | 784 | 800 | 289 | 859 | 2,974 |
| on-demand parser | 179 | **271** | **110** | **308** | 1,619 |
| fast DOM A | 366 | 366 | 143 | 413 | **1,266** |
| fast DOM B | 851 | 868 | 757 | 887 | 1,995 |
| DOM lib | 7,216 | 7,233 | 3,351 | 7,317 | 12,605 |

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
