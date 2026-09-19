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
| serpent | **238** | **428** | **120** | **144** |
| serpent, bounded |  | 488 |  |  |
| serpent, BJData for size |  |  | 197 | 228 |
| struct-mapping lib | 283 | 581 | 154 | 160 |
| the same, bounded |  | 712 |  |  |
| the same, CBOR |  |  | 229 | 198 |
| the same, built for size | 347 | 865 |  |  |
| DOM lib | 4,428 | 7,617 | 3,670 | 5,803 |

Decoding, serpent allocates once per string and once for the container; the struct-mapping
library 10,015 times and the DOM library 70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | **0.73** | 1.33 | **0.50** | 1.03 | **0.85** |
| JSON decode | **0.86** | **0.78** | **0.83** | **0.77** | **0.83** |
| binary encode | 1.02 | **0.93** | **0.97** | 1.02 | 1.05 |
| binary decode | **0.80** | **0.94** | **0.99** | **0.93** | 1.20 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 428 | 689 |
| JSON encode | 238 | 1,205 |
| JSON encode, five reals | 364 | 2,242 |
| JSON encode, five integers | 119 | 404 |
| binary decode | 144 | 144 |
| binary encode | 120 | 166 |

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
| serpent | **0.07** | 704 | 245 | 821 | 2,684 |
| serpent, over an index built first | 798 | 789 | 283 | 859 | 2,997 |
| on-demand parser | 179 | **272** | **109** | **313** | 1,614 |
| fast DOM A | 370 | 369 | 145 | 418 | **1,297** |
| fast DOM B | 872 | 873 | 754 | 918 | 2,000 |
| DOM lib | 7,266 | 7,292 | 3,326 | 7,378 | 13,379 |

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
