# Your types

There are three ways to opt a type in. They all end up at the same place, so pick the shortest
one that fits.

## The macro

For a plain aggregate, list the members:

```cpp
struct point {
    int x = 0;
    int y = 0;

    SERPENT_DEFINE_TYPE(point, x, y)
};
```

Outside the type, when you cannot edit it:

```cpp
SERPENT_DEFINE_TYPE_NON_INTRUSIVE(point, x, y)
```

## One function, both directions

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

## Two functions, when the directions differ

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

## What works out of the box

Arithmetic types, `bool`, `std::string` and string-likes, `std::optional`, any range, any
keyed container with string keys, ranges of `std::byte` (as binary), and enums.

For a type you cannot add functions to, specialise `serpent::serializer<T>`.

## Missing and extra keys

| Situation | Result |
|---|---|
| Key absent from the document | the member keeps whatever it already held |
| Key present that your type does not name | ignored |
| Keys in a different order | still read correctly, slightly slower |
