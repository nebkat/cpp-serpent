// A table that has fallen behind the enumeration it is for must not compile.
//
// The case this exists for is an SDK upgrade adding an enumerator: without the check the new
// one reads and writes as the fallback and nothing says so. Expected to fail to build - the
// diagnostic is the thing under test.

#include <serpent/json.hpp>

namespace {

// As a foreign header would declare it, with an enumerator the table below does not know about.
enum parity { parity_none = 0x0, parity_even = 0x2, parity_odd = 0x3, parity_mark = 0x4 };

} // namespace

template<>
struct serpent::enum_values<parity> {
    static constexpr serpent::enum_entry<parity> values[] {
        { parity_none, "none", serpent::fallback {} },
        { parity_even, "even" },
        { parity_odd, "odd" },
    };
};

int main() {
    return static_cast<int>(serpent::json::encode(parity_even).size());
}
