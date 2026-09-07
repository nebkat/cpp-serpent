#pragma once

// The document-level BJData API: encode a value, decode a value, measure one, or splice one
// document into another. These name the BJData writer and view, so they live here rather
// than in serial/, which knows about neither.

#include <serpent/bjdata/view.hpp>
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
inline void write_value(writer &out, view source) noexcept {
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
 * The compaction options are a template argument, so a build that turns one off does not
 * carry its code: bjdata::write<no_compaction>(sink, value).
 */
template<writer_options Options = writer_options {}, sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value) {
    basic_writer<Options> target { out };
    target.value(value);
    return target.finish();
}

/** Encodes a value into a fresh buffer. The allocating convenience over write(). */
template<writer_options Options = writer_options {}, typename T>
[[nodiscard]] std::vector<std::byte> encode(const T &value) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    basic_writer<Options> target { out };
    target.value(value);
    if (!target.finish()) buffer.clear();
    return buffer;
}

/** Decodes a value from a buffer, or nullopt when it does not parse. */
template<typename T>
[[nodiscard]] std::optional<T> decode(std::span<const std::byte> buffer) {
    return view::over(buffer).try_get<T>();
}

/** The exact byte length a value would occupy, with no allocation. */
template<writer_options Options = writer_options {}, typename T>
[[nodiscard]] std::size_t measure(const T &value) {
    counting_sink counter;
    basic_writer<Options> target { counter };
    target.value(value);
    // finish() both hands over the last batch and reports the total, so the sink is only there
    // to have somewhere for the bytes to go.
    return target.finish().value_or(0);
}

/** Output byte-identical to the reference encoder: unbounded containers, nothing else changed. */
inline constexpr writer_options reference_parity { .counted_containers_from = never_counted };

/** Shorthand for the policy that does no compaction at all. */
inline constexpr writer_options no_compaction { .compact_types = false, .numeric_packing = false };

} // namespace serpent::bjdata
