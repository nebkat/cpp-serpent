// An annotation that cannot mean anything where it is must not compile - here serpent::required
// on a member that is not an optional, which is required already. Expected to fail to build.

#include <serpent/json.hpp>

namespace {

struct[[= serpent::serializable {}]] config {
    [[= serpent::required {}]] int port = 0;
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(config {}).size());
}
