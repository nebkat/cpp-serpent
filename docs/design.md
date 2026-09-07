# Design

## No intermediate representation

The point is what it does **not** do. Nothing is inflated from bytes into an object graph and
then converted into your classes. You get forward iterators into the bytes you already have,
and decoders that go straight from those into your types.

A DOM-style value object you can build freely is a layer that could sit *on top* of
this. It is not what this is built out of.

## Three layers

| Namespace | Knows about | Holds |
|---|---|---|
| `serpent` | no format | `kind`, `errc`, sinks, the emitter, the customization layer |
| `serpent::json` | JSON | scanner, `reader`, `writer` |
| `serpent::bjdata` | BJData | markers, `view`, `writer`, N-D, block notation |

`serpent` depends on neither format. Each format depends only on `serpent`. One header,
`serpent/bjdata/json.hpp`, knows both — it renders a BJData document as JSON.

## Customizations are dumb, writers are smart

A customization names fields and hands over values. Choosing markers, measuring whether a
packed array beats a generic one, and taking the memcpy path are the writer's business.

```cpp
friend void to_json(auto &out, const samples &value) {
    const auto scope = out.object();
    scope.member("readings", value.readings);   // that is all
}
```

From that line BJData may write a packed `[$u#` array copied in one go, or a generic one when
that is smaller; JSON writes plain numbers. A format added later needs no change here.

Anything a customization had to *ask* the writer about would be a capability that belonged in
`value()` and was not there yet.

## Templated, not erased

`to_json` and `from_json` are templated on the writer and reader, so a format added later
needs no new overload and the writer keeps its concrete API. The cost is one instantiation per
type per format actually used:

| | per type |
|---|---|
| one format | 1,234 B |
| both formats | 2,376 B |

Only types actually used with both pay it. Erasing the writer would remove that cost, at the
price of the concrete API that the zero-copy paths are built on.

## Why `view` is not called `reader`

`bjdata::view` never advances. Copy it, index it, iterate it — it still refers to the same
value. That is `span`/`string_view` semantics.

`json::reader` cannot be a view because JSON values must be constructed. The distinction is
the format's, and naming them the same would hide it exactly where it matters.

## Correctness

Checked against a reference implementation rather than against hand-written vectors. For each
of 44 documents, the reference implementation encodes it and decodes
those bytes back, and then:

1. the block notation matches, token for token;
2. the view decodes to the same values;
3. **re-encoding those values reproduces the reference bytes exactly**;
4. splicing reproduces the document byte for byte;
5. the JSON output matches the reference implementation's own JSON;
6. parsing the reference implementation's JSON gives the same values again.

Every suite also truncates its documents at each byte offset and requires a clean rejection,
under AddressSanitizer and UndefinedBehaviorSanitizer. Every header compiles on its own.
