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

JSON is decoded from a `std::string`, which both serpent and the struct-mapping library read
[trusting the zero byte after it](reading.md#text-with-a-zero-byte-after-it-is-read-faster)
rather than checking where the text ends - the struct-mapping library's default, and what a
`std::string` gets from serpent. The rows marked bounded are each library reading the same text
with every byte checked, which is what a buffer with nothing known past its end costs.

## Your own types

Ten thousand records of five members: an integer, a string, two reals and a boolean. Binary is
each library's own format - BJData [written for speed](bjdata.md#size-or-speed) for serpent,
BEVE for the struct-mapping library, CBOR for the DOM library - and CBOR through the
struct-mapping library too, which holds the format still.

| | JSON encode | JSON decode | binary encode | binary decode |
|---|---:|---:|---:|---:|
| serpent | **233** | **417** | **119** | **152** |
| serpent, bounded |  | 475 |  |  |
| serpent, BJData for size |  |  | 196 | 228 |
| struct-mapping lib | 284 | 585 | 154 | 164 |
| the same, bounded |  | 698 |  |  |
| the same, CBOR |  |  | 235 | 192 |
| the same, built for size | 343 | 852 |  |  |
| DOM lib | 4,317 | 7,893 | 3,564 | 5,824 |

Decoding, serpent allocates once per string and once for the container; the struct-mapping
library 10,015 times and the DOM library 70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | **0.55** | 1.33 | **0.50** | 1.04 | **0.82** |
| JSON decode | **0.80** | **0.78** | **0.85** | **0.76** | **0.84** |
| binary encode | 1.01 | **0.93** | **0.97** | 1.00 | 1.04 |
| binary decode | **0.81** | **0.98** | **0.98** | **0.98** | 1.18 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 417 | 676 |
| JSON encode | 233 | 1,193 |
| JSON encode, five reals | 365 | 2,263 |
| JSON encode, five integers | 119 | 405 |
| binary decode | 152 | 147 |
| binary encode | 119 | 167 |

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
| serpent | **0.07** | 655 | 233 | 829 | 2,696 |
| serpent, over an index built first | 751 | 745 | 256 | 812 | 2,950 |
| on-demand parser | 179 | **270** | **110** | **315** | 1,618 |
| fast DOM A | 363 | 362 | 143 | 413 | **1,251** |
| fast DOM B | 869 | 871 | 757 | 911 | 1,906 |
| DOM lib | 7,221 | 7,233 | 3,307 | 7,365 | 13,244 |

serpent reads in place and builds nothing, so what it does not look at costs nothing and it never
allocates; walking a whole document it is 2-4x behind parsers that index or build one first.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types | level on JSON decode and binary decode, ahead on binary encode, 1.4x behind on JSON encode |
| traverse whole documents repeatedly | an indexing parser is 2-4x quicker |
| want a mutable document object | [`serpent::value`](value.md), a sixteen-byte node and one allocation per container |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
