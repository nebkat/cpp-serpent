# BJData

[BJData](https://github.com/NeuroJSON/bjdata) is little-endian and self-delimiting, which is
what makes reading it in place possible.

## Zero copy, concretely

```cpp
// [$u#U3 — three little-endian uint16s
if (auto samples = document["adc"].as_span<std::uint16_t>()) {
    auto peak = std::ranges::max(*samples);                              // no copy
    auto owned = std::ranges::to<std::vector<std::uint16_t>>(*samples);  // opt in to one
}
```

`as_span<T>()` requires the element marker to be exactly what `T` packs as, because zero copy
demands an exact layout match.

## What the writer emits

Output is byte-identical to [dart-bjdata](https://github.com/nebkat/dart-bjdata):

- Integer markers take the narrowest width that holds the value, preferring unsigned, so
  `200` is `U` and never `i`.
- Floats narrow where the round trip is exact, so `1.0` is three bytes and ±inf and NaN narrow
  to `float16`.
- Plain lists and maps are unbounded. A count only ever follows a `$type`.
- A uniform numeric list becomes `[$T#n` only when that is **strictly** smaller.

## Compaction is compile-time

```cpp
bjdata::encode(value);                        // everything the reference encoder does
bjdata::encode<bjdata::no_compaction>(value); // declared widths, and none of that code emitted
```

A build that turns one off does not carry its code — about 1 KB of `__text` for a three-type
translation unit.

## Paying size for a copy

A contiguous range packed at the element's own width is already in wire order and goes out in
**one copy**. Narrowed or generic, it costs a store per element.

```cpp
constexpr bjdata::writer_options slack { .copy_tolerance_percent = 5 };
bjdata::encode<slack>(readings);
```

| 1,000 doubles | tolerance 0 | with the copy |
|---|---|---|
| real values | 7,802 B generic | 8,007 B — **+2.6%** |
| all float16-exact | 2,007 B as `h` | 8,007 B — +299% |

Zero is the default and reproduces the reference exactly.

## N-dimensional arrays

A dimension-array count becomes a strided view, so peeling a dimension is arithmetic, and the
column-major form is a different set of strides over the same bytes.

```cpp
#include <serpent/bjdata/ndarray.hpp>

if (auto grid = bjdata::as_ndarray(document["image"])) {
    grid->shape();                              // {480, 640}
    grid->at(12).at(34).value().as_int<int>();
    grid->at(12).flat<std::uint8_t>();          // a whole row, when contiguous
}
```

## Splicing

`view::payload_bytes()` gives a value's own bytes, so a subdocument moves with no re-encoding:

```cpp
bjdata::write_value(writer, document["calibration"]);   // marker, then one memcpy
```

## Block notation

```cpp
#include <serpent/bjdata/notation.hpp>

bjdata::block_notation(bytes);   // [S][U][5][hello]
```

A trace of the *bytes*, not the decoded value, so it catches grammar drift that comparing
values would miss.

## Scope

Draft 3 in full: every marker, all container forms, dimension-array counts, and the noop
rules. `E` is rejected rather than skipped.

!!! warning "Strict grammar"

    A strong type must be fixed width, so `$S`, `$H`, `$Z`, `$T` and `$F` are rejected — which
    is what keeps every typed container O(1) to index and skip. nlohmann writes arrays of
    strings as `[$S#…`, so reading its output needs a leniency mode this does not yet have.

Draft 4 Structure-of-Arrays is not implemented in either direction.
