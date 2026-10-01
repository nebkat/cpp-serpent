#pragma once

// The document-level MessagePack API: encode a value, decode one, measure one, or splice one
// document into another - the same calls bjdata/document.hpp offers.

#include <serpent/msgpack/reader.hpp>
#include <serpent/msgpack/writer.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <cstddef>

namespace serpent::msgpack {

/** Copies a value into a writer with no re-encoding: its bytes, as they stand. */
inline void write_value(writer &out, const reader &source) noexcept {
    const auto bytes = source.encoded();
    if (!bytes) {
        out.fail(errc::type_mismatch);
        return;
    }
    out.encoded(*bytes);
}

/** Writes one value to a sink and reports the bytes produced, or the first failure. */
template<sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value) {
    writer target { out };
    target.value(value);
    return target.finish();
}

/** Encodes a value into a fresh buffer. The allocating convenience over write(). */
template<typename T>
[[nodiscard]] std::vector<std::byte> encode(const T &value) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    writer target { out };
    target.value(value);
    if (!target.finish()) buffer.clear();
    return buffer;
}

/** Decodes a value from a buffer, or nullopt when it does not parse. */
template<typename T>
[[nodiscard]] std::optional<T> decode(std::span<const std::byte> buffer) {
    return reader::over(buffer).as<T>();
}

/**
 * Decodes, saying why when it cannot: where the document stopped parsing, which member the type
 * needed and did not find, or errc::type_mismatch. A second pass over the bytes, as BJData's is.
 */
template<typename T>
[[nodiscard]] std::expected<T, error> try_decode(std::span<const std::byte> buffer) {
    if (const auto checked = validate(buffer); !checked) return std::unexpected { checked.error() };
    auto value = reader::over(buffer).as<T>();
    if (!value) {
        if (const auto absent = first_missing_member<T>(reader::over(buffer)); !absent.empty()) {
            return std::unexpected { error { errc::missing_key, 0, absent } };
        }
        return std::unexpected { error { errc::type_mismatch, 0 } };
    }
    return std::expected<T, error> { std::move(*value) };
}

/** The exact byte length a value would occupy. Allocates only where a container has to be gathered. */
template<typename T>
[[nodiscard]] std::size_t measure(const T &value) {
    counting_sink counter;
    writer target { counter };
    target.value(value);
    return target.finish().value_or(0);
}

} // namespace serpent::msgpack
