#pragma once

// Rendering a BJData document as JSON.
//
// The bridge between the two formats, so neither json/ nor bjdata/ has to know about the
// other, and it is how an already-encoded document is rendered:
//
//     json::encode(bjdata::reader::over(bjdata::encode(value)))

#include <serpent/bjdata/document.hpp>
#include <serpent/bjdata/ndarray.hpp>
#include <serpent/bjdata/reader.hpp>
#include <serpent/json/writer.hpp>
#include <serpent/sink.hpp>

#include <expected>
#include <string>

#include <cstddef>

namespace serpent::json {

using bjdata::as_ndarray;
using bjdata::ndarray_view;

void write_value(writer &out, const bjdata::reader &source);

/** An N-D array is nested rather than flattened, which is what a JSON consumer expects. */
inline void write_ndarray(writer &out, const ndarray_view &source) {
    if (source.rank() == 0) {
        write_value(out, source.value());
        return;
    }
    const auto scope = out.array();
    for (std::size_t index = 0; index < source.size(); ++index)
        write_ndarray(out, source.at(index));
}

/** Transcribes one BJData value, and everything under it, as JSON. */
inline void write_value(writer &out, const bjdata::reader &source) {
    switch (source.type()) {
    case kind::null: out.null(); return;
    case kind::boolean: out.boolean(source.as<bool>() == true); return;
    case kind::integer:
        if (source.type_marker() == bjdata::marker::uint64)
            out.integer(source.as<std::uint64_t>().value_or(0));
        else
            out.integer(source.as<std::int64_t>().value_or(0));
        return;
    case kind::real: out.real(source.as<double>().value_or(0.0)); return;
    case kind::string: {
        const auto text = source.as<std::string_view>().value_or("");
        if (source.type_marker() == bjdata::marker::high_precision)
            out.high_precision(text);
        else
            out.string(text);
        return;
    }
    case kind::array: {
        if (const auto shaped = as_ndarray(source); shaped && shaped->rank() > 1) {
            write_ndarray(out, *shaped);
            return;
        }
        const auto scope = out.array();
        for (const auto &element : source.array())
            write_value(out, element);
        return;
    }
    case kind::object: {
        const auto scope = out.object();
        for (const auto &[name, element] : source.items()) {
            out.key(name);
            write_value(out, element);
        }
        return;
    }
    default: out.fail(errc::type_mismatch); return;
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
std::expected<std::size_t, error> write(S &out, const bjdata::reader &source, writer_options options = {}) {
    writer target { out, options };
    if (!source.payload_bytes()) {
        target.fail(errc::unexpected_end);
        return target.finish();
    }
    write_value(target, source);
    return target.finish();
}

/** The allocating convenience over write_TMP. Empty when the document does not parse. */
[[nodiscard]] inline std::string encode(const bjdata::reader &source, writer_options options = {}) {
    std::string text;
    container_sink out { text };
    if (!write(out, source, options)) text.clear();
    return text;
}

} // namespace serpent::json
