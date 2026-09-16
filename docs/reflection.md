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
| `required {}` | an optional field | the key must be present, though its value may be null |
| `defaulted {}` | a plain field | the document may omit it, and it keeps its declared default |
| `discriminant("key")` | a type | names it on the wire under `key`, so a variant can select it |
| `tagged("key")` | a variant field | the same, decided at the field instead of on the alternatives |
| `as(...)` | an enumerator | what it is on the wire, where its identifier will not do |
| `fallback {}` | an enumerator | the one to use when nothing matches, in both directions |

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

## Enumerations

An enumeration that says nothing goes out as its underlying number. Annotate it and the
enumerators go by their identifiers instead:

```cpp
enum class [[= serpent::serializable {}]] nav_system { unknown, gps, glonass };
```

```json
"glonass"
```

Where an identifier is not what the wire calls it, say so on the enumerator. Note the position:
an enumerator's attribute follows its name.

```cpp
enum class [[= serpent::serializable {}]] fix_dimension {
    none              [[= serpent::fallback {}, = serpent::as(nullptr)]],
    two_dimensional   [[= serpent::as("2d")]],
    three_dimensional [[= serpent::as("3d")]],
};
```

`as` takes null, a boolean, a whole number, a real or a string, and they may be **mixed within
one enumeration** — a stop-bit count that is `1`, `1.5` and `2` is written exactly so, and a
word length maps to `5`, `6`, `7`, `8` regardless of its underlying values. The type's
[naming rule](#adjusting-the-keys) applies to the identifiers it falls back on, so
`notConnected` under `kebab_case` is `"not-connected"`.

`fallback {}` names one enumerator as the answer when nothing else fits, in **both** directions:
a value on the wire matching no enumerator reads as it, and an enumeration value that is not any
enumerator — cast in from a number — writes as it. Without one, an unrecognised value fails to
read rather than guessing.

### Absent, null, unrecognised

Four things a document can do to an enumeration member, and they stay four:

| The document | The member gets |
|---|---|
| leaves the key out | whatever its own initialiser said, if it may be left out at all |
| says a value the enumeration names | that enumerator |
| says a value it does not name | the `fallback {}`, or the read fails if there is none |
| says `null` | an enumerator annotated `as(nullptr)`, else the `fallback {}`, else the read fails |

The default is at the member and the fallback is on the enumeration, which is the right way
round: the default is what *this field* means when unsaid, so two structs holding the same
enumeration may disagree about it, and it lives in the initialiser where a C++ programmer looks
for it.

```cpp
struct [[= serpent::serializable {}]] listener {
    [[= serpent::defaulted {}]] auth_method auth = auth_method::hotspot;
};
```

!!! note "Null is a value the table does not name, so a fallback catches it"

    Only a fallback does. Without one, `null` fails the read exactly as a plain `int` member
    does when the document says `null` — serpent's answer for a field where null is *meaningful*
    is `std::optional`, which reads it as empty and needs no enumerator spent on it.

    So a fallback is a total function from every wire value to an enumerator, null included.
    If you want null to mean one enumerator and unrecognised values another, say
    `as(nullptr)` on the one null means; it is matched before the fallback is considered.

Two enumerations may give the same spelling different meanings, which a rule derived from the
identifiers could not: `rtk_float` is `"float"` in one and `"rtk_float"` in another.

## An enumeration you cannot annotate

An annotation cannot go on an enumeration declared in someone else's header, so the table goes
in a specialization instead. It carries exactly what the annotations carry — a value per
enumerator, of any of the kinds `as` takes, and one fallback:

```cpp
template<>
struct serpent::enum_values<uart_stop_bits_t> {
    static constexpr serpent::enum_entry<uart_stop_bits_t> values[] {
        { UART_STOP_BITS_1,   1,   serpent::fallback {} },
        { UART_STOP_BITS_1_5, 1.5 },
        { UART_STOP_BITS_2,   2   },
        { UART_STOP_BITS_MAX, serpent::skip {} },   // a sentinel, never on the wire
    };
};
```

Both forms run the same lookup, so null, the mixed kinds and the fallback behave identically
whichever way an enumeration was mapped. **The table needs no reflection at all** — it is the
way to map an enumeration on a toolchain that has none.

!!! tip "A table is held to its type"

    Where reflection *is* available, the table is checked against the enumerators the compiler
    can see, and one it does not name is a compile error naming the one it missed:

    ```
    error: static assertion failed: this serpent::enum_values table does not name every
    enumerator of its type. Not named: UART_PARITY_MARK. Give each a value, or say
    { enumerator, serpent::skip {} } to keep it off the wire deliberately
    ```

    Ownership only blocks *attaching* an annotation to an enumerator; it does not block
    enumerating them. So an enumerator added by an SDK upgrade is caught at the build that
    picks it up, rather than quietly reading and writing as the fallback.

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

## What the compiler refuses to let you get wrong

The list of members, and what each one is called on the wire, is knowable where reflection is —
and nowhere else. So a mistake about the relationship between a type and its document is a build
error rather than something you find by reading a document that came out wrong:

| Refused | Because |
|---|---|
| two members that are the same key | one would overwrite the other reading, and both would be written |
| an enumeration with two enumerators of the same wire value | whichever was written, only one could ever be read back |
| more than one `fallback {}` | nothing says which |
| a `serpent::enum_values` table that does not name every enumerator | the one it missed would quietly become the fallback |
| `required {}` on a member that is not an optional | it is required already; the annotation says nothing |
| `required {}` and `defaulted {}` together | they are opposites |
| `skip {}` beside `key("…")` | a member that is not in the document has no key |
| `as(…)` or `naming {…}` on a data member | both belong somewhere else |

Each names what it found:

```
error: static assertion failed: two members of this type are the same key on the wire: port.
One would overwrite the other reading, and both would be written; rename one with
serpent::key, or leave one out with serpent::skip
```

```
error: static assertion failed: this enumeration cannot be read back as it is written:
off and standby are the same value on the wire
```

Two of these hold without reflection, because a `serpent::enum_values` table is ordinary data:
duplicate entries and a second fallback are refused wherever the table compiles. The rest need
the member or enumerator list, so **on a toolchain without reflection they are simply absent** —
the code still builds and the mistake still ships. Build once with reflection somewhere, and it
is caught.

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
