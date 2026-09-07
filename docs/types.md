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

## Which one wins

A type can carry more than one of these — annotating a type that already has a `json_convert`
is the normal way to migrate it. They resolve in a fixed order, and the first that exists wins:

1. a `serpent::serializer<T>` specialization
2. a `json_convert` — the macro forms expand to one
3. a `to_json` / `from_json` pair
4. reflection

So a hand-written conversion always beats the reflected one, and adding an annotation to a
type that already has one changes nothing until you delete the hand-written form.

## What works out of the box

Arithmetic types, `bool`, `std::string` and string-likes, `std::optional`, any range, any
keyed container with string keys, ranges of `std::byte` (as binary), and enums. None of these
need opting in, in either direction.

## Missing and extra keys

| Situation | Result |
|---|---|
| Key absent from the document | the member keeps whatever it already held |
| Key present that your type does not name | ignored |
| Keys in a different order | still read correctly, slightly slower |
