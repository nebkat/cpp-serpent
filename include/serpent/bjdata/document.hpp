#pragma once

// The document-level BJData API: encode a value, decode a value, measure one, or splice one
// document into another. These name the BJData writer and reader, so they live here rather
// than in serial/, which knows about neither.

#include <serpent/bjdata/reader.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <cstddef>

namespace serpent::bjdata {

/** Copies a value into a writer with no re-encoding: its marker, then its bytes. */
template<prefer Preference>
void write_value(basic_writer<Preference> &out, const reader &source) noexcept {
    const auto payload = source.payload_bytes();
    if (!payload) {
        out.fail(errc::type_mismatch);
        return;
    }
    out.put_marker(source.type_marker());
    out.put(*payload);
}

/**
 * Writes one value to a sink and reports the bytes produced, or the first failure.
 *
 * What is preferred is a template argument: bjdata::write<prefer::speed>(sink, value).
 */
template<prefer Preference = prefer::size, sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value, writer_options options = {}) {
    basic_writer<Preference> target { out, options };
    target.value(value);
    return target.finish();
}

/** Encodes a value into a fresh buffer. The allocating convenience over write(). */
template<prefer Preference = prefer::size, typename T>
[[nodiscard]] std::vector<std::byte> encode(const T &value, writer_options options = {}) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    basic_writer<Preference> target { out, options };
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
 * Decodes, saying why when it cannot.
 *
 * A document that does not parse is reported with the kind and the byte offset validate()
 * found; one that parses but does not fit the type is errc::type_mismatch, which carries no
 * offset because nothing on the way in recorded where the shape stopped matching.
 *
 * It costs a second pass over the bytes, since validating and decoding are separate walks.
 * decode() is the one to use when the answer is all you want.
 */
template<typename T>
[[nodiscard]] std::expected<T, error> try_decode(std::span<const std::byte> buffer) {
    if (const auto checked = validate(buffer); !checked) return std::unexpected { checked.error() };
    auto value = reader::over(buffer).as<T>();
    if (!value) {
        // A member the type needed and the document left out is the one mismatch that can say
        // something specific, so it is worth the walk back over the document to name it.
        if (const auto absent = first_missing_member<T>(reader::over(buffer)); !absent.empty()) {
            return std::unexpected { error { errc::missing_key, 0, absent } };
        }
        return std::unexpected { error { errc::type_mismatch, 0 } };
    }
    return std::expected<T, error> { std::move(*value) };
}

/** The exact byte length a value would occupy, with no allocation. */
template<prefer Preference = prefer::size, typename T>
[[nodiscard]] std::size_t measure(const T &value, writer_options options = {}) {
    counting_sink counter;
    basic_writer<Preference> target { counter, options };
    target.value(value);
    // finish() both hands over the last batch and reports the total, so the sink is only there
    // to have somewhere for the bytes to go.
    return target.finish().value_or(0);
}

} // namespace serpent::bjdata
