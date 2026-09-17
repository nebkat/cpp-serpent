#pragma once

// decode() and try_decode(), which own the whole of a read and so keep a memo of their own -
// isolating the traversal from anything else the thread is reading at the same time.

#include <serpent/json/reader.hpp>

#include <expected>
#include <optional>
#include <string_view>

namespace serpent::json {

/**
 * Decodes a value from JSON text.
 *
 * Read through a walking handle, which leaves a note of how far each traversal got so the step
 * to the next value resumes rather than restarting. Nothing about the result changes; a document
 * that decodes one way decodes the same the other, which the suite asserts.
 */
template<typename T>
[[nodiscard]] std::optional<T> decode(std::string_view text) {
    walk_memo memo;
    return reader::over(text, memo).template try_get<T>();
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
    walk_memo memo;
    auto value = reader::over(text, memo).template try_get<T>();
    if (!value) {
        if (const auto absent = first_missing_member<T>(reader::over(text)); !absent.empty()) {
            return std::unexpected { error { errc::missing_key, 0, absent } };
        }
        return std::unexpected { error { errc::type_mismatch, 0 } };
    }
    return std::expected<T, error> { std::move(*value) };
}

} // namespace serpent::json
