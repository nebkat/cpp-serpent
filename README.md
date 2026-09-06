# serpent

A C++23 serialization library. Zero-copy [BJData](https://github.com/NeuroJSON/bjdata) reading
and writing, JSON reading and writing, and one definition per type that serves all four.

Named rather than described, because it is not a BJData library with JSON bolted on nor the
reverse — and because a third format would not make the name wrong. The namespaces stay
`nonstd::` regardless: the whole layout exists so these headers drop into a firmware's
`lib/common/include/nonstd/` unchanged.

`nonstd::bjdata::view` is a cursor onto bytes you already have. It allocates nothing, copies
nothing, and owns nothing: strings come back as `std::string_view` into the source buffer,
typed arrays as spans over it, and containers are walked by iterators that hold the only
traversal state there is. Parsing a document is reading its first byte.

BJData is little-endian and self-delimiting, which is what makes this possible — every
value's extent follows from its marker plus a length prefix, so a document can be traversed
in place rather than inflated into a DOM.

```cpp
#include <nonstd/bjdata.hpp>

using namespace nonstd::bjdata;

const auto document = view::over(bytes);              // no parsing yet

const auto name    = document["fv"].as_string();      // string_view into `bytes`
const auto version = document["dv"].as_int<std::uint16_t>();
const auto mac     = document["sn"].as_binary();      // span over `bytes`

for (const auto [key, value] : document["cfg"].items()) { ... }
for (const auto element : document["fn"].array())      { ... }
```

## Two error models, one parser

Reading a file off a device and reading a hand-written fixture want different things, so
both are available over the same bytes and the same single implementation.

| Tier | Members | On failure |
|---|---|---|
| **Total** | `operator[]`, `find`, `try_get<T>`, `as_bool` / `as_int` / `as_float` / `as_string` / `as_binary` / `as_span`, iteration | an invalid view, `std::nullopt`, or an empty range — never throws |
| **Checked** | `at`, `get<T>`, `string`, `binary`, `span<T>` | throws `bjdata::error`, carrying an `errc` and the byte offset |
| **Validating** | `validate(bytes)` | `std::expected<void, error>` — one walk, for the trust boundary |

```cpp
// Tolerant: a missing key, a wrong type or a corrupt document all just yield nothing.
const auto label = document["meta"]["label"].as_string().value_or("unnamed");

// Strict: tell me exactly what is wrong and where.
try {
    const auto label = document.at("meta").at("label").string();
} catch (const error &failure) {
    std::println("{} at byte {}", failure.what(), failure.offset());
}
```

The checked tier is a handful of inline wrappers over the total tier, so it costs nothing
when unused, and the library's only `#if __cpp_exceptions` is in one `raise()` helper. Under
`-fno-exceptions` the total tier is completely unaffected (there is a test for exactly this).

A view is bounds-checked against its buffer on every access, so traversing a truncated or
corrupt document is safe even if `validate()` was never called — it degrades to invalid
rather than reading past the end. Nesting is capped at `max_depth`, so a hostile document
cannot drive unbounded recursion.

## Zero copy, concretely

`as_span<T>()` requires the array's element marker to be exactly the one `T` packs as, and
then hands back the source bytes reinterpreted in place:

```cpp
// [$u#U3 with 3 little-endian uint16s
if (const auto samples = document["adc"].as_span<std::uint16_t>()) {
    const auto peak = std::ranges::max(*samples);          // no copy
    auto owned = std::ranges::to<std::vector<std::uint16_t>>(*samples);  // opt in to one
}
```

That works because of `nonstd::unaligned_ptr` (`include/nonstd/unaligned_ptr.hpp`), which is
useful on its own. There is no `T` object at an address inside a byte buffer, so it never
forms a `T*` or a `T&`: it is a proxy pointer that loads and stores by value through
`nonstd::unaligned<T, Bits, E>`.

```cpp
nonstd::unaligned_little_span<std::uint16_t> values { buffer + 1, 8 };   // misaligned, fine
std::iota(values.begin(), values.end(), 0);
std::ranges::sort(values);
```

It models `std::random_access_iterator` (C++20 concepts permit proxy references), so all of
`<algorithm>` and `<ranges>` applies. Constness is carried in `T`, mirroring `T*` and
`const T*`. Odd widths work too — `unaligned_little_ptr<std::int32_t, 24>` reads a
sign-extended 24-bit field.

## Writing

`writer` emits into a **sink**, which is anything with a member
`write(std::span<const std::byte>)`. The return type is deliberately unconstrained, so the
classes a firmware already has — ring buffers, sockets, flash record writers, OTA writers —
are sinks as they stand, whether they return `void`, `bool`, `std::size_t` or
`std::expected`.

```cpp
std::vector<std::byte> buffer;
container_sink out { buffer };
writer w { out };
{
    const auto document = w.object();
    document.member("host", "example.com");
    document.member("port", 8080);
    document.member("samples", std::vector<int> { 1, 2, 3, 4, 5 });
}
const auto written = w.finish();     // std::expected<std::size_t, error>
```

Shipped sinks: `container_sink` (a growable container), `span_sink` (a fixed buffer that
latches overflow — no heap), `counting_sink` (sizes a document without producing it),
`iterator_sink` (any output iterator), and `ostream_sink` (its own header, so `<ostream>`
stays out of embedded translation units). A bare lambda works too — `writer w { callback };`.

Failures **latch**: a full buffer, an unbalanced container or a key written outside an object
records the first error and makes everything after it a no-op, so you check once at the end
rather than after every field. Convenience wrappers: `to_bytes(value)`, `from_bytes<T>(bytes)`
and `measure(value)`, the last of which computes an exact byte length with no allocation.

The sink is type-erased inside `writer` — a function pointer and a context pointer — so
`writer` is one class rather than a template. That is what lets a user's customization be a
plain function, and it means one instantiation instead of one per sink.

### What it emits

Output is **byte-identical to dart-bjdata's** by default. That means:

- Integer markers take the narrowest width that holds the value, preferring unsigned, so
  `200` is `U` and never `i`.
- Floats narrow wherever the round trip is exact, so `1.0` is three bytes (`[h]`), and ±inf
  and NaN narrow to `float16` too.
- Plain lists and maps are **unbounded** (`[…]`, `{…}`). A count only ever follows a `$type`
  — the counted-but-untyped `[#n` form is never written.
- A uniform numeric list becomes `[$T#n` only when that is *strictly* smaller. A generic
  array stores each value at its own width; a typed one pays the widest throughout, so both
  are measured. A tie keeps the generic form. In practice a uniform-width list packs at five
  elements, and one large value that forces a wider marker can keep a list generic.

`writer_options { compact_types, numeric_packing }` turns either heuristic off.

`typed_array<T>(span)` is the write counterpart to `as_span<T>()`: header, then the payload
in a single copy.

## Custom types

A type opts in **once** and gets both directions. There are three ways, and they compose.

**One function, both directions** — for the symmetric majority:

```cpp
struct segment {
    point start, end;
    std::string label;

    friend void serial_convert(auto &visitor, conversion_object_t<decltype(visitor), segment> value) {
        visitor.member("start", value.start);
        visitor.member("end",   value.end);
        visitor.member("label", value.label);
    }
};
```

`visitor` is a `write_visitor` or a `read_visitor`, and `conversion_object_t` resolves to
`const segment &` or `segment &` to match. Reading keeps a cursor into the object and only
falls back to a scan when a key is not where it was expected, so a document written in
declaration order costs one pass rather than one scan per field. An absent key leaves the
member at its existing value; an unknown key is ignored.

A container is opened only by a scope, which closes it in its destructor; there is no
`begin_object`/`end_object` to forget. Scopes are movable, so one can be held in a
`std::optional` to open in one place and close in another, and a moved-from scope closes
nothing. If a container is still open when `finish()` is called, that is an error rather
than a truncated document.

**A macro**, when the body would just be a list of members:

```cpp
struct point { int x = 0; int y = 0; NONSTD_SERIAL_DEFINE_TYPE(point, x, y) };
```

`NONSTD_SERIAL_DEFINE_TYPE_NON_INTRUSIVE(type, ...)` does the same from outside the type. Field
names cannot be recovered without reflection, so a macro is the only option; the shape
deliberately matches nlohmann's `..._DEFINE_TYPE` family.

**Two functions**, when the directions genuinely differ — defaults on read, keys omitted on
write, a value that is a string one way and a bool the other:

```cpp
friend void serial_write(bjdata::writer &out, const connection &value);
friend bool serial_read(bjdata::view source, connection &value);
```

These are resolved by ADL against whichever writer or reader is passed, so the escape hatch
costs no format-neutrality: overload them per format when the bodies differ, or declare one
`auto &` template when they don't.

```cpp
friend void serial_write(json::writer &out, const connection &value);   // and JSON too
```

Note the asymmetry in how they are passed: the writer is a mutable reference because writing
accumulates, while the reader goes **by value** because it is a small trivially copyable
handle that is never mutated — the same convention as `std::string_view` and `std::span`.

For a type you cannot add functions to, specialize `serializer<T>`.

**One definition covers all four paths.** `serial_convert` names neither reader nor writer, so
a type using it — which is what `NONSTD_SERIAL_DEFINE_TYPE` writes — is read and written in
both formats without being told about any of them. Nothing in the customization layer names a
format; the only names that do are the ones you write yourself, in the overloads you choose.

## Reflection (C++26)

`reflect.hpp` implements a fourth form in which the compiler enumerates the fields, so no
list is written at all:

```cpp
struct [[=serial::serializable]] [[=serial::naming{serial::naming_style::snake_case}]] ethernet_config {
    [[=serial::key("ip")]] std::string ip_address;
    [[=serial::skip]]      int cache_generation;
                           ip_mode mode;          // key becomes "mode"
};
```

`[[=serial::key(...)]]` overrides one key, `[[=serial::naming{...}]]` derives all of them from the
identifiers (`snake_case`, `camel_case`, `pascal_case`, `kebab_case`,
`screaming_snake_case`), and `[[=serial::skip]]` leaves a field out. Annotations are ordinary
values, not parsed strings, and are always written qualified. It is **opt-in** — via
`[[=serial::serializable]]` or by specializing
`enable_reflection<T>` — because reflecting every aggregate that merely lacks a
`serial_convert` would turn any struct that happens to be serializable into a wire-format
commitment, silently.

It generates exactly the `visitor.member(key, value.field)` calls the macro does, so nothing
else in the design changes — and it is the *names*, not the speed, that reflection provides:
the visitor inlines away entirely either way.

> **Availability.** This needs three C++26 papers — P2996 (reflection), P1306 (`template for`)
> and P3394 (annotations) — and is compiled only when `__cpp_reflection` and
> `__cpp_impl_reflection_annotations` are both present. No compiler available today has them,
> so **that binding is unverified**; expect to adjust spellings when one ships. Check
> `reflection_available` at compile time.
>
> The parts that do not need reflection — the annotation types and the whole
> identifier-to-key case conversion — are deliberately outside the gate and are compiled and
> unit-tested in every build, so the unproven surface is only the binding to `std::meta`.
> Acronyms are a known limitation: `IPAddress` converts to `ipaddress`, not `ip_address`.

## JSON

Both directions, from the same customizations.

```cpp
#include <nonstd/bjdata/json.hpp>

std::string text = to_json(config);                        // {"host":"example.com","port":8080}
std::string pretty = to_json(document, { .indent = 2 });   // two-space indented
write_json(sink, view::over(bytes));                       // transcribe a stored .bjd
```

There are two routes in, and the split falls out of the customization design rather than
being designed for. `serial_convert` and `NONSTD_SERIAL_DEFINE_TYPE` take `auto &visitor` and never
name the BJData writer, so **those types serialise straight to JSON with no intermediate at
all**. A type using the `to_bjdata(writer &, …)` form names the writer, so it reaches JSON by
being written as a document first and transcribed — correct, but it allocates.

`write_json(sink, view)` transcribes a document that already exists, which is what you want
for dumping a stored file, and it is what covers the `to_bjdata` form.

### Reading

```cpp
#include <nonstd/bjdata/json_reader.hpp>

auto config = from_json<ethernet_config>(text);       // the same type that reads BJData
auto value  = json_reader::over(text);                // or walk it directly
validate_json(text);                                  // std::expected<void, error>
```

**It is a reader, not a view — and that distinction is the format's, not a naming choice.**
BJData values *are* the bytes, so `view` can hand out `string_view`s into the buffer. JSON
values must be constructed: `"a\nb"` is six characters of source and three of value. So
strings are **always decoded**, never handed back as a borrow that happens to work when the
data contains no escapes — an API whose shape depends on its contents is one that passes
testing and fails in the field.

| | |
|---|---|
| `as_string()` | always decodes, returns `std::string` |
| `decode_string_into(span<char>)` | decodes into caller storage; refuses to truncate |
| `string_is("...")` | compares without materialising anything, escapes included |

Numbers are validated against JSON's grammar rather than left to `std::from_chars`, which is
more permissive: it would accept `inf`, `nan`, `.5` and `5.`, and read `01` as `1`. `1e400`
parses as a real and then fails to convert, rather than quietly becoming infinity.

Everything else carries over — the same `errc`, the same three accessor tiers, forward
iterators holding all the traversal state, and no allocation except where a decoded string is
asked for.

Three things JSON cannot represent, and what happens:

| | |
|---|---|
| Binary (`[$B#`) | an array of integers — `[222,173,190,239]` |
| NaN, ±infinity | `null`, so the output is always valid JSON |
| N-D arrays | nested, not flattened — a 2×3 becomes `[[1,2,3],[4,5,6]]` |

> Note: `tools/python/util.py:201-231` in sunrise-firmware renders binary as **hex chunks**
> rather than integers, so the firmware and that Python tooling will print the same document
> differently. Worth reconciling if both ever feed the same consumer.

Numbers are rendered exactly as the reference implementation renders them, which is what lets
the test suite compare our JSON to dart-bjdata's own JSON byte for byte.

## Splicing

`view::payload_bytes()` returns a value's own bytes, so a subdocument can be copied into
another document with no re-encoding at all:

```cpp
write_value(out, document["calibration"]);   // marker, then one memcpy
```

## N-dimensional arrays

A dimension-array count (`#[Nx Ny]`) becomes a strided view, so peeling a dimension is
arithmetic rather than a copy — and the column-major form (`#[[Nx Ny]]`) is just a different
set of strides over the same bytes, where a DOM reader has to permute them into a new
allocation.

```cpp
#include <nonstd/bjdata/ndarray.hpp>

if (const auto grid = as_ndarray(document["image"])) {
    grid->shape();                              // {480, 640}
    grid->at(12).at(34).value().as_int<int>();  // one row, one pixel
    grid->at(12).flat<std::uint8_t>();          // a whole row as a span, when contiguous
}
```

## Block notation

`block_notation()` (`include/nonstd/bjdata/notation.hpp`) renders a document as
dart-bjdata's token trace — `[S][U][5][hello]` — matching its output byte for byte,
including Dart's `double.toString()` formatting. It is a trace of the *bytes*, not of the
decoded value, so it catches grammar drift that comparing decoded values would miss.

## Scope

Reads and writes **draft 3**: every marker (`Z T F N U i u I m l M L h d D C B S H`),
unbounded, counted and typed containers, dimension-array counts in all three forms and both
orderings, and the noop rules (skipped in untyped arrays and in *all* objects, but not in
`$`-typed arrays). `E` is explicitly rejected rather than skipped. `h` is decoded by bit
manipulation rather than `_Float16`, which the xtensa backend cannot be relied on for.

JSON is read and written in full, including surrogate-pair escapes.

**Not implemented:** draft 4 Structure-of-Arrays, in either direction. The grammar is
recorded in the plan and the design is unaffected — SoA adds container kinds, it does not
reshape the view.

**Asymmetric:** the reader parses N-D arrays; the writer does not emit them. This does not
affect byte parity, because the reference only forms an N-D candidate when the innermost
elements are typed data, which JSON decoding never produces.

## Grammar: strict dart-bjdata parity

The reference is [dart-bjdata](https://github.com/nebkat/dart-bjdata), and this reader
accepts exactly what it does. In particular a strong type must be fixed width, so `$S`,
`$H`, `$Z`, `$T` and `$F` are rejected — which is what keeps every typed container O(1) to
index and to skip.

> **Note for a future firmware port.** nlohmann's `ubjson_prefix` returns `'Z'` for null,
> `'T'`/`'F'` for booleans and `'S'` for strings, and sunrise-firmware's `app::fs::store_json`
> writes with `use_type = true` — so a firmware-written array of strings is `[$S#…`, which
> this reader rejects. Reading those files needs a leniency mode first: accept zero-width
> strong types `Z`/`T`/`F` and variable-width `$S`/`$H` arrays (iteration only, no random
> access). That is purely additional accepted grammar and drops into the same core without
> changing the API.

## Building and testing

Header only; add `include/` to your include path, or link the `bjdata` INTERFACE target.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Tests build with AddressSanitizer and UndefinedBehaviorSanitizer by default
(`-DBJDATA_TEST_SANITIZE=OFF` to disable). Every suite truncates its documents at each byte
offset and requires a clean rejection, so out-of-bounds reads fail the build rather than
lurk.

`bjdata_fixture_test` checks against the reference implementation rather than against
vectors transcribed by hand. For each of 44 documents, dart-bjdata encodes it and decodes
those exact bytes back, and then four things must hold:

1. the view's block notation matches dart's, token for token;
2. the view decodes to the same values dart decodes;
3. **re-encoding those decoded values through the writer reproduces dart's bytes exactly** —
   which, because the reference chose every marker from the value, proves the whole ladder at
   once: integer widths, float narrowing, the packing measurement, container shapes and key
   encoding;
4. splicing the document with `write_value` reproduces it byte for byte;
5. **the JSON output matches dart-bjdata's own JSON rendering of those bytes exactly** —
   number formatting, key order, escaping and indentation included.

A sixth pass parses dart-bjdata's own JSON for every fixture and requires the JSON reader to
produce the same values the BJData reader produces from the same document.

Every header is also compiled on its own, since a missing include is otherwise hidden by
whichever header a test happened to include first.

The corpus deliberately straddles the packing decision (four elements versus five, a single
large value that forces a wider marker, a size tie that must stay generic), since that is
where a writer is easiest to get subtly wrong.

```sh
cd ~/Work/Troo/dart-bjdata && dart compile exe bin/bjdata.dart -o /tmp/bjdatacli
python3 test/generate_fixtures.py /tmp/bjdatacli
```

## Layout

| directory | namespace | contents |
|---|---|---|
| `serial/` | `nonstd::serial` | format-neutral: `kind`, `errc`, sinks, the emitter, the customization layer |
| `bjdata/` | `nonstd::bjdata` | markers, `view`, `writer`, N-D arrays, block notation |
| `json/` | `nonstd::json` | `scanner`, `reader`, `writer` |
| `bjdata/json.hpp` | `nonstd::bjdata` | the one header that knows both formats |

Each format namespace adopts the neutral one (`using namespace serial;`), so `bjdata::view`
and `json::reader` both see `errc`, `kind` and `serializer` without qualification, and a
`using namespace nonstd::bjdata;` in your own code picks them up too. Using both formats at
once means qualifying `bjdata::writer` against `json::writer`, which is the point.

`serial/` depends on neither format; `bjdata/` and `json/` each depend only on `serial/`, and
that is enforced by the build rather than by convention — every header compiles on its own.
The single bridge is `bjdata/json.hpp`, which renders a BJData document as JSON — and is
also how a `to_bjdata`-only type reaches JSON, by being encoded and then transcribed:

```cpp
const auto encoded = to_bytes(value);
const auto text = to_json(view::over(encoded));
```

That is deliberately explicit. A hidden intermediate inside `to_json` would allocate a whole
document behind the caller's back.

The `nonstd/` include paths deliberately match sunrise-firmware's
`lib/common/include/nonstd/`, so these headers can be dropped in there unchanged.
`unaligned.hpp` is a verbatim copy of the firmware's and must not fork.

One caveat for that move: `notation.hpp` uses `std::format`, which appears nowhere in the
firmware — it is fmt-only. That header needs an fmt spelling, or should stay host-only.
