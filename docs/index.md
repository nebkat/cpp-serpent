# serpent

Zero-copy **JSON** and **BJData** for C++23. One definition per type serves both formats, in
both directions.

```cpp
struct [[= serpent::serializable {}]] reading {
    std::uint32_t at;
    double celsius;
};

auto text  = json::encode(value);     // std::string
auto bytes = bjdata::encode(value);   // std::vector<std::byte>

auto a = json::decode<reading>(text);
auto b = bjdata::decode<reading>(bytes);
```

The compiler already knows what the fields are called, so that annotation is the whole
definition. It needs GCC 16 with `-freflection`; on a compiler without it, name the fields once
yourself and nothing else changes:

```cpp
struct reading {
    std::uint32_t at;
    double celsius;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), reading> value) {
        visitor.member("at", value.at);
        visitor.member("celsius", value.celsius);
    }
};
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

## Where the compiler can, it writes the definition for you

On GCC 16 with `-freflection`, the field list goes away entirely:

```cpp
struct [[= serpent::serializable {},
       = serpent::naming { serpent::naming_style::snake_case }]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress;
    [[= serpent::skip {}]]   int cacheGeneration;
                             int mtuBytes;          // becomes "mtu_bytes"
};
```

No other released compiler has the whole C++26 feature set this needs yet, so the macro and
function forms remain first-class — see [Reflection](reflection.md).

## At a glance

| | |
|---|---|
| Header only | C++23, no dependencies beyond [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) |
| Allocates | only where you ask it to |
| Works without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| JSON output | byte-identical to the reference implementation's own JSON |
| BJData output | byte-identical to the reference implementation |

## Where to go

<div class="grid cards" markdown>

- :material-rocket-launch: **[Getting started](getting-started.md)** — build it, run something
- :material-code-braces: **[Your types](types.md)** — three ways to opt a type in
- :material-book-open-variant: **[Reading](reading.md)** and **[Writing](writing.md)** — the API
- :material-thought-bubble: **[Design](design.md)** — why it is shaped this way

</div>
