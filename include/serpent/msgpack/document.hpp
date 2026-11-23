#pragma once

// The document-level MessagePack API: encode a value, decode one, measure one, or splice one
// document into another - the same calls bjdata/document.hpp offers.

#include <serpent/msgpack/reader.hpp>
#include <serpent/msgpack/writer.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>
#include <serpent/locate.hpp>

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

namespace detail {

/** Whether a document that has been read ends exactly where its bytes do; as BJData's does. */
[[nodiscard]] inline bool ends_with_buffer(const reader &root, std::span<const std::byte> buffer) noexcept {
    return root.value_end() == buffer.data() + buffer.size();
}

} // namespace detail

/**
 * Decodes a value from a buffer, or says why it cannot.
 *
 * The whole buffer is the document: one that ends early, breaks partway or has bytes after it is
 * refused, as validate() would refuse it. Checked as it is read, so a document that decodes is
 * read once; only one that does not is walked again, to say why.
 *
 * A document that does not parse is reported with the kind and the byte offset validate()
 * found; one that parses but does not fit the type is errc::type_mismatch, or errc::missing_key
 * naming a key the root object lacks. With `options.trace` the error also has the path(), offset
 * and found() kind of the value at fault, and a missing key is found at any depth.
 */
template<typename T>
[[nodiscard]] std::expected<T, error> decode(std::span<const std::byte> buffer, decode_options options = {}) {
    const auto root = reader::over(buffer);
    auto value = root.as<T>();
    if (value && detail::ends_with_buffer(root, buffer)) return std::expected<T, error> { std::move(*value) };
    if (const auto checked = validate(buffer); !checked) return std::unexpected { checked.error() };
    if (!value) return std::unexpected { serpent::detail::explain_mismatch<T>(reader::over(buffer), options) };
    // Read, and valid, yet not ending where the buffer does: no walk can disagree with
    // validate() about where a value ends, so this is only here to be total.
    return std::unexpected { error { errc::trailing_data, 0 } };
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
