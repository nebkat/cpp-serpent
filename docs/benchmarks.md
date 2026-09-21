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

## Building a document object

`serpent::value` built from JSON text, against the two other DOM libraries in this suite, on the
usual corpora — [json-bench](https://github.com/nebkat/json-bench)'s `data/json`. Allocations are
counted by replacing `operator new`; times are the best of five, all three libraries in one
process on a machine under other load (the absolutes would be lower on a quiet one).

| | serpent::value | struct-mapping lib's generic tree | DOM lib |
|---|---:|---:|---:|
| twitter (616 KB) | **8,040** allocs · **726** KB · **771** µs | 10,090 · 2,921 KB · 837 µs | 27,323 · 1,574 KB · 3,249 µs |
| canada (2.2 MB) | **56,086** · **3,578** KB · **3,148** µs | 114,198 · 12,937 KB · 3,871 µs | 170,280 · 7,001 KB · 12,808 µs |
| citm_catalog (1.7 MB) | **14,101** · **1,154** KB · **1,564** µs | 19,906 · 4,856 KB · 1,625 µs | 56,023 · 3,312 KB · 7,083 µs |
| gsoc-2018 (3.2 MB) | **15,891** · **3,510** KB · 1,562 µs | 21,090 · 5,723 KB · 1,679 µs | 50,020 · 4,956 KB · 16,125 µs |
| github_events (63 KB) | **858** · **85** KB · 75 µs | 1,080 · 207 KB · 74 µs | 2,773 · 194 KB · 374 µs |

Quicker than the struct-mapping library's tree on three corpora of five, by up to a fifth, and
level on the other two — for a quarter to a third of the DOM library's allocations and half to
four-fifths of the other tree's, in less memory than either on every corpus. The two times shown
without a winner are inside run-to-run variation: repeated, gsoc-2018 ranges from 8% quicker to
level and github_events lands on either side of parity, so neither is read as a win. Arena
parsers that build no tree of their own (the fast DOMs below) remain 2–4x ahead on building,
which is what an arena buys.

### What the smaller node cost, and where it went

Shrinking the node to sixteen bytes and giving each container one allocation bought the
allocation and memory figures above, and at first it cost 16–22% of build time. The cost was not
the node's size but its destructor: a forty-byte node held a variant whose destructor inlined
away for a scalar, where the new node called `release()` out of line for every value the builder
assigned or destroyed — enough to make it the top symbol in a profile of building a tree. Only
four of the fourteen shapes own anything and they are contiguous in the tag nibble, so that test
is a subtract and a compare, settled inline, and only an owning node calls out to free. Measured
against the tree this library had before (forty bytes, a vector per container, a string per
name), same binaries alternating, best of three:

| | previous tree | sixteen-byte node | |
|---|---:|---:|---:|
| twitter | 702 µs | 698 µs | −1% |
| canada | 3,326 | 3,071 | **−8%** |
| citm_catalog | 1,482 | 1,461 | −1% |
| gsoc-2018 | 1,391 | 1,488 | +7% |
| github_events | 67 | 70 | +4% |

From 8% quicker to roughly a tenth slower, depending on the corpus and on the run — the two slower corpora move by a few points between repeats — for a third to a half of the memory.

How much of that remains depends on what else is in the translation unit. Measured on its own,
in a small program that builds nothing but a tree, twitter is 8–10% slower on the new node
rather than 1% — the same inline-budget effect [described above](#it-was-never-the-format),
reversed: the old node's scalar destructor is one of the small functions that stops being
inlined when a budget is tight, so the two converge under pressure and diverge without it. The
table above is measured beside the other two libraries, which is the comparison the rest of this
page makes.

What is left on the slower corpora is movement rather than freeing: against the previous tree at
the same head, `memmove` samples rose 216 against 141 and `free` 140 against 92, while string
scanning, reading and number parsing are level. That is the builder staging each value in a
per-depth scratch vector and then moving it into the run — every value moved twice — and
destroying a scratch vector of pairs per object. Building straight into a run reserved after a
count pass, or keeping the scratch as `run<>` blocks that become the tree by pointer swap rather
than by move, would take both out.

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
| want a mutable document object | [`serpent::value`](value.md): a sixteen-byte node and one allocation per container - quicker than the other tree on three corpora of five and level on two, in a quarter to a third of the DOM library's allocations and less memory than either |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
