#pragma once

// decode() and try_decode(): a whole document into one of your types.

#include <serpent/json/reader.hpp>

#include <expected>
#include <optional>
#include <string_view>

namespace serpent::json {

/** Decodes a value from JSON text. */
template<typename T>
[[nodiscard]] std::optional<T> decode(std::string_view text) {
    return reader::over(text).template as<T>();
}

/**
 * Decodes into a value that already exists, which then keeps whatever capacity its containers
 * had - for reading many documents into the one object. A member the document leaves out keeps
 * its value, as it does for any read into an existing object; whether the document fits the type
 * is the answer.
 */
template<typename T>
bool decode_into(std::string_view text, T &value) {
    return read_into(reader::over(text), value);
}

/**
 * Decodes, saying why when it cannot.
 *
 * A document that does not parse is reported with the kind and the byte offset validate() found;
 * one that parses but does not fit the type is errc::type_mismatch, unless a member the type
 * needed was missing, which is named.
 */
template<typename T>
[[nodiscard]] std::expected<T, error> try_decode(std::string_view text) {
    if (const auto checked = validate(text); !checked) return std::unexpected { checked.error() };
    auto value = reader::over(text).template as<T>();
    if (!value) {
        if (const auto absent = first_missing_member<T>(reader::over(text)); !absent.empty()) {
            return std::unexpected { error { errc::missing_key, 0, absent } };
        }
        return std::unexpected { error { errc::type_mismatch, 0 } };
    }
    return std::expected<T, error> { std::move(*value) };
}

} // namespace serpent::json
