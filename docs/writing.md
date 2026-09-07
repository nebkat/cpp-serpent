# Writing

## Sinks

A sink is anything with a member `write(std::span<const std::byte>)`. The return type is
unconstrained, so classes you already have qualify unmodified whether they return `void`,
`bool`, a byte count or an `expected`.

```cpp
std::vector<std::byte> buffer;
serpent::container_sink out { buffer };
bjdata::writer writer { out };
```

| Sink | For |
|---|---|
| `container_sink` | a growable container |
| `span_sink` | a fixed buffer; latches overflow, never truncates |
| `counting_sink` | sizing, or hashing in one pass |
| `iterator_sink` | any output iterator |
| `ostream_sink` | a `std::ostream` (its own header) |

A bare lambda works too: `serpent::bjdata::writer w { callback };`

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

## `finish()` is what completes the document

Writers emit at token granularity — a brace, a key, a separator — and the sink is reached
through a type-erased call that cannot be inlined, so bytes are gathered into a small internal
batch and handed over when it fills. That makes BJData encoding about 1.7x quicker, at the
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
