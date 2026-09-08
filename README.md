# serpent

Zero-copy **JSON** and **BJData** for C++23. One definition per type serves both formats, in
both directions.

📖 **Documentation** — built from `docs/`; run `mkdocs serve` to read it locally.

```cpp
#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

struct [[= serpent::serializable {}]] reading {
    std::uint32_t at;
    double celsius;
};

const auto text = json::encode(value);      // {"at":1700000000,"celsius":4.5}
const auto bytes = bjdata::encode(value);   // the same value, compact binary

const auto a = json::decode<reading>(text);
const auto b = bjdata::decode<reading>(bytes);
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
decoders that go straight from those into your types.

```cpp
auto document = json::reader::over(text);
document["port"].as_int<int>();                // parsed on demand, nothing built

auto stored = bjdata::view::over(bytes);
stored["name"].as_string();                    // string_view INTO bytes
stored["samples"].as_span<std::uint16_t>();    // span INTO bytes
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
| Header only | C++23, one dependency: [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) |
| Allocates | only where you ask it to |
| Without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| JSON output | byte-identical to the reference implementation's own JSON |
| BJData output | byte-identical to the reference implementation |

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
