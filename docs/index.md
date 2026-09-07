# serpent

Zero-copy **JSON** and **BJData** for C++23. One definition per type serves both formats, in
both directions.

```cpp
struct reading {
    std::uint32_t at;
    double celsius;

    SERPENT_DEFINE_TYPE(reading, at, celsius)
};

auto text  = json::encode(value);     // std::string
auto bytes = bjdata::encode(value);   // std::vector<std::byte>

auto a = json::decode<reading>(text);
auto b = bjdata::decode<reading>(bytes);
```

## What makes it different

**There is no DOM.** Nothing is inflated from bytes into an intermediate object graph and then
converted into your classes. You get forward iterators into the bytes you already have, and
decoders that go straight from those into your types.

=== "Reading JSON"

    ```cpp
    auto document = json::reader::over(text);

    document["port"].as_int<int>();                      // parsed on demand
    for (auto host : document["hosts"].array()) { }      // walked in place
    ```

=== "Reading BJData"

    ```cpp
    auto document = bjdata::view::over(bytes);

    document["name"].as_string();                        // string_view INTO bytes
    document["samples"].as_span<std::uint16_t>();        // span INTO bytes
    ```

=== "Into your type"

    ```cpp
    auto config = json::decode<link_config>(text);
    ```

BJData borrows strings outright, because the value *is* the bytes. JSON decodes them, because
an escape means it is not — see [Reading](reading.md#strings).

## At a glance

| | |
|---|---|
| Header only | C++23, no dependencies beyond [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) |
| Allocates | only where you ask it to |
| Works without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| JSON output | byte-identical to dart-bjdata's own JSON |
| BJData output | byte-identical to [dart-bjdata](https://github.com/nebkat/dart-bjdata) |

## Where to go

<div class="grid cards" markdown>

- :material-rocket-launch: **[Getting started](getting-started.md)** — build it, run something
- :material-code-braces: **[Your types](types.md)** — three ways to opt a type in
- :material-book-open-variant: **[Reading](reading.md)** and **[Writing](writing.md)** — the API
- :material-thought-bubble: **[Design](design.md)** — why it is shaped this way

</div>
