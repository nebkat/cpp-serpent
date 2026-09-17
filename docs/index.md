# serpent

Zero-copy **JSON** and **BJData** for C++23. Annotate a type once; it reads and writes in every
format, in both directions.

```cpp
struct [[= serpent::serializable {}]] reading {
    std::uint32_t at;
    double celsius;
};

auto text  = json::encode(value);          // std::string
auto bytes = bjdata::encode(value);        // std::vector<std::byte>

auto a = json::decode<reading>(text);      // std::optional<reading>
auto b = bjdata::decode<reading>(bytes);
```

That annotation is the whole definition. The compiler already knows what the fields are called,
so nothing lists them again — and nothing can fall out of step with them when one is added.

## Say more where you need to

```cpp
struct [[= serpent::serializable {},
       = serpent::naming { serpent::naming_style::snake_case }]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress;
    [[= serpent::skip {}]]   int cacheGeneration;
                             int mtuBytes;            // becomes "mtu_bytes"
    [[= serpent::defaulted {}]] int retries = 3;      // older documents may omit it
};
```

Ten annotations cover renaming, omitting, naming rules, optional and required members,
enumerations that are words rather than numbers, and variants that say what they are.
See [Annotations](reflection.md).

!!! tip "And the compiler checks what it can see"

    Two members that end up as the same key, an enumeration where two values are the same word,
    `required` on a member that is required already — those are build errors naming what they
    found, rather than documents that come out wrong.

## Reading is lazy and allocates nothing

```cpp
auto document = json::reader::over(text);

document["port"].as_int<int>();                   // parsed on demand
for (auto host : document["hosts"].array()) { }   // walked in place
```

Nothing is inflated into an object graph first. A reader is a small handle over bytes you
already own, strings and spans point *into* them, and stopping early costs only what you read.
See [Reading](reading.md).

## At a glance

| | |
|---|---|
| Headers, and one source file | C++23, one dependency: [cpp-unaligned](https://github.com/nebkat/cpp-unaligned). The source file is the bundled real-number writer, and [can be turned off](json.md#formatting) |
| Allocates | only where you ask it to; reading allocates nothing |
| Without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| Types you do not own | described from outside, with no conversion code — [here](reflection.md#a-type-you-cannot-annotate) |
| Output | BJData [as small or as quick as it can be](bjdata.md#size-or-speed), whichever you prefer |

!!! note "Reflection needs GCC 16.1 with `-freflection`"

    No other released compiler has the whole C++26 feature set yet, and the annotation is a
    *parse error* on one that does not — so a header shared between toolchains carries both
    forms behind `#if SERPENT_HAS_REFLECTION`. Naming your fields once by hand is the other
    form, and nothing else about the library changes. See
    [Requirements](reflection.md#requirements).

## Where to go

<div class="grid cards" markdown>

- :material-rocket-launch: **[Getting started](getting-started.md)** — build it, run something
- :material-tag-text: **[Annotations](reflection.md)** — everything a type can say about itself
- :material-package-variant: **[Types and conversions](types.md)** — what works already, and writing your own
- :material-book-open-variant: **[Reading](reading.md)** and **[Writing](writing.md)** — the API
- :material-file-tree: **[A document as a tree](value.md)** — when the shape is decided at run time
- :material-speedometer: **[Benchmarks](benchmarks.md)** — against four other libraries, wins and losses

</div>
