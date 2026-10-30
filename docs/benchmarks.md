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
| serpent | **254** | **397** | **114** | **146** |
| serpent, bounded |  | 441 |  |  |
| serpent, BJData for size |  |  | 171 | 226 |
| struct-mapping lib | 277 | 461 | 149 | 150 |
| the same, bounded |  | 559 |  |  |
| the same, CBOR |  |  | 228 | 187 |
| the same, built for size | 330 | 830 |  |  |
| DOM lib | 4,086 | 7,202 | 3,346 | 5,494 |

Decoding, serpent allocates 15 times, the struct-mapping library 15 and the DOM library
70,029.

The same, one kind of member at a time and a record of numbers in bulk, as a ratio to the
struct-mapping library's normal build - below 1.0 is serpent ahead:

| | strings | booleans | integers | reals | 100 × 2000 numbers |
|---|---:|---:|---:|---:|---:|
| JSON encode | **0.53** | 1.36 | **0.51** | 1.04 | **0.82** |
| JSON decode | **0.98** | **0.82** | **0.84** | **0.76** | **0.83** |
| binary encode | 1.12 | **0.93** | **0.96** | **0.99** | 1.05 |
| binary decode | **0.85** | 1.02 | 1.12 | 1.09 | 1.23 |

A described type is what all of this is for. The same record behind a hand-written
`json_convert` takes 261 to encode to BJData and 1,186 to decode.

## What the switches are worth

Every optimisation with a plainer alternative is behind a switch, [listed
here](configuration.md), and the benchmark is built once per configuration. `plain` is every
switch off:

| | default | plain |
|---|---:|---:|
| JSON decode | 397 | 608 |
| JSON encode | 254 | 1,143 |
| JSON encode, five reals | 353 | 2,164 |
| JSON encode, five integers | 115 | 356 |
| binary decode | 146 | 151 |
| binary encode | 114 | 210 |

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

The whole document as a tree its user can hold and change: `serpent::value` against the other
two libraries' trees, on five corpora from [simdjson's examples](https://github.com/simdjson/simdjson-data).
The struct-mapping library's generic tree is shown in its default form, which holds every number
as a double - twitter's 64-bit ids do not survive it - and in the form that keeps a 64-bit
integer whole, as `serpent::value` does. Each is timed building the tree and freeing it, the best
of ten runs; allocations, and the kilobytes they ask for in all, are counted building it once.

From JSON text:

| | serpent::value | struct-mapping lib's generic tree | the same, 64-bit integers | DOM lib |
|---|---:|---:|---:|---:|
| twitter (616 KB) | **8,166** allocs · **808** KB · **532** µs | 9,481 · 2,903 KB · 796 µs | 9,481 · 2,903 KB · 1,054 µs | 27,323 · 1,575 KB · 3,175 µs |
| canada (2.2 MB) | **56,070** allocs · **3,563** KB · **3,118** µs | 114,197 · 12,937 KB · 3,951 µs | 114,197 · 12,937 KB · 6,684 µs | 170,280 · 7,001 KB · 12,447 µs |
| citm_catalog (1.7 MB) | **14,074** allocs · **1,159** KB · **1,224** µs | 19,634 · 4,848 KB · 1,517 µs | 19,634 · 4,848 KB · 2,223 µs | 56,023 · 3,313 KB · 6,889 µs |
| gsoc-2018 (3.2 MB) | **15,881** allocs · **3,510** KB · **1,201** µs | 18,425 · 5,643 KB · 1,593 µs | 18,425 · 5,643 KB · 2,036 µs | 50,020 · 4,957 KB · 15,605 µs |
| github_events (63 KB) | **844** allocs · **89** KB · **47** µs | 971 · 204 KB · 67 µs | 971 · 204 KB · 89 µs | 2,773 · 194 KB · 350 µs |

From each library's own binary - BJData written for speed, BEVE, CBOR:

| | serpent::value from BJData | generic tree from BEVE | the same, 64-bit integers | DOM lib from CBOR |
|---|---:|---:|---:|---:|
| twitter | **8,164** allocs · **807** KB · **476** µs | 9,309 · 2,874 KB · 742 µs | 9,309 · 2,874 KB · 829 µs | 60,288 · 3,577 KB · 2,243 µs |
| canada | 56,070 allocs · **3,563** KB · 2,590 µs | **56,050** · 6,531 KB · **2,471** µs | **56,050** · 6,531 KB · 3,400 µs | 282,394 · 11,440 KB · 6,756 µs |
| citm_catalog | **14,073** allocs · **1,159** KB · **915** µs | 14,740 · 4,142 KB · 1,082 µs | 14,740 · 4,142 KB · 1,159 µs | 108,764 · 6,401 KB · 3,191 µs |
| gsoc-2018 | **15,879** allocs · **3,504** KB · **823** µs | 18,403 · 5,574 KB · 1,237 µs | 18,403 · 5,574 KB · 1,322 µs | 131,910 · 18,003 KB · 8,287 µs |
| github_events | **842** allocs · **85** KB · **42** µs | 963 · 199 KB · 64 µs | 963 · 199 KB · 71 µs | 6,751 · 417 KB · 238 µs |

From JSON, `serpent::value` is the quickest of the four on every corpus: 0.67–0.81x the time of
the struct-mapping library's tree in its default form and 0.47–0.59x in the form that keeps
integers whole, in a quarter to a third of the DOM library's allocations, and in less memory than
either - a quarter to three-fifths of the other tree's. Arena parsers that build no tree of their
own (the fast DOMs below) remain ahead on building, which is what an arena buys.

From binary it is quicker again than from its own JSON, by a tenth to a third, and ahead of the
generic tree from BEVE on four corpora of five, at 0.64–0.85x its time. The fifth is canada,
which is arrays of numbers: BEVE holds each as a typed array that the generic tree reads in one
step, and comes out 4% ahead in as many allocations, though in nearly twice the memory.

### What the smaller node cost, and where it went

Shrinking the node to sixteen bytes and giving each container one allocation bought the
allocation and memory figures above, and at first it cost 16–22% of build time. The cost was not
the node's size but its destructor: a forty-byte node held a variant whose destructor inlined
away for a scalar, where the new node called `release()` out of line for every value the builder
assigned or destroyed — enough to make it the top symbol in a profile of building a tree. Only
four of the ten shapes own anything and they are contiguous in the tag nibble, so that test
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
| serpent | **0.07** | 680 | 222 | 893 | 2,860 |
| serpent, over an index built first | 766 | 763 | 239 | 838 | 2,663 |
| on-demand parser | 172 | **258** | **105** | **298** | 1,547 |
| fast DOM A | 352 | 352 | 137 | 397 | **1,206** |
| fast DOM B | 834 | 833 | 704 | 875 | 1,862 |
| DOM lib | 6,866 | 6,891 | 3,190 | 6,996 | 12,546 |

serpent reads in place and builds nothing, so what it does not look at costs nothing and it never
allocates; walking a whole document it is 2-4x behind parsers that index or build one first.

## What this means

| If you | then |
|---|---|
| pull a few fields out of a large payload | serpent, by orders of magnitude |
| convert your own types | serpent, ahead on all four operations: 0.77–0.97x the struct-mapping library's time |
| traverse whole documents repeatedly | an indexing parser is 2-4x quicker |
| want a mutable document object | [`serpent::value`](value.md): a sixteen-byte node and one allocation per container - quicker than the other tree from JSON on every corpus, and from binary on four of five, in a quarter to a third of the DOM library's allocations and less memory than either |
| need BJData and JSON from one definition, or cannot allocate while reading | serpent |
