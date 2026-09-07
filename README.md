# serpent

Zero-copy **BJData** and **JSON** for C++23. One definition per type serves both formats, in
both directions.

📖 **Documentation** — built from `docs/`; run `mkdocs serve` to read it locally.

```cpp
#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

struct reading {
    std::uint32_t at;
    double celsius;

    SERPENT_DEFINE_TYPE(reading, at, celsius)
};

const auto bytes = bjdata::encode(value);   // compact binary
const auto text = json::encode(value);      // {"at":1700000000,"celsius":4.5}

const auto a = bjdata::decode<reading>(bytes);
const auto b = json::decode<reading>(text);
```

## Why

**There is no DOM.** Nothing is inflated from bytes into an intermediate object graph and then
converted into your classes — you get forward iterators into the bytes you already have, and
decoders that go straight from those into your types.

```cpp
auto document = bjdata::view::over(bytes);

document["name"].as_string();                  // string_view INTO bytes
document["samples"].as_span<std::uint16_t>();  // span INTO bytes
```

| | |
|---|---|
| Header only | C++23, one dependency: [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) |
| Allocates | only where you ask it to |
| Without exceptions | the whole non-throwing tier stays intact under `-fno-exceptions` |
| BJData output | byte-identical to [dart-bjdata](https://github.com/nebkat/dart-bjdata) |
| JSON output | byte-identical to dart-bjdata's own JSON |

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
| [Your types](docs/types.md) | three ways to opt a type in |
| [Reading](docs/reading.md) · [Writing](docs/writing.md) | the API |
| [BJData](docs/bjdata.md) · [JSON](docs/json.md) | format specifics |
| [Design](docs/design.md) | why it is shaped this way |

```sh
pip install -r docs/requirements.txt
mkdocs serve
```

Publishing to GitHub Pages needs a plan that allows it for a private repository. The workflow
builds the site on every push and deploys once the `PAGES_ENABLED` repository variable is set.
