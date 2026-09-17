# JSON

Both directions, from the same customizations.

```cpp
#include <serpent/json.hpp>

auto text = json::encode(config);
auto pretty = json::encode(config, { .indent = 2 });
auto value = json::decode<config>(text);
json::validate(text);
```

## Rendering a stored document

```cpp
#include <serpent/bjdata/json.hpp>

auto text = json::encode(bjdata::view::over(bytes));
```

This is the one header that knows both formats. It is also how a type using `to_json`/
`from_json` for a writer it does not have reaches JSON: encode, then transcribe.

## Reading

`json::reader` is a reader, not a view — see [Reading](reading.md#strings) for what that means
for strings.

Numbers are validated against JSON's grammar rather than left to `std::from_chars`, which is
more permissive: it would accept `inf`, `nan`, `.5` and `5.`, and read `01` as `1`. `1e400`
parses and then fails to convert, rather than quietly becoming infinity.

Escapes are checked properly, surrogate pairs included.

## Three things JSON cannot represent

| | Written as |
|---|---|
| Binary | an array of integers — `[222,173,190,239]` |
| NaN, ±infinity | `null`, so the output is always valid JSON |
| N-D arrays | nested, not flattened — a 2×3 becomes `[[1,2,3],[4,5,6]]` |

## Formatting

A real is written with the shortest digits that read back as the same double — in full from a
ten-thousandth up to 1e16, and with an exponent beyond:

| value | written as |
|---|---|
| `-39.9` | `-39.9` |
| `40.0` | `40.0` |
| `1e15` | `1000000000000000.0` |
| `1e16` | `1e+16` |
| `0.00001` | `1e-05` |

!!! note "A whole number keeps its `.0`"

    JSON has one kind of number and does not say whether `40` is an integer, so whatever reads
    it has to guess from the text. A real written as `40` comes back as an integer anywhere the
    type is inferred rather than known — a `std::variant`, a [tree](value.md), a document
    transcribed into BJData. The point costs two characters and keeps a real a real.

The digits are found and written by a bundled copy of [Żmij](https://github.com/vitaut/zmij),
in about a third of the time `std::to_chars` takes. It is the one part of the library that is
not a header, so CMake builds it into a small library that `serpent` links;
`-DSERPENT_USE_ZMIJ=OFF` goes without it and leaves the library header-only. **The text is the
same either way**, character for character — only the time taken differs.

Everything else — key order, escaping, indentation, integers — is rendered the way the
reference implementation renders it, which lets the test suite compare our JSON with its own
byte for byte. Reals agree with it too between a ten-thousandth and 1e16; outside that it
spells every digit out where this library writes an exponent, which is the same value in
different words.
