#pragma once

// The document-level BJData API: encode a value, decode a value, measure one, or splice one
// document into another. These name the BJData writer and view, so they live here rather
// than in serial/, which knows about neither.

#include <nonstd/bjdata/view.hpp>
#include <nonstd/bjdata/writer.hpp>
#include <nonstd/serial/serializer.hpp>
#include <nonstd/serial/sink.hpp>

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <cstddef>

namespace nonstd::bjdata {

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

/** Writes one value to a sink and reports the bytes produced, or the first failure. */
template<sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value, writer_options options = {}) {
    writer target { out, options };
    target.value(value);
    return target.finish();
}

/** Encodes a value into a fresh buffer. The allocating convenience over write(). */
template<typename T>
[[nodiscard]] std::vector<std::byte> to_bytes(const T &value, writer_options options = {}) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    writer target { out, options };
    target.value(value);
    if (!target.finish()) buffer.clear();
    return buffer;
}

/** Decodes a value from a buffer, or nullopt when it does not parse. */
template<typename T>
[[nodiscard]] std::optional<T> from_bytes(std::span<const std::byte> buffer) {
    return view::over(buffer).try_get<T>();
}

/** The exact byte length a value would occupy, with no allocation. */
template<typename T>
[[nodiscard]] std::size_t measure(const T &value, writer_options options = {}) {
    counting_sink counter;
    writer target { counter, options };
    target.value(value);
    return counter.size();
}

}// namespace nonstd::bjdata
