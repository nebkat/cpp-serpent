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

| | serpent | struct-mapping lib | the same, for size | DOM lib |
|---|---:|---:|---:|---:|
| JSON decode | **511** | 561 | 861 | 7,581 |
| JSON encode | 426 | **283** | 342 | 4,312 |
| binary decode | 235 | 175 (CBOR), **160** (its own) | | 5,761 |
| binary encode | **117** | 193 (CBOR), 153 (its own) | | 3,595 |

Binary is BJData [written for speed](bjdata.md#size-or-speed); written for size it is 161 to
encode and 297 to decode. Decoding allocates once per string and once for the container; the DOM
library allocates 70,000 times.

By kind of member, JSON, against the struct-mapping library's normal build — below 1.0 is serpent
ahead:

| | strings | booleans | integers | reals |
|---|---:|---:|---:|---:|
| decode | **0.89** | **0.75** | 1.11 | 1.13 |
| encode | 1.14 | 2.14 | **0.59** | 1.18 |

A hundred records of two thousand numbers each, which both libraries write as one typed
payload: 62 to encode against 46, and 48 to decode against 40.

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
| binary decode | 235 | 290 |
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

| | serpent | on-demand | fast DOM A | fast DOM B | DOM lib |
|---|---:|---:|---:|---:|---:|
| first key of citm_catalog.json | **0.07** | 178 | 366 | 864 | 7,333 |
| last key of the same | 695 | **272** | 366 | 861 | 7,306 |
| sum ids, twitter.json | 247 | **111** | 145 | 758 | 3,408 |
| count every value, citm_catalog.json | 1,186 | **311** | 414 | 909 | 7,472 |
| the same, with an index built first | 842 | | | | |
| sum every coordinate, canada.json | 2,882 | 1,619 | **1,253** | 1,950 | 13,301 |

serpent reads in place and builds nothing, so what it does not look at costs nothing and it
never allocates; walking a whole document it is 2-4x behind parsers that index or build one
first. `json::validate` alone takes 694 on citm.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types | level on JSON decode, ahead on binary encode, 1.5x behind on JSON encode and binary decode |
| traverse whole documents repeatedly | an indexing parser is 2-4x quicker |
| want a mutable document object | serpent has none |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
