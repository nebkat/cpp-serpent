# A document as a tree

Everything else in serpent goes straight between bytes and your types. `serpent::value` is the
one piece that does not: an owning tree you build a node at a time, for a document whose shape is
not known until it is written.

```cpp
#include <serpent/value.hpp>
```

Its own header, included by nothing else. Nothing in the library depends on it, so a build that
never mentions it never pays for it — which is the only reason it can exist without contradicting
everything on the [design](design.md) page.

## When it is the right answer

When you are producing a document and the fields are decided at run time:

```cpp
serpent::value detail;
detail["records"] = records.size();
detail["asked"] = requested;
if (!crcs.empty()) detail["crcs"] = crcs.size();

throw failure { "out_of_range", std::move(detail) };
```

That is the thing a struct cannot express: the members differ per call site, the branches add
different fields, and the value has to outlive the frame that started it. A helper taking
alternating keys and values covers the first line and nothing else — it cannot nest, cannot be
added to across branches, and cannot be held.

## When it is not

**Reading.** A document you have the bytes of is already a tree, and one that costs nothing:
`view::over(bytes)` and `reader::over(text)` walk it in place, with every string and span pointing
into the buffer you already had. Building a `value` out of one copies the whole document to learn
what the view would have told you for free.

**A shape you know.** Say it as a struct and let reflection write both directions. A tree gives up
the type checking, the member names and the one-pass reader in exchange for a flexibility you are
not using.

## Building

| | |
|---|---|
| `value {}` | null |
| `value { 3 }`, `value { 1.5 }`, `value { true }`, `value { "text" }` | the scalars |
| `value::of({ { "x", 1 }, { "y", 2 } })` | an object, written out |
| `value::of({ 1, 2, 3 })` | an array, written out |
| `document["key"]` | the member, added as null if it was not there |
| `document.push_back(item)` | appends, making an array of a null value |

`operator[]` and `push_back` turn a null value into an object or an array on the way, so a
document grows from nothing without being declared first. They refuse — `errc::type_mismatch` —
on a value that already holds something else, rather than discarding it.

```cpp
serpent::value document;
document["window"]["first"] = 10;     // both objects made on the way
document["ids"].push_back(1);
```

## Any serializable type, as a tree

A tree built by hand holds the builtin kinds. `to_value` reaches everything else — through the
type's own conversion, whichever form it takes, because the destination being a tree rather than
bytes is the only difference:

```cpp
auto tree = serpent::to_value(reading);   // annotated, tabulated or hand-written, it does not matter
tree["received_at"] = now;                // then shaped at run time
bjdata::write(socket, tree);
```

Nothing is written for a type to be reachable this way. `serpent::value_writer` answers the same
protocol `bjdata::writer` and `json::writer` answer, so a type that can be written at all can be
written here, and a tree built from a value encodes byte for byte as writing that value directly
would.

### And back out of it

`from_value` is the other direction, and it goes through the same code every other reader does:

```cpp
const auto recovered = serpent::from_value<reading>(tree);
```

`serpent::value_reader` is what makes that work — a handle to one node answering exactly what
`view` and `reader` answer, so nothing that decodes has to know which of the three it was given.
That is the reason the tree is a third way to *hold* a document rather than a separate world:

| holds | owns | finding the next value |
|---|---|---|
| `view`, `reader` | nothing | scanning the bytes |
| `value_reader` | the values themselves | already there |

Reading from bytes is still the cheaper path, and the one to reach for when you have them. This
is for when what you have is a tree.

## Reading it back

The same accessors the readers have, in the same shapes: `as_bool()`, `as_int<T>()`,
`as_float<T>()`, `as_string()`, `as_binary()`, each returning `std::optional`. `at(key)` and
`at(index)` throw, naming the key; `operator[]` on a missing member gives null.

Integer width is not kept. A value holds `std::int64_t`, `std::uint64_t` or `double`, and the
writer narrows every integer to the smallest marker that holds it anyway — so a document
round-tripped through a tree comes back byte-identical under the default options, and wider than
it went in under `no_compaction`.

Objects remember the order their members were added, because a document built by hand is usually
read by a person. Neither format ascribes meaning to key order, and two objects with the same
members compare equal whatever order they were built in.

## Everywhere else a type goes

`value` is an ordinary serializable type: a member of a struct, an element of a container, or the
whole document.

```cpp
struct [[= serpent::serializable {}]] failure {
    std::string reason;
    serpent::value detail;     // whatever this particular failure had to say
};
```

Binary stays binary in BJData, which has a type for it, and travels as an array of numbers in
JSON, which does not — the same rule as `std::vector<std::byte>` anywhere else.
