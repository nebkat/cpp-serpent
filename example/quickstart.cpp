// One definition per type, carried by both formats in both directions.

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

struct[[= serpent::serializable {}]] reading {
    std::uint32_t at = 0;
    double celsius = 0;
};

struct[[= serpent::serializable {}]] station {
    std::string name;
    std::optional<int> altitude;
    std::vector<reading> readings;
};

int main() {
    const station original {
        .name = "north ridge",
        .altitude = 1840,
        .readings = { { 1700000000, 4.5 }, { 1700000060, 4.25 }, { 1700000120, 3.75 } },
    };

    // Nothing here says which format the type supports. It supports both.
    const auto bytes = bjdata::encode(original);
    const auto text = json::encode(original, { .indent = 2 });

    std::printf("BJData: %zu bytes\n", bytes.size());
    std::printf("JSON:\n%s\n\n", text.c_str());

    const auto from_bytes = bjdata::decode<station>(bytes);
    const auto from_text = json::decode<station>(text);
    if (!from_bytes || !from_text) return 1;

    std::printf("round trip: %s, %zu readings, altitude %d\n", from_bytes->name.c_str(), from_bytes->readings.size(),
            from_bytes->altitude.value_or(-1));

    // The two agree, which is the point of one definition serving both.
    if (json::encode(*from_bytes) != json::encode(*from_text)) return 1;

    // Sizing costs nothing but a walk, so a buffer can be reserved exactly.
    std::printf("measured %zu bytes before writing any\n", bjdata::measure(original));
    return 0;
}
