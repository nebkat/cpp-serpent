# MessagePack

[MessagePack](https://github.com/msgpack/msgpack/blob/master/spec.md) is big-endian, and every
container states its count before its first element. Nothing in it is proprietary: the one
thing beyond JSON's shapes it has, the extension type, is a type number and some bytes that an
application gives a meaning to.

```cpp
#include <serpent/msgpack.hpp>

const auto bytes = serpent::msgpack::encode(frame);
const auto again = serpent::msgpack::decode<can_frame>(bytes);
```

The same type definitions serve it as serve JSON and BJData, and `write`, `encode`, `decode`,
`try_decode`, `measure`, `validate` and `write_value` are the calls BJData offers.

## Counts come first

A map's count is its first byte, and `object()` opens one before anyone knows how many members
it will get. So a container opened without a count is gathered: from the outermost one so
opened, everything is kept aside with a gap for each header, and when that one closes every
count is known and the lot goes to the sink with each header at its narrowest. That allocates.

What knows its count is written straight through instead, with nothing gathered:

| | |
|---|---|
| a described type | counted from its members: those not skipped, and a discriminant |
| a range that can be measured without being used up | counted |
| `out.array(n)`, `out.object(n)` | counted by you |
| `json_convert`, `to_json`, a map, a tagged variant | gathered |

A counted container closed with more or fewer elements than it promised fails the writer with
`errc::unbalanced_container`, rather than send a document that says one thing and holds another.

## What the writer writes

| | |
|---|---|
| an integer | at the narrowest width that holds it; anything not negative as unsigned |
| a `float` | float 32 |
| a `double` | float 64, never narrowed |
| text | str, at the narrowest length form |
| a range of `std::byte` | bin |
| `out.extension(type, bytes)` | fixext where the length is one, two, four, eight or sixteen, else ext |
| `out.encoded(bytes)` | MessagePack already encoded, as it stands |

## What the reader reads

The reader is BJData's, with the same accessors and the same refusal to be copied, and it lends:
a str comes back as a `std::string_view` and a bin as a `std::span<const std::byte>`, both into
the document.

| on the wire | reads as |
|---|---|
| bin | an array of bytes, as BJData's `[$B` does: `as<std::span<const std::byte>>()` views it whole, `array()` walks it a byte at a time |
| ext | no kind: `kind::invalid`, `is_extension()`, and `extension()` for its type and bytes. Stepped over wherever it stands, and never refused by `validate()` |
| a map with a key that is not a str | valid, and `validate()` passes it; `items()` stops at the first such key, since it has no name to give it |
