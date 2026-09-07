// A type that is annotated for reflection and also carries a hand-written conversion.
//
// This must NOT compile: the hand-written one would be used and the annotation would quietly
// do nothing. Built by ctest as an expected failure, so the diagnostic itself is tested.

#include <serpent/json.hpp>

struct[[= serpent::serializable {}]] contradictory {
    int x = 1;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), contradictory> value) {
        visitor.member("x", value.x);
    }
};

int main() { return serpent::json::encode(contradictory {}).empty() ? 1 : 0; }
