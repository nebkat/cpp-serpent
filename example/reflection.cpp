// Letting the compiler name the fields, instead of a macro listing them.
//
// The reflected path needs C++26: P2996 for reflection, P1306 for `template for`, P3394 for
// the [[=value]] annotation syntax. No released toolchain has all three, so this example
// compiles the annotated form only when they are present and falls back to the macro
// otherwise - which is exactly how you would ship a type today.

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <cstdio>
#include <string>

namespace json = serpent::json;

#if SERPENT_HAS_REFLECTION

// Keys come from the identifiers, adjusted by the type's naming rule; a field can override
// its own, and one can be left out entirely.
struct[[= serpent::serializable]][[= serpent::naming { serpent::naming_style::snake_case }]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress = "0.0.0.0";
    [[= serpent::skip]] int cacheGeneration = 0;
    int mtuBytes = 1500;
};

#else

// The same type, written the way it has to be until a compiler catches up. The keys below are
// what the annotations above would have produced.
struct link_config {
    std::string ipAddress = "0.0.0.0";
    int cacheGeneration = 0;
    int mtuBytes = 1500;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), link_config> value) {
        visitor.member("ip", value.ipAddress);
        visitor.member("mtu_bytes", value.mtuBytes);
    }
};

#endif

int main() {
    std::printf("reflection available: %s\n\n", serpent::reflection_available ? "yes" : "no");

    const link_config config { .ipAddress = "10.0.0.4", .cacheGeneration = 7, .mtuBytes = 9000 };
    std::printf("%s\n\n", json::encode(config, { .indent = 2 }).c_str());

    const auto back = json::decode<link_config>(json::encode(config));
    if (!back || back->ipAddress != config.ipAddress || back->mtuBytes != config.mtuBytes) return 1;
    // cacheGeneration is not on the wire, so it comes back at its default rather than 7.
    if (back->cacheGeneration != 0) return 1;

    // The identifier-to-key conversion is ordinary constexpr code and runs whether or not the
    // compiler can reflect, so the naming rules can be relied on either way.
    using serpent::naming_style;
    using serpent::detail::convert_case;
    static_assert(convert_case("mtuBytes", naming_style::snake_case).view() == "mtu_bytes");
    static_assert(convert_case("mtu_bytes", naming_style::camel_case).view() == "mtuBytes");
    static_assert(convert_case("mtu_bytes", naming_style::pascal_case).view() == "MtuBytes");
    static_assert(convert_case("mtuBytes", naming_style::kebab_case).view() == "mtu-bytes");
    static_assert(convert_case("mtuBytes", naming_style::screaming_snake_case).view() == "MTU_BYTES");
    std::printf("naming rules hold at compile time either way\n");
    return 0;
}
