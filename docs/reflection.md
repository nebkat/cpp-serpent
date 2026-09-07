# Reflection

Let the compiler enumerate the fields, instead of a macro listing them.

```cpp
struct [[= serpent::serializable]]
       [[= serpent::naming { serpent::naming_style::snake_case }]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress;
    [[= serpent::skip]]      int cacheGeneration;
                             int mtuBytes;          // becomes "mtu_bytes"
};
```

| Annotation | On | Does |
|---|---|---|
| `serializable` | a type | opts it in |
| `naming{style}` | a type | derives every key from the identifiers |
| `key("...")` | a field | overrides one key |
| `skip` | a field | leaves it out |

Styles: `as_written`, `snake_case`, `screaming_snake_case`, `kebab_case`, `camel_case`,
`pascal_case`.

Annotations are ordinary values, not parsed strings, and are always written qualified.

!!! danger "Not available yet"

    This needs three C++26 papers — P2996 (reflection), P1306 (`template for`) and P3394
    (annotations) — and **no released toolchain implements all three**. The binding to
    `std::meta` follows the papers rather than a working compiler, so expect to adjust
    spellings. Check `serpent::reflection_available` at compile time.

Everything that does **not** need reflection sits outside the gate and is tested, so the
naming rules can be relied on today:

```cpp
using serpent::detail::convert_case;
using serpent::naming_style;

static_assert(convert_case("mtuBytes", naming_style::snake_case).view() == "mtu_bytes");
static_assert(convert_case("mtu_bytes", naming_style::camel_case).view() == "mtuBytes");
```

Opting in is deliberate. Reflecting every aggregate that merely lacks a `json_convert` would
turn any struct that happens to be serializable into a wire-format commitment, silently.

Acronyms are a known limitation: `IPAddress` converts to `ipaddress`, not `ip_address`.

See `example/reflection.cpp` for the annotated form beside the fallback every toolchain needs
today.
