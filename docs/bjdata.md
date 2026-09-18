# BJData

[BJData](https://github.com/NeuroJSON/bjdata) is little-endian and self-delimiting, which is
what makes reading it in place possible.

## Size or speed

A document is either to be as small as it can be, or as quick to write and read as it can be.
That is the one thing to choose, and everything the writer decides follows from it:

```cpp
bjdata::encode(value);                        // prefer::size, the default
bjdata::encode<bjdata::prefer::speed>(value);
```

| | `prefer::size` | `prefer::speed` |
|---|---|---|
| a number on its own | the narrowest marker that holds it exactly: `200` is `U`, `1.0` is a three-byte `h` | the marker of its own type: an `int` is `l`, a `double` is `D` |
| a range of numbers | a typed array, `[$T#n`, at the numbers' own type | the same |
| any other range | unbounded, `[ … ]`: a count costs bytes | counted, `[#n`: the reader sizes its container once |

A range of numbers is a typed array whichever is preferred, at the width the numbers already
have. Narrowing is for a value on its own, or in an array that is mixed anyway: narrowing a run
of numbers would mean measuring it and then storing each element, where this is one copy to
write, one to read, and [a span to view](#zero-copy-concretely). A `std::vector<std::int32_t>`
of small values you want small is a `std::vector<std::uint8_t>`.

Either is read by the same reader, which takes whatever marker it finds. It is a template
argument, so a build carries the code of the one it uses.

## Zero copy, concretely

```cpp
// [$u#U3 - three little-endian uint16s
if (auto samples = document["adc"].as<nonstd::unaligned_little_span<const std::uint16_t>>()) {
    auto peak = std::ranges::max(*samples);                              // no copy
    auto owned = std::ranges::to<std::vector<std::uint16_t>>(*samples);  // opt in to one
}
```

`as<nonstd::unaligned_little_span<const T>>()` requires the element marker to be exactly what `T` packs as, because zero copy
demands an exact layout match. Decoding into a `std::vector<T>` of that type is one copy.

## N-dimensional arrays

A dimension-array count becomes a strided view, so peeling a dimension is arithmetic, and the
column-major form is a different set of strides over the same bytes.

```cpp
#include <serpent/bjdata/ndarray.hpp>

if (auto grid = bjdata::as_ndarray(document["image"])) {
    grid->shape();                              // {480, 640}
    grid->at(12).at(34).value().as<int>();
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
    is what keeps every typed container O(1) to index and skip. Some libraries write arrays of
    strings as `[$S#…`, so reading its output needs a leniency mode this does not yet have.

Draft 4 Structure-of-Arrays is not implemented in either direction.
