# serpent

Zero-copy **BJData** and **JSON** for C++23. One definition per type serves both formats, in
both directions.

```cpp
struct reading {
    std::uint32_t at;
    double celsius;

    SERPENT_DEFINE_TYPE(reading, at, celsius)
};

auto bytes = bjdata::encode(value);   // std::vector<std::byte>
auto text  = json::encode(value);     // std::string

auto a = bjdata::decode<reading>(bytes);
auto b = json::decode<reading>(text);
```

## What makes it different

**There is no DOM.** Nothing is inflated from bytes into an intermediate object graph and then
converted into your classes. You get forward iterators into the bytes you already have, and
decoders that go straight from those into your types.

=== "Reading a document"

    ```cpp
    auto document = bjdata::view::over(bytes);

    document["name"].as_string();              // string_view INTO bytes
    document["samples"].as_span<std::uint16_t>();   // span INTO bytes
    ```

=== "Reading into your type"

    ```cpp
    auto config = bjdata::decode<link_config>(bytes);
    ```

## At a glance

| | |
|---|---|
| Header only | C++23, no dependencies beyond [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) |
| Allocates | only where you ask it to |
| Works without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| BJData output | byte-identical to [dart-bjdata](https://github.com/nebkat/dart-bjdata) |
| JSON output | byte-identical to dart-bjdata's own JSON |

## Where to go

<div class="grid cards" markdown>

- :material-rocket-launch: **[Getting started](getting-started.md)** — build it, run something
- :material-code-braces: **[Your types](types.md)** — three ways to opt a type in
- :material-book-open-variant: **[Reading](reading.md)** and **[Writing](writing.md)** — the API
- :material-thought-bubble: **[Design](design.md)** — why it is shaped this way

</div>
