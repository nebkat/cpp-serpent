// Two members that are the same key on the wire must not compile. One would overwrite the
// other reading, and both would be written. Expected to fail to build.

#include <serpent/json.hpp>

namespace {

struct[[= serpent::serializable {}]] listener {
    [[= serpent::key("port")]] int listen_port = 1;
    int port = 2;
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(listener {}).size());
}
