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
| `discriminant("key")` | a type | names it on the wire under `key`, so a variant can select it |
| `tagged("key")` | a variant field | the same, decided at the field instead of on the alternatives |

Styles: `as_written`, `snake_case`, `screaming_snake_case`, `kebab_case`, `camel_case`,
`pascal_case`.

Annotations are ordinary values rather than parsed strings, which is why they are written with
`=` and braces, and always qualified.

## Naming a type on the wire

A `std::variant` is read by asking each alternative whether the value fits, which cannot
separate two types that have the same members. A discriminant gives the type a name instead:

```cpp
struct [[= serpent::discriminant("unit")]] celsius    { double value; };
struct [[= serpent::discriminant("unit")]] fahrenheit { double value; };
struct [[= serpent::discriminant("unit", "K")]] kelvin { double value; };

using temperature = std::variant<celsius, fahrenheit, kelvin>;
```

```json
{"unit":"fahrenheit","value":70.7}
```

The name defaults to the type's own identifier, which reflection already knows; the second
argument overrides it. It is **not a member** — nothing declares it, reading a type on its own
ignores it, and it is written before the fields.

Decoding a variant then reads that key and picks the alternative it names. A name that matches
nothing fails, rather than falling back to a guess.

### Tagging at the field instead

A discriminant on the type needs the alternatives to be yours to annotate. Where they are not,
put the tag on the field:

```cpp
struct point  { int x, y; };      // someone else's header
struct circle { int radius; };

struct [[= serpent::serializable {}]] drawing {
    std::string title;
    [[= serpent::tagged("kind")]] std::variant<point, circle> body;
};
```

```json
{"title":"a","body":{"kind":"circle","radius":9}}
```

Naming the alternatives here is also what opts them in, so neither type needs to know serpent
exists. They are named by their own identifiers unless you say otherwise:

```cpp
[[= serpent::tagged("k", { "pt", "circ" })]] std::variant<point, circle> body;
```

Because the tag belongs to the field, the same types can be tagged differently in different
places, which a type-level annotation cannot do.

### Alternatives that are not objects

Only an alternative written as an object of its own members can carry a name — there is nowhere
to put one on a number. Those keep the untagged behaviour, which is all they need:

```cpp
[[= serpent::tagged("kind")]] std::variant<int, point, circle> body;
```

```json
{"body":42}
{"body":{"kind":"circle","radius":9}}
```

A number is recovered as a number, and the objects are still told apart by name. This holds for
the type-level `discriminant` too: alternatives that cannot carry one do not stop the ones that
can from being named. A name that matches no alternative still fails, rather than falling
through to the untagged attempt.

## A type you cannot annotate

You cannot put an annotation on someone else's type, so the opt-in is a trait instead. It
carries the naming rule as well, because with no conversion function of your own there is
nowhere else for a type-level setting to live — the trait is the only surface you control:

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
    // a json_convert naming the fields
#endif
```

`serpent::reflection_available` is the same answer as a `constexpr bool`. `example/reflection.cpp`
shows a type written both ways.

!!! warning "The annotation is not ignorable, so a shared header needs the guard"

    On a compiler that does not know the syntax the annotation is a **parse error**, not an
    attribute that is quietly dropped:

    ```
    error: expected ']' before '=' token
    ```

    So a header compiled by more than one toolchain cannot simply carry the annotation and let
    older compilers skip it — it has to carry both forms behind `#if SERPENT_HAS_REFLECTION`.
    Verified against GCC 15.2 (the ESP-IDF v6.1 toolchain), which rejects it, and GCC 16.1,
    which accepts it.

    GCC 16 **without** `-freflection` is the case to watch: it parses the annotation and then
    ignores it, so the type is not reflected and the first thing to complain is the converter,
    at the point of use. The message says so, but check `-freflection` is really on the command
    line before believing the type is at fault.

## Reflection and a hand-written conversion are exclusive

Annotating a type that already has a `json_convert`, or a `to_json` / `from_json` pair, is a
compile error. Preferring one silently would leave the annotation on the type doing nothing,
so it is diagnosed instead. Replace one form with the other in the same change.

A `serializer<T>` specialization is the exception — it replaces the dispatch outright, so it
may sit alongside an annotation and it wins.

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
