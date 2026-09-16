# Your types

Every form below ends up at the same place, so pick the shortest one that fits. The question
that decides it is usually whether you own the type: if you do, annotate it and let the
compiler do the rest; if you do not, name the fields yourself.

## Types you own

### Let the compiler name the fields

Where the compiler supports reflection, it already knows the field names and there is nothing
else to write:

```cpp
struct [[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};
```

This is the form to reach for, and the only one that also gives you
[discriminated variants](reflection.md#naming-a-type-on-the-wire). It needs GCC 16 with
`-freflection`.

A header compiled by more than one toolchain needs care: the annotation does not degrade to
nothing on a compiler that cannot parse it, it fails to compile. Carry both forms behind
`#if SERPENT_HAS_REFLECTION`, as `example/reflection.cpp` does.

The forms below exist because most compilers cannot do this yet — GCC 14 and 15 are what
the embedded toolchains ship, and neither has it. They are a compatibility path, not a
preference: reach for them when your compiler leaves you no choice.

### One function, both directions

When the body needs to do more than list members:

```cpp
struct segment {
    point start, end;
    std::string label;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), segment> value) {
        visitor.member("start", value.start);
        visitor.member("end", value.end);
        visitor.member("label", value.label);
    }
};
```

`visitor` is reading or writing, and `conversion_object_t` resolves to `segment &` or
`const segment &` to match. You write the field list once.

### Two functions, when the directions differ

Defaults on read, keys omitted on write, a value that is a string one way and a bool the
other:

```cpp
struct connection {
    std::string host = "localhost";
    std::optional<int> port {};

    friend void to_json(auto &out, const connection &value) {
        const auto scope = out.object();
        scope.member("host", value.host);
        if (value.port) scope.member("port", *value.port);   // (1)!
    }

    friend bool from_json(auto source, connection &value) {
        if (!source.is_object()) return false;
        const connection defaults {};
        value.host = source["host"].as_string().value_or(defaults.host);
        value.port = source["port"].template as_int<int>();
        return true;
    }
};
```

1. An empty optional writes no key at all, rather than `null`.

!!! tip "Keep the body format-blind"

    Say `out.value(x)` and let the writer decide what that means. BJData may pack a
    `std::vector<std::uint16_t>` into a typed array and copy it whole; JSON writes numbers.
    Neither your type nor its author needs to know.

## Types you do not own

You cannot annotate a type from someone else's header, and you cannot add a hidden friend to
it. Three ways in, shortest first.

### Opt it into reflection from outside

The trait says yes on the type's behalf, and may carry the naming rule the annotation would
have:

```cpp
template<>
struct serpent::enable_reflection<foreign_reading> : std::true_type {
    static constexpr serpent::naming_style style = serpent::naming_style::snake_case;
};
```

```json
{"sensor_id":4,"degrees_celsius":21.5}
```

Every field is included, so this fits a type whose fields you want as they are. There is no
way to rename or skip one from out here — for that, use a form below.

### The macro, beside the type

Name the members yourself, in the type's own namespace:

```cpp
SERPENT_DEFINE_TYPE_NON_INTRUSIVE(point, x, y)
```

### Specialize the serializer

Full control, including types that are not objects at all:

```cpp
template<>
struct serpent::serializer<timestamp, void> {
    template<typename Writer>
    static void write(Writer &out, const timestamp &value) { out.value(value.unix_seconds()); }

    template<typename Source>
    static bool read(Source source, timestamp &value) {
        const auto seconds = source.template try_get<std::int64_t>();
        if (!seconds) return false;
        value = timestamp::from_unix(*seconds);
        return true;
    }
};
```

## Only one at a time

A type may carry exactly one of these forms. If it is opted in to reflection **and** has a
hand-written conversion, that is a compile error rather than a silent ranking:

```
static assertion failed: this type is opted in to reflection and also has a
hand-written conversion. The hand-written one would be used and the annotation
would do nothing; remove whichever of the two you did not mean
```

The alternative would be worse: the annotation would sit on the type looking like it does
something while the hand-written form quietly won. Migrating a type therefore means replacing
one with the other in the same change, not adding the annotation and coming back later.

The one exception is a `serializer<T>` specialization. It replaces the dispatch entirely
rather than competing inside it, so it may coexist with an annotation and it wins — which is
what specializing it is for, and the only way to override a type whose definition you cannot
touch.

## What works out of the box

Arithmetic types, `bool`, `std::string` and string-likes, `std::optional`, `std::variant`, any
range, any keyed container with string keys, ranges of `std::byte` (as binary), and enums — which go out
as their underlying number unless they
[say otherwise](reflection.md#enumerations). None of these need opting in, in either direction.

## A field that can hold one of several types

`std::variant` needs no tag on the wire, because a value already says what it is:

```cpp
struct setting {
    std::string name;
    std::variant<bool, std::int64_t, double, std::string> value;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), setting> value) {
        visitor.member("name", value.name);
        visitor.member("value", value.value);
    }
};
```

```json
{"name":"threshold","value":2.5}
```

Writing emits whichever alternative is held. Reading asks each alternative, in declaration
order, whether the value fits, and the first that accepts it wins. `std::monostate` is `null`,
so `std::variant<std::monostate, T>` behaves like an optional that is explicitly present.

Two rules follow from "first that fits":

- **Declaration order is the tie-break.** `variant<double, std::int64_t>` reading `4` gives the
  double, because a whole number fits one. Put the more specific alternative first.
- **An object must name at least one of a type's members to be read as that type.** Decoding
  `{"radius":9}` into a `point` would otherwise succeed and leave every member at its default,
  which is right for a missing key but useless for telling alternatives apart. Inside a variant
  it is not a match.

Alternatives that are genuinely indistinguishable — two structs with the same field names —
cannot be resolved this way at all. Give them a name instead, with
[a discriminant](reflection.md#naming-a-type-on-the-wire):

```cpp
struct [[= serpent::discriminant("unit")]] celsius    { double value; };
struct [[= serpent::discriminant("unit")]] fahrenheit { double value; };
```

```json
{"unit":"fahrenheit","value":70.7}
```

The alternative is then chosen by what the document calls it rather than by what parses, and a
name that matches nothing fails instead of guessing.

Where the alternatives are not yours to annotate, tag the field instead — which also opts them
in, so neither type needs to know serpent exists:

```cpp
[[= serpent::tagged("kind")]] std::variant<point, circle> body;
```

Both need reflection; without it, order remains the only tie-break.

## Durations and times

`<serpent/chrono.hpp>`, included separately so `<chrono>` stays out of translation units that do
not want it, handles `std::chrono::duration`, `time_point` and `hh_mm_ss`:

```cpp
struct [[= serpent::serializable {}]] sample {
    std::chrono::milliseconds elapsed;
    std::chrono::sys_time<std::chrono::milliseconds> at;
};
```

```json
{"elapsed":1500,"at":1700000000000}
```

A duration travels as its count and nothing else. A time point is the duration since its clock's
epoch, and a time of day the duration it was built from, so both follow whatever the duration
does.

!!! warning "The unit is in the type, not on the wire"

    Nothing here can check that both ends named the same duration. Writing `milliseconds` and
    reading `seconds` gives you the same number under a different name — silently wrong by a
    factor of a thousand. Say which unit a field is in, in its name or in your schema, the way
    you would for any other bare number.

## Containers

Every container shape reads back in the shape it was written, and the two directions decide by
the same question so they cannot drift apart:

| | on the wire | read back by |
|---|---|---|
| `vector`, `deque`, `list` | an array | growing at the back |
| `set`, `unordered_set` | an array | inserting |
| `array`, and anything else of fixed size | an array | assigning the slots it already has |
| `map` with text keys | an object | by key |
| `map` with any other key | an array of two-element arrays | by pairs |
| `vector<byte>`, `array<byte, N>` | binary, where the format has it | in place |

A fixed-size sequence is its length: a document of another length is refused rather than filled
as far as it goes, since the rest would otherwise keep whatever a default-constructed one held.

## A map whose keys are not text

A document's keys are text, so a map whose key type is not travels as a sequence of two-element
arrays instead:

```cpp
std::map<uuid, calibration> table;     // [[key, value], [key, value], …]
std::map<std::string, int> named;      // {"x": 1}
```

Both sides decide by the same question — whether the key converts to a string — so a container
reads back in the shape it was written. A `std::pair` on its own is the same two-element array,
which is what makes the sequence form work for any key type that can be serialized at all.

## Missing and extra keys

A member the type needs has to be in the document. Keeping its default instead is how a
half-specified document passes for a whole one, and nothing downstream can tell the difference
between a field that said zero and a field that said nothing.

| | |
|---|---|
| a plain member is absent | **an error** |
| a `std::optional` member is absent | it is empty — absence is what the type represents |
| a member marked `[[= serpent::defaulted {}]]` is absent | it keeps the default it declared |
| an optional marked `[[= serpent::required {}]]` is absent | **an error** — the key must be stated, though its value may be null |
| a key is present that the type does not name | ignored |
| the keys are in a different order | read correctly, slightly slower |

So a format that gains a field can mark it `defaulted` and keep reading documents written before
it existed, and a type that must be fully specified gets that for nothing.

A whole type can say it, the way it says a naming rule — which is what a stored configuration
usually wants, since a field added in a later version is simply absent from every file written
before it, and such a file must still load rather than failing and losing every other setting
with it:

```cpp
struct [[= serpent::serializable {}, = serpent::defaulted {}]] user_config {
    std::string host = "localhost";
    int port = 8080;
    [[= serpent::required {}]] int version = 1;   // the exception
};
```

An optional is unaffected by the type-wide rule: absence is already what it represents, so only
`required {}` on the member itself changes anything there.

```cpp
struct [[= serpent::serializable {}]] calibration {
    double offset;                                   // must be there
    double scale;                                    // must be there
    [[= serpent::defaulted {}]] int revision = 1;    // added later; older documents may omit it
    std::optional<std::string> note;                 // may be absent or null
};
```

The check costs one bit set per member and one comparison per object — reflection knows how many
members there are, so the mask is a `std::uint64_t` and nothing is allocated or walked twice.

A hand-written `json_convert` gets the same guarantee from the same rule, without saying anything
extra: `member()` insists, and exempts a `std::optional` for the reason above. The member that may
be left out is the one that has to be spelled differently.

```cpp
friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), settings> value) {
    visitor.member("host", value.host);                   // must be there
    visitor.member_if_present("retries", value.retries);  // keeps whatever it already held
}
```

That way round on purpose: the strict answer is the one you get by not thinking about it, and
leniency is a thing you ask for by name. A `to_json`/`from_json` pair writes its own body and is
on its own, as it is for everything else.

A failed read names the member it wanted:

```cpp
const auto decoded = serpent::json::try_decode<calibration>(text);
if (!decoded && decoded.error().code() == serpent::errc::missing_key) {
    log("the document has no %s", decoded.error().key());
}
```

Naming it costs a second walk over the document, taken only on the way to reporting an error —
the generated reader knows a member was missing, not which one, and the check that told it that
costs a bit set per member. `decode()` skips all of it and returns nothing.

