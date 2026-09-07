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

This is the shortest form and the one to reach for first. It needs GCC 16 with `-freflection`
— see [Reflection](reflection.md) for the naming rules and for what to do on compilers that
are not there yet. Everything below works everywhere.

### The macro

For a plain aggregate, list the members:

```cpp
struct point {
    int x = 0;
    int y = 0;

    SERPENT_DEFINE_TYPE(point, x, y)
};
```

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
range, any keyed container with string keys, ranges of `std::byte` (as binary), and enums. None
of these need opting in, in either direction.

## A field that can hold one of several types

`std::variant` needs no tag on the wire, because a value already says what it is:

```cpp
struct setting {
    std::string name;
    std::variant<bool, std::int64_t, double, std::string> value;

    SERPENT_DEFINE_TYPE(setting, name, value)
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

Alternatives that are genuinely indistinguishable — two structs with the same field names — are
resolved by order, and nothing can do better without a tag you put there yourself.

## Missing and extra keys

| Situation | Result |
|---|---|
| Key absent from the document | the member keeps whatever it already held |
| Key present that your type does not name | ignored |
| Keys in a different order | still read correctly, slightly slower |
