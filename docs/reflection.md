# Reflection

The compiler already knows what your fields are called. Where it can tell you, serpent uses
that and you write no conversion code at all.

```cpp
struct [[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};

json::encode(point { 3, 4 });   // {"x":3,"y":4}
```

That is the whole opt-in. The same type now reads and writes in every format.

## Adjusting the keys

Identifiers are rarely the keys you want on the wire, so a type can carry a naming rule and a
field can override or opt out of its own:

```cpp
struct [[= serpent::serializable {},
       = serpent::naming { serpent::naming_style::snake_case }]] link_config {
    [[= serpent::key("ip")]] std::string ipAddress = "0.0.0.0";
    [[= serpent::skip {}]]   int cacheGeneration = 0;
                             int mtuBytes = 1500;   // becomes "mtu_bytes"
};
```

```json
{"ip":"10.0.0.4","mtu_bytes":9000}
```

| Annotation | On | Does |
|---|---|---|
| `serializable {}` | a type | opts it in |
| `naming {style}` | a type | derives every key from the identifiers |
| `key("...")` | a field | overrides one key |
| `skip {}` | a field | leaves it out of the document, both directions |

Styles: `as_written`, `snake_case`, `screaming_snake_case`, `kebab_case`, `camel_case`,
`pascal_case`.

Annotations are ordinary values rather than parsed strings, which is why they are written with
`=` and braces, and always qualified.

## A type you cannot annotate

You cannot put an annotation on someone else's type, so the opt-in is a trait instead. It can
carry the naming rule too:

```cpp
template<>
struct serpent::enable_reflection<foreign_reading> : std::true_type {
    static constexpr serpent::naming_style style = serpent::naming_style::snake_case;
};
```

```json
{"sensor_id":4,"degrees_celsius":21.5}
```

Every field is included and none can be renamed individually, because `key` and `skip` go on
the fields. When you need that much control over a type you do not own, write the conversion
by hand — see [Types you do not own](types.md#types-you-do-not-own).

## Requirements

Reflection needs three C++26 papers together — P2996 (reflection), P1306 (`template for`) and
P3394 (annotations). **GCC 16 has all three**, behind a flag:

```
-std=c++26 -freflection
```

No other released compiler has the full set yet. The CMake build probes for it and builds the
reflected tests only where it works, so a project can carry annotated types before every
compiler it targets has caught up:

```cpp
#if SERPENT_HAS_REFLECTION
    // annotated definition
#else
    // SERPENT_DEFINE_TYPE, or a json_convert
#endif
```

`serpent::reflection_available` is the same answer as a `constexpr bool`. `example/reflection.cpp`
shows a type written both ways.

## A hand-written conversion always wins

Reflection is the last thing tried, after a `serializer<T>` specialization, a `json_convert`
and a `to_json` / `from_json` pair. Annotating a type that already has one of those changes
nothing until you delete the hand-written form, which makes adding the annotation a safe first
step rather than an ambiguity.

## Why opting in is deliberate

Reflecting every aggregate that merely lacks a `json_convert` would turn any struct that
happens to be serializable into a wire-format commitment, silently. Adding a field would
change the wire; so would reordering. Annotating the type makes that a decision.

## Limitations

- Acronyms convert naively: `IPAddress` becomes `ipaddress`, not `ip_address`. Use
  `key("ip_address")` where it matters.
- A `key` is capped at 63 characters, because an annotation's type has to be structural and so
  stores an array rather than a pointer.
- Only non-static data members are enumerated, in declaration order.

The naming rules themselves are ordinary constexpr code and are compiled and tested
everywhere, reflection or not:

```cpp
using serpent::detail::convert_case;
using serpent::naming_style;

static_assert(convert_case("mtuBytes", naming_style::snake_case).view() == "mtu_bytes");
static_assert(convert_case("mtu_bytes", naming_style::camel_case).view() == "mtuBytes");
```
