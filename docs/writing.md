# Writing

`encode` gives a fresh string; `write` puts the document into a string you already have,
replacing what it held and keeping its capacity, so many documents can go through one buffer:

```cpp
std::string buffer;
for (const auto &record : records) {
    json::write(record, buffer);
    send(buffer);
}
```

Anything else the bytes should go to is a sink.

## Sinks

A sink is anything with a member `write(std::span<const std::byte>)`. The return type is
unconstrained, so classes you already have qualify unmodified whether they return `void`,
`bool`, a byte count or an `expected`.

```cpp
std::vector<std::byte> buffer;
serpent::container_sink out { buffer };
bjdata::writer writer { out };
```

| Sink | For | |
|---|---|---|
| `container_sink` | a growable container | written into directly |
| `span_sink` | a fixed buffer; latches overflow, never truncates | written into directly |
| `counting_sink` | sizing, or hashing in one pass | |
| `iterator_sink` | any output iterator | |
| `ostream_sink` | a `std::ostream` (its own header) | |

A bare lambda works too: `serpent::bjdata::writer w { callback };`

### Sizing the destination

A sink over contiguous storage is written into in place: the document is composed in the
buffer that will hold it, rather than gathered elsewhere and copied in. To do that the writer
asks the container for the most a value could take before it writes it, so a container grows
somewhat past its document the first time — and a buffer that has held a document of that size
is not grown again:

```cpp
std::string buffer;
for (const auto &value : values) {
    serpent::json::write(value, buffer);   // allocates the first time round, then never
    send(buffer);
}
```

Where nothing may allocate at all, a `span_sink` over a fixed buffer is the sink to use: it
hands the writer whatever is left of it, and latches `overflowed()` only when a value will not
fit at all.

## Writing

```cpp
{
    const auto document = writer.object();
    document.member("host", "example.com");
    document.member("samples", std::vector<int> { 1, 2, 3 });
}
const auto written = writer.finish();   // std::expected<std::size_t, error>
```

A container is opened only by a scope, which closes it. There is no `begin_object` to forget.
Scopes are movable, so one can live in a `std::optional` to open in one place and close in
another.

## A type plus a few fields of your own

Where a document is some type's members *and* something decided at the call site, and has to stay
flat — writing the value as a member would nest it, and a nested document is a different
document:

```cpp
const auto object = out.object();
serpent::write_members(out, partition);      // the type's own members
object.member("state", state);               // and the ones only this caller knows
```

`write_members` opens nothing and closes nothing, so an object has to be open already; a writer
with none latches `errc::key_outside_object` as it would for any stray key. `read_members` is the
inverse — the type reads its own members out of an object holding more than them, and leaves the
rest alone.

It works for a type that names its fields, reflected or with a `json_convert`. A `to_json` writes
its own object and has no members to lend, so it is refused; split its body into a function taking
the open scope if you need this for one.


## `finish()` is what completes the document

Writers emit at token granularity — a brace, a key, a separator — and the sink is reached
through a type-erased call that cannot be inlined, so bytes are gathered before being handed
over - into the destination itself for the sinks above that allow it, and into a small internal
batch for the rest. That makes BJData encoding about 1.7x quicker, at the
cost of one rule:

!!! warning "A sink does not hold the whole document until `finish()`"

    Call `finish()` before reading what a sink collected. Destroying the writer flushes too, so
    scoping it works as well, but reading the sink while the writer is still alive and
    unfinished gives you only what has been handed over so far.

    A failure can likewise surface at `finish()` rather than at the write that caused it: a
    write smaller than the batch may not have reached the sink yet. `finish()` is where the
    answer is, which is what it was for already.

Writers are neither copyable nor movable — two of them sharing one sink would hand over the
same batch twice.

## Failures latch

The first error is kept and every later call is a no-op, so you check once at the end:

```cpp
std::array<std::byte, 64> storage {};
serpent::span_sink out { storage };
bjdata::writer writer { out };
writer.value(large_document);

if (!writer.finish()) { /* did not fit */ }
```

## Sizing first

```cpp
const auto size = bjdata::measure(value);   // exact, no allocation
```
