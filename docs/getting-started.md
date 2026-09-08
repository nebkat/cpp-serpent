# Getting started

## Add it to your build

```cmake
include(FetchContent)
FetchContent_Declare(serpent
        GIT_REPOSITORY https://github.com/nebkat/cpp-serpent.git
        GIT_TAG main)
FetchContent_MakeAvailable(serpent)

target_link_libraries(your_target PRIVATE serpent::serpent)
```

It pulls [cpp-unaligned](https://github.com/nebkat/cpp-unaligned) itself, so the first
configure needs network access.

## Your first program

```cpp
#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

struct [[= serpent::serializable {}]] station { // (1)!
    std::string name;
    std::optional<int> altitude;
    std::vector<double> samples;
};

int main() {
    const station value { "north ridge", 1840, { 4.5, 4.25, 3.75 } };

    const auto bytes = bjdata::encode(value);        // compact binary
    const auto text = json::encode(value, { .indent = 2 });   // readable

    const auto back = bjdata::decode<station>(bytes);
    const auto also = json::decode<station>(text);
}
```

1.  Needs a compiler that can enumerate the fields for you: GCC 16 with `-freflection`. On any
    other, drop the annotation and name them once instead —
    `SERPENT_DEFINE_TYPE(station, name, altitude, samples)` inside the struct. Nothing else in
    this program changes. See [Reflection](reflection.md).


That is the whole surface for most uses: `encode`, `decode`, and one annotation on your type.

## Building this repository

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests build with AddressSanitizer and UndefinedBehaviorSanitizer by default
(`-DSERPENT_TEST_SANITIZE=OFF` to turn that off).

## Examples

`example/` is built and run by `ctest`, so it cannot quietly stop compiling.

| File | Shows |
|---|---|
| `quickstart.cpp` | one type, both formats, both directions |
| `reading_without_copying.cpp` | a document in a fixed buffer, read back with no allocation |
| `reflection.cpp` | annotations naming the keys, with today's fallback |
| `transcoding.cpp` | a stored document rendered as JSON, one value lifted out |
