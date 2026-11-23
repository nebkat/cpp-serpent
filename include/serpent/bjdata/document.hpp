#pragma once

// The document-level BJData API: encode a value, decode a value, measure one, or splice one
// document into another. These name the BJData writer and reader, so they live here rather
// than in serial/, which knows about neither.

#include <serpent/bjdata/reader.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>
#include <serpent/locate.hpp>

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

namespace detail {

/**
 * Whether a document that has been read ends exactly where its bytes do. A container cut short
 * or broken partway leaves its walk short of its end, so this is what turns "the elements ran
 * out" into "the document is broken" - and trailing bytes into a refusal rather than nothing.
 */
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
