# serpent

Zero-copy **JSON** and **BJData** for C++23. One definition per type serves both formats, in
both directions.

📖 **Documentation** — built from `docs/`; run `mkdocs serve` to read it locally.

```cpp
#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

struct [[= serpent::serializable {}]] reading {
    std::uint32_t at;
    double celsius;
};

const auto text = serpent::json::encode(value);      // {"at":1700000000,"celsius":4.5}
const auto bytes = serpent::bjdata::encode(value);   // the same value, compact binary

const auto a = serpent::json::decode<reading>(text);
const auto b = serpent::bjdata::decode<reading>(bytes);
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

## Why

**There is no DOM.** Nothing is inflated from bytes into an intermediate object graph and then
converted into your classes — you get forward iterators into the bytes you already have, and
decoders that go straight from those into your types. There is a tree for the documents whose
shape is only known at run time, in a header nothing else includes, and that is the whole of its
role.

```cpp
auto document = serpent::json::reader::over(text);
document["port"].as<int>();                // parsed on demand, nothing built

auto stored = serpent::bjdata::reader::over(bytes);
stored["name"].as<std::string_view>();                    // string_view INTO bytes
stored["samples"].as<nonstd::unaligned_little_span<const std::uint16_t>>();    // span INTO bytes
```

BJData borrows strings outright, because the value *is* the bytes. JSON decodes them, because
an escape means it is not — see [Reading](docs/reading.md#strings).

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
function forms remain first-class — see [Reflection](docs/reflection.md).
| | |
|---|---|
| Headers, and one source file | C++23, one dependency: [cpp-unaligned](https://github.com/nebkat/cpp-unaligned). The source file is a bundled real-number writer; `-DSERPENT_USE_ZMIJ=OFF` does without it |
| Allocates | only where you ask it to |
| Without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| BJData output | as small as it can be, or as quick to write and read, whichever you prefer |

## Install

```cmake
include(FetchContent)
FetchContent_Declare(serpent
        GIT_REPOSITORY https://github.com/nebkat/cpp-serpent.git
        GIT_TAG main)
FetchContent_MakeAvailable(serpent)

target_link_libraries(your_target PRIVATE serpent::serpent)
```

## Build

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Configuring fetches `cpp-unaligned` at a pinned tag, so the first configure needs network
access. Tests build with ASan and UBSan by default.

## Documentation

| | |
|---|---|
| [Getting started](docs/getting-started.md) | build it, run something |
| [Your types](docs/types.md) | opting a type in, whether or not you own it |
| [Reflection](docs/reflection.md) | letting the compiler name the fields |
| [Reading](docs/reading.md) · [Writing](docs/writing.md) | the API |
| [JSON](docs/json.md) · [BJData](docs/bjdata.md) | format specifics |
| [Benchmarks](docs/benchmarks.md) | against four other libraries, including where it loses |
| [Design](docs/design.md) | why it is shaped this way |

```sh
pip install -r docs/requirements.txt
mkdocs serve
```

Publishing to GitHub Pages needs a plan that allows it for a private repository. The workflow
builds the site on every push and deploys once the `PAGES_ENABLED` repository variable is set.

## Acknowledgements

serpent owes the most to [Glaze](https://github.com/stephenberry/glaze). Its approach — one
description of a type serving both directions, reflection writing that description where the
compiler can, and the reader handing back views into the caller's own buffer rather than a tree —
is the shape serpent is built in, and Glaze got there first and faster. Its integer formatting is
not merely an influence but vendored outright, in `include/serpent/external/glaze/`. The
benchmarks measure against it precisely because it is the bar worth clearing, and
[Benchmarks](docs/benchmarks.md) records the workloads where it still wins.

Two more libraries are here as code rather than as ideas: [Żmij](https://github.com/vitaut/zmij)
by Victor Zverovich writes the shortest text that round-trips a real, and
[fast_float](https://github.com/fastfloat/fast_float) by Daniel Lemire, João Paulo Magalhaes and
its contributors reads one back exactly rounded. Both do a job that is easy to get subtly wrong
and that neither the standard library nor this author would have done as well.
`glaze/itoa_40kb.hpp` carries its own lineage back to ibireme's `itoa_yy.c` and RealTimeChris's
Jsonifier.

The BJData format itself is [a specification by Qianqian Fang and
contributors](https://neurojson.org/bjdata), building on Universal Binary JSON.

## Licence

MIT — see [LICENSE.md](LICENSE.md), which also lists the licences of the code above.
