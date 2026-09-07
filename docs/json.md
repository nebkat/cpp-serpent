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

Numbers are rendered the way the reference implementation renders them, which is what lets the
test suite compare our JSON to dart-bjdata's own JSON byte for byte.
