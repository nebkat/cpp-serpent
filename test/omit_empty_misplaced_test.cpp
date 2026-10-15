// serpent::omit_empty on a member that is not an optional, which is never empty, must not compile.
// Expected to fail to build.

#include <serpent/json.hpp>

namespace {

struct[[= serpent::serializable {}]] config {
    [[= serpent::omit_empty {}]] int port = 0;
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(config {}).size());
}
