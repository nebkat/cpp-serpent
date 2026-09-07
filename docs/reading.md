# Reading

## Two handles, one shape

| | |
|---|---|
| `bjdata::view` | a **view** — values *are* the bytes, so strings and arrays are borrowed |
| `json::reader` | a **reader** — values must be constructed, so strings are decoded |

The difference is the format's, not a naming choice. Everything else is the same: the same
`errc`, the same accessors, the same forward iterators.

```cpp
auto document = bjdata::view::over(bytes);      // or json::reader::over(text)

document["name"];                 // missing key gives an invalid handle, not an error
document["ports"][2];
for (auto element : document["ports"].array()) { }
for (auto [key, value] : document.items()) { }
```

## Three tiers over one parser

=== "Total — never throws"

    ```cpp
    auto label = document["meta"]["label"].as_string().value_or("unnamed");
    ```

    A missing key, a wrong type, or a corrupt document all yield nothing. Traversal of an
    invalid handle stays invalid, so there is no per-step checking.

=== "Checked — throws"

    ```cpp
    try {
        auto label = document.at("meta").at("label").string();
    } catch (const serpent::error &failure) {
        std::println("{} at byte {}", failure.what(), failure.offset());
    }
    ```

=== "Validating — once, up front"

    ```cpp
    if (auto problem = bjdata::validate(bytes); !problem) {
        std::println("{} at byte {}", problem.error().what(), problem.error().offset());
    }
    ```

!!! note "Safe either way"

    Every access is bounds-checked against the buffer, so walking a truncated or corrupt
    document is safe even without `validate()`. Nesting is capped, so a hostile document
    cannot exhaust the stack.

## Accessors

```cpp
value.type();          // null, boolean, integer, real, string, array, object, invalid
value.is_array();
value.size();

value.as_bool();
value.as_int<std::uint16_t>();     // range-checked, not truncated
value.as_float<double>();
value.as_string();
value.as_binary();                  // BJData only
value.as_span<std::uint16_t>();     // BJData only
```

## Strings

```cpp
// BJData: the value IS the bytes
std::optional<std::string_view> borrowed = document["name"].as_string();

// JSON: escapes mean the value must be built
std::optional<std::string> decoded = reader["name"].as_string();
std::string into;
reader["name"].read_string_into(into);
reader["name"].decode_string_into(buffer);   // no allocation; refuses to truncate
reader["name"].string_is("expected");        // compares without materialising
```

!!! warning "No conditional borrowing"

    JSON never hands back a `string_view` that happens to work when the data has no escapes.
    An API whose shape depends on its contents passes testing and fails in the field.
