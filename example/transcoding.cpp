// Rendering a stored BJData document as JSON, and lifting one value out of it.

#include <serpent/bjdata.hpp>
#include <serpent/bjdata/json.hpp>
#include <serpent/bjdata/notation.hpp>
#include <serpent/json.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

int main() {
    // Something already on disk, or off a wire.
    const auto stored = bjdata::encode(std::map<std::string, std::vector<int>> {
            { "alpha", { 1, 2, 3 } },
            { "beta", { 40, 50, 60 } },
    });

    if (const auto problem = bjdata::validate(stored); !problem) {
        std::printf("malformed at byte %zu: %s\n", problem.error().offset(), problem.error().what());
        return 1;
    }

    const auto document = bjdata::reader::over(stored);

    // Straight to JSON, without decoding into anything in between.
    std::printf("%s\n\n", json::encode(document, { .indent = 2 }).c_str());

    // A token-level trace of the bytes, for when a document is not what you expected.
    std::printf("%s\n\n", bjdata::block_notation(stored).c_str());

    // Lifting one value out copies it verbatim - its marker, then its bytes - so no value is
    // decoded and re-encoded on the way.
    std::vector<std::byte> lifted;
    serpent::container_sink out { lifted };
    bjdata::writer writer { out };
    bjdata::write_value(writer, document["beta"]);
    if (!writer.finish()) return 1;

    std::printf("lifted \"beta\" as %zu bytes: %s\n", lifted.size(), json::encode(bjdata::reader::over(lifted)).c_str());
    return 0;
}
