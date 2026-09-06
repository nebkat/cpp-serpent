#pragma once

// Rendering a BJData document as JSON.
//
// The bridge between the two formats, so neither json/ nor bjdata/ has to know about the
// other. This is also what covers a type using to_bjdata, which names the BJData writer and
// therefore cannot be written to JSON directly: encode it, then transcribe it.
//
//     to_json(view::over(to_bytes(value)))

#include <nonstd/bjdata/document.hpp>
#include <nonstd/bjdata/ndarray.hpp>
#include <nonstd/bjdata/view.hpp>
#include <nonstd/json/writer.hpp>
#include <nonstd/serial/sink.hpp>

#include <expected>
#include <string>

#include <cstddef>

namespace nonstd::bjdata {

void write_json_value(json_writer &out, view source);

/** An N-D array is nested rather than flattened, which is what a JSON consumer expects. */
inline void write_json_ndarray(json_writer &out, const ndarray_view &source) {
    if (source.rank() == 0) {
        write_json_value(out, source.value());
        return;
    }
    const auto scope = out.array();
    for (std::size_t index = 0; index < source.size(); ++index) write_json_ndarray(out, source.at(index));
}

/** Transcribes one BJData value, and everything under it, as JSON. */
inline void write_json_value(json_writer &out, view source) {
    switch (source.type()) {
        case kind::null:
            out.null();
            return;
        case kind::boolean:
            out.boolean(source.as_bool() == true);
            return;
        case kind::integer:
            if (source.type_marker() == marker::uint64) out.integer(source.as_int<std::uint64_t>().value_or(0));
            else out.integer(source.as_int<std::int64_t>().value_or(0));
            return;
        case kind::real:
            out.real(source.as_float<double>().value_or(0.0));
            return;
        case kind::string: {
            const auto text = source.as_string().value_or("");
            if (source.type_marker() == marker::high_precision) out.high_precision(text);
            else out.string(text);
            return;
        }
        case kind::array: {
            if (const auto shaped = as_ndarray(source); shaped && shaped->rank() > 1) {
                write_json_ndarray(out, *shaped);
                return;
            }
            const auto scope = out.array();
            for (const auto element : source.array()) write_json_value(out, element);
            return;
        }
        case kind::object: {
            const auto scope = out.object();
            for (const auto [name, element] : source.items()) {
                out.key(name);
                write_json_value(out, element);
            }
            return;
        }
        default:
            out.fail(errc::type_mismatch);
            return;
    }
}

/**
 * Writes an existing BJData document to a sink as JSON.
 *
 * The value is checked once here rather than at every level: the reader is deliberately
 * total, so walking a truncated document would otherwise emit whatever happened to parse and
 * call it a success. payload_bytes() costs a single pass over the value.
 */
template<sink S>
std::expected<std::size_t, error> write_json(S &out, view source, json_options options = {}) {
    json_writer target { out, options };
    if (!source.payload_bytes()) {
        target.fail(errc::unexpected_end);
        return target.finish();
    }
    write_json_value(target, source);
    return target.finish();
}

/** The allocating convenience over write_json. Empty when the document does not parse. */
[[nodiscard]] inline std::string to_json(view source, json_options options = {}) {
    std::string text;
    container_sink out { text };
    if (!write_json(out, source, options)) text.clear();
    return text;
}

}// namespace nonstd::bjdata
