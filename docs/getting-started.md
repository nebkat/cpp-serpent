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

Everything is headers but one bundled source file, which writes real numbers and is built into a
small library that `serpent::serpent` links for you. Set `SERPENT_USE_ZMIJ` to `OFF` before
`FetchContent_MakeAvailable` to go without it: the output is identical, reals are written about
three times slower, and the library is header-only. Using the headers without CMake gets that
by default. It is one of several such switches, each with a plainer way kept beside it;
[Configuration](configuration.md) lists them and what each costs.

## Your first program

```cpp
#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

struct [[= serpent::serializable {}]] station { // (1)!
    std::string name;
    std::optional<int> altitude;
    std::vector<double> samples;
};

int main() {
    const station value { "north ridge", 1840, { 4.5, 4.25, 3.75 } };

    const auto bytes = serpent::bjdata::encode(value);        // compact binary
    const auto text = serpent::json::encode(value, { .indent = 2 });   // readable

    const auto back = serpent::bjdata::decode<station>(bytes);
    const auto also = serpent::json::decode<station>(text);
}
```

1.  Needs a compiler that can enumerate the fields for you: GCC 16 with `-freflection`. On any
    other, drop the annotation and write a `json_convert` naming the three fields — see
    [Types and conversions](types.md#one-function-both-directions). Nothing else in this program changes. Note that the
    annotation is a *parse error* on a compiler that does not know it rather than something
    quietly ignored, so a header built by two toolchains needs both forms behind
    `#if SERPENT_HAS_REFLECTION`.


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
