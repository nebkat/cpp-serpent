// Two enumerators that are the same value on the wire must not compile: whichever was written,
// only one of them could ever be read back. Expected to fail to build.

#include <serpent/json.hpp>

namespace {

enum class[[= serpent::serializable {}]] mode {
    off [[= serpent::as("idle")]],
    standby [[= serpent::as("idle")]],
    running,
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(mode::off).size());
}
