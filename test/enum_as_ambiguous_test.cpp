// A field's entries must not make two enumerators the same value on the wire - here idle is
// renamed to what lost is already called. Expected to fail to build.

#include <serpent/json.hpp>

namespace {

enum class probe_state { idle, lost };

struct[[= serpent::serializable {}]] status {
    [[= serpent::enum_as_name { serpent::enum_entry { probe_state::idle, "lost" } }]] probe_state probe {};
};

} // namespace

int main() {
    return static_cast<int>(serpent::json::encode(status {}).size());
}
