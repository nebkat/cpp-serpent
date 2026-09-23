// enum_as_name on a field that holds no enumeration cannot mean anything. Expected to fail to
// build.

#include <serpent/json.hpp>

namespace {

struct[[= serpent::serializable {}]] status {
    [[= serpent::enum_as_name {}]] int count = 0;
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(status {}).size());
}
