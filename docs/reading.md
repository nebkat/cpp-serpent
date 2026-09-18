# Reading

## When it fails, and why

`decode` says whether it worked:

```cpp
if (const auto config = json::decode<link_config>(text)) { /* … */ }
```

`try_decode` says why it did not:

```cpp
const auto config = json::try_decode<link_config>(text);
if (!config) {
    log("bad config: %s at %zu", describe(config.error().code()), config.error().offset());
}
```

A document that does not parse is reported with the kind and the byte offset — `unexpected_end`,
`invalid_escape`, `invalid_number` and the rest. One that parses but is not this shape is
`type_mismatch`, and carries no offset: nothing on the way in recorded where the shape stopped
matching.

It costs a second pass, because validating and decoding are separate walks. Reach for `decode`
when the answer is all you want.

## Two readers, one shape

`bjdata::reader` and `json::reader` are the same kind of thing - a handle to one value inside a
buffer, holding nothing of its own - with one difference the format dictates: BJData values
*are* the bytes, so the binary reader can lend them (`as<std::string_view>()`, a typed array as a
span), where a JSON string has to be decoded and is only ever handed over as a `std::string`.
Everything else is the same: the same `errc`, the same `as<T>()`, the same iteration.

A reader can be moved but not copied, for a reason explained [below](#walking-a-document-costs-one-pass),
so a loop over one is written `for (const auto &element : ...)`.

```cpp
auto document = bjdata::reader::over(bytes);    // or json::reader::over(text)

document["name"];                 // missing key gives an invalid handle, not an error
document["ports"][2];
for (const auto &element : document["ports"].array()) { }
for (const auto &[key, value] : document.items()) { }
```

## Three tiers over one parser

=== "Total — never throws"

    ```cpp
    auto label = document["meta"]["label"].as<std::string_view>().value_or("unnamed");
    ```

    A missing key, a wrong type, or a corrupt document all yield nothing. Traversal of an
    invalid handle stays invalid, so there is no per-step checking.

=== "Checked — throws"

    ```cpp
    try {
        auto label = document.at("meta").at("label").get<std::string_view>();
    } catch (const serpent::error &failure) {
        std::println("{} at byte {}", failure.what(), failure.offset());
    }
    ```

=== "Validating — once, up front"

    ```cpp
    if (auto problem = bjdata::validate(bytes); !problem) {
        std::println("{} at byte {}", problem.error().what(), problem.error().offset());
    }
    ```

!!! note "Safe either way"

    Every access is bounds-checked against the buffer, so walking a truncated or corrupt
    document is safe even without `validate()`. Nesting is capped, so a hostile document
    cannot exhaust the stack.

## Accessors

On both handles:

```cpp
value.type();          // null, boolean, integer, real, string, array, object, invalid
value.is_array();
value.size();

value.as<bool>();
value.as<std::uint16_t>();     // range-checked, not truncated
value.as<double>();
value.as<std::string_view>();
```

On `bjdata::reader` only:

```cpp
value.as<std::span<const std::byte>>();                 // a [$B# array, as its raw bytes
value.as<nonstd::unaligned_little_span<const std::uint16_t>>();    // a [$u# array, in place
```

These cannot exist on `json::reader`, and the reason is the format rather than the API. A span
hands back a contiguous run of `T` out of the buffer — but in JSON `[900,901,902]` is *text*,
so there are no `std::uint16_t` in there to point at. JSON has no binary type either.

The JSON equivalent is to build the container you wanted:

```cpp
// BJData: a span over the bytes, no allocation
auto samples = document["samples"].as<nonstd::unaligned_little_span<const std::uint16_t>>();

// JSON: decoded into a container you own
auto samples = json::decode<std::vector<std::uint16_t>>(text);
for (const auto &element : reader["samples"].array()) element.as<std::uint16_t>();
```

!!! note "Not alignment-sensitive"

    `as<nonstd::unaligned_little_span<const T>>()` returns an `unaligned_little_span<const T>`, so the bytes need no
    particular alignment and elements are read through a proxy. It is still a
    `random_access_range`, so `<algorithm>` and `<ranges>` apply.

## Strings

```cpp
// BJData: the value IS the bytes
std::optional<std::string_view> borrowed = document["name"].as<std::string_view>();

// JSON: escapes mean the value must be built
std::optional<std::string> decoded = reader["name"].as<std::string>();
std::string into;
reader["name"].read_string_into(into);
reader["name"].decode_string_into(buffer);   // no allocation; refuses to truncate
reader["name"].string_is("expected");        // compares without materialising
```

!!! warning "No conditional borrowing"

    JSON never hands back a `string_view` that happens to work when the data has no escapes.
    An API whose shape depends on its contents passes testing and fails in the field.

## Walking a document costs one pass

A forward iterator has to know where a value ends before it can hand you the next one, and with
nothing written down the only way to know is to walk it. Left alone, that means a byte is walked
once by the loop that wants it and again by the loop stepping over it — once for every level of
nesting above it.

It is not left alone. Every reader keeps a note of how far a walk of *it* got, and the iterator
that owns it reads the note to step past it:

```cpp
for (const auto &row : document.array())          // outer
    for (const auto &value : row.array())         // inner, and it leaves the note in `row`
        total += value.as<int>();                 // so the outer step resumes
```

Nothing is shared: not between two traversals, not between threads. The note lives in the
handle, which is why the handle cannot be copied - a copy would have a note of its own, and
`for (auto row : ...)` would silently walk every byte twice; `for (auto &row : ...)` walks them
once, and the compiler insists on the `&`. A traversal the note does not cover simply scans,
exactly as it would have anyway, which is why a value read twice, two handles held at once, and
members taken out of order all keep working.

Decoding into a type gets the same treatment from the other side: a reflected type is read by
walking the document once and handing each key to the member that claims it, so a document whose
keys arrive in an order you did not choose costs no more than one that matches your declarations.

## An index, for a document read more than once

Where the same large document is traversed repeatedly, `#!cpp #include <serpent/json/indexed.hpp>`
records where every value is, once:

```cpp
const auto index = json::structural_index::over(text);

for (const auto &row : index.root()["rows"].array())      // every step is a hop, not a scan
    total += row["value"].as<double>().value_or(0);
```

It answers everything an ordinary reader answers, so anything that takes a reader takes this.
One entry per value at twelve bytes — for a 1.6 MB document, 0.4 MB — and building it costs
about what validating costs. Worth it from the second traversal; for a
single pass, or for a message rather than a file, read normally.
