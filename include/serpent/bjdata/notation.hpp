#pragma once

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/real_format.hpp>
#include <serpent/bjdata/reader.hpp>
#include <serpent/bjdata/soa.hpp>

#include <charconv>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <cstddef>

namespace serpent::bjdata {

namespace detail {

inline void write_block(std::string &out, std::string_view text) {
    out += '[';
    out += text;
    out += ']';
}

/**
 * An integer as a block. Through std::to_chars rather than std::format, which on Apple's libc++
 * brings the floating-point formatting with it and so needs macOS 13.3 of anyone who includes it.
 */
inline void write_integer_block(std::string &out, std::integral auto value) {
    char digits[24];
    const auto written = std::to_chars(digits, digits + sizeof digits, value);
    write_block(out, std::string_view { digits, static_cast<std::size_t>(written.ptr - digits) });
}

inline void write_block(std::string &out, marker value) {
    out += '[';
    out += static_cast<char>(value);
    out += ']';
}

/** Emits a <marker><integer> pair as two blocks and returns its value. */
inline std::int64_t notate_marked_integer(std::string &out, cursor &source) {
    if (!source.need(1)) return 0;

    const auto kind = to_marker(source.peek());
    if (!is_integer(kind)) {
        source.fail(errc::invalid_length);
        return 0;
    }
    source.advance(1);
    write_block(out, kind);

    const auto width = payload_width(kind);
    if (!source.need(width)) return 0;

    const auto value = load_integer(kind, source.position);
    source.advance(width);
    if (kind == marker::uint64) write_integer_block(out, load<std::uint64_t>(source.position - width));
    else write_integer_block(out, value);
    return value;
}

inline void notate_string_body(std::string &out, cursor &source) {
    const auto length = notate_marked_integer(out, source);
    if (length < 0) {
        source.fail(errc::negative_length);
        return;
    }
    if (!source.need(static_cast<std::uint64_t>(length))) return;
    write_block(out,
            std::string_view { reinterpret_cast<const char *>(source.position), static_cast<std::size_t>(length) });
    source.advance(static_cast<std::uint64_t>(length));
}

void notate_value(std::string &out, cursor &source, marker kind, int depth);

/** Emits the count after '#', which may be a plain integer or a dimension array. */
inline std::uint64_t notate_count(std::string &out, cursor &source) {
    if (!source.need(1)) return 0;

    if (source.peek_marker() != marker::array_begin) {
        const auto value = notate_marked_integer(out, source);
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }

    // Re-parse the dimension list for its product, then emit it token by token.
    auto measured = source;
    const auto shape = read_count(measured);

    source.advance(1);
    write_block(out, marker::array_begin);
    bool column_major = false;
    if (source.available(1) && source.peek_marker() == marker::array_begin) {
        source.advance(1);
        write_block(out, marker::array_begin);
        column_major = true;
    }

    if (source.available(1) && source.peek_marker() == marker::strong_type) {
        source.advance(1);
        write_block(out, marker::strong_type);
        if (!source.need(1)) return 0;
        const auto element = to_marker(source.peek());
        source.advance(1);
        write_block(out, element);
        if (!source.need(1)) return 0;
        source.advance(1);
        write_block(out, marker::count);
        const auto entries = notate_marked_integer(out, source);
        const auto width = payload_width(element);
        for (std::int64_t index = 0; index < entries && source.ok(); ++index) {
            if (!source.need(width)) return 0;
            write_integer_block(out, load_integer(element, source.position));
            source.advance(width);
        }
    } else if (source.available(1) && source.peek_marker() == marker::count) {
        source.advance(1);
        write_block(out, marker::count);
        const auto entries = notate_marked_integer(out, source);
        for (std::int64_t index = 0; index < entries && source.ok(); ++index)
            notate_marked_integer(out, source);
    } else {
        while (source.ok() && source.available(1) && source.peek_marker() != marker::array_end) {
            notate_marked_integer(out, source);
        }
        if (source.need(1)) {
            source.advance(1);
            write_block(out, marker::array_end);
        }
    }

    if (column_major && source.need(1)) {
        source.advance(1);
        write_block(out, marker::array_end);
    }
    return shape.total;
}

/** One type of a table's schema: its marker, then a length, a nested record or a run as it has. */
inline void notate_schema_type(std::string &out, cursor &source, int depth);

/** A table schema's fields, whose `{` has been emitted, up to and including its `}`. */
inline void notate_schema_fields(std::string &out, cursor &source, int depth) {
    if (depth > max_depth) {
        source.fail(errc::depth_exceeded);
        return;
    }
    while (source.need(1)) {
        if (source.peek_marker() == marker::object_end) {
            source.advance(1);
            write_block(out, marker::object_end);
            return;
        }
        notate_string_body(out, source);
        notate_schema_type(out, source, depth);
    }
}

inline void notate_schema_type(std::string &out, cursor &source, int depth) {
    if (!source.need(1)) return;
    const auto kind = source.peek_marker();
    source.advance(1);
    write_block(out, kind);
    switch (kind) {
    case marker::string:
    case marker::high_precision: notate_marked_integer(out, source); return;
    case marker::object_begin: notate_schema_fields(out, source, depth + 1); return;
    case marker::array_begin: {
        if (!source.need(1)) return;
        if (source.peek_marker() == marker::strong_type) {
            // `[$U]`, an offset index; or `[$S#n` and its entries, a dictionary.
            source.advance(1);
            write_block(out, marker::strong_type);
            if (!source.need(1)) return;
            write_block(out, to_marker(source.peek()));
            source.advance(1);
            if (!source.need(1)) return;
            const auto next = source.peek_marker();
            source.advance(1);
            write_block(out, next);
            if (next != marker::count) return;
            const auto entries = notate_marked_integer(out, source);
            for (std::int64_t index = 0; index < entries && source.ok(); ++index) notate_string_body(out, source);
            return;
        }
        while (source.need(1) && source.peek_marker() != marker::array_end) notate_schema_type(out, source, depth + 1);
        if (!source.ok()) return;
        source.advance(1);
        write_block(out, marker::array_end);
        return;
    }
    default: return;
    }
}

/**
 * One field of one record, as the reference implementation writes it: a number or a `T` or `F`
 * with no marker of its own, since the schema has given it; a dictionary or offset index as
 * the integer it is; nothing for a null field; a nested record or run field by field.
 */
inline void notate_table_field(std::string &out, const soa::schema &layout, const soa::field &field, const std::byte *at) {
    switch (field.kind) {
    case soa::field_kind::null: return;
    case soa::field_kind::boolean: write_block(out, to_marker(*at)); return;
    case soa::field_kind::text: write_block(out, soa::table::fixed_text(field, at)); return;
    case soa::field_kind::dictionary:
    case soa::field_kind::offsets: write_integer_block(out, load_integer(field.type, at)); return;
    case soa::field_kind::record:
    case soa::field_kind::array:
        for (auto index = field.children; index < field.next; index = layout.fields[index].next) {
            const auto &child = layout.fields[index];
            notate_table_field(out, layout, child, at + child.offset);
        }
        return;
    case soa::field_kind::scalar: break;
    }
    switch (field.type) {
    case marker::float16:
    case marker::float32:
    case marker::float64: {
        const double value = field.type == marker::float16 ? decode_float16(load<std::uint16_t>(at))
                : field.type == marker::float32            ? load<float>(at)
                                                           : load<double>(at);
        write_block(out, serpent::detail::format_real(value).view());
        return;
    }
    case marker::character: write_integer_block(out, load<std::uint8_t>(at)); return;
    case marker::uint64: write_integer_block(out, load<std::uint64_t>(at)); return;
    default: write_integer_block(out, load_integer(field.type, at)); return;
    }
}

/**
 * A draft 4 table, whose `$` has been emitted and whose schema's `{` is at the cursor: the
 * schema token by token, the count, every record's fields - a record at a time, or a field at a
 * time when the table is column-major - and then each offset field's offsets and its text.
 */
inline void notate_table(std::string &out, cursor &source, const std::byte *container, int depth) {
    soa::table table;
    if (!table.place_at({ source.origin, source.limit }, container)) {
        source.fail(errc::invalid_strong_type);
        return;
    }

    source.advance(1);
    write_block(out, marker::object_begin);
    notate_schema_fields(out, source, depth + 1);
    if (!source.need(1)) return;
    source.advance(1);
    write_block(out, marker::count);
    notate_count(out, source);
    if (!source.ok()) return;

    const auto &layout = table.fields();
    if (table.by_column()) {
        for (std::uint32_t index = 0; index < layout.count(); index = layout.fields[index].next) {
            for (std::uint64_t record = 0; record < table.size(); ++record)
                notate_table_field(out, layout, layout.fields[index], table.at(layout.fields[index], record));
        }
    } else {
        for (std::uint64_t record = 0; record < table.size(); ++record) {
            for (std::uint32_t index = 0; index < layout.count(); index = layout.fields[index].next)
                notate_table_field(out, layout, layout.fields[index], table.at(layout.fields[index], record));
        }
    }

    // Placing the table checked that every offset table and its text lie inside the buffer.
    const std::byte *after = table.payload_end();
    for (const auto &field : layout) {
        if (field.kind != soa::field_kind::offsets) continue;
        const auto width = payload_width(field.type);
        for (std::uint64_t index = 0; index <= table.size(); ++index)
            write_integer_block(out, load_integer(field.type, after + index * width));
        const auto length = static_cast<std::size_t>(load_integer(field.type, after + table.size() * width));
        after += (table.size() + 1) * width;
        write_block(out, std::string_view { reinterpret_cast<const char *>(after), length });
        after += length;
    }
    source.position = table.end();
}

inline void notate_container(std::string &out, cursor &source, bool object, int depth) {
    marker element = marker::invalid;
    std::uint64_t count = 0;
    bool unbounded = false;

    if (!source.need(1)) return;

    if (source.peek_marker() == marker::strong_type) {
        const auto *const container = source.position - 1;
        source.advance(1);
        write_block(out, marker::strong_type);
        if (!source.need(1)) return;
        if (source.peek_marker() == marker::object_begin) {
            notate_table(out, source, container, depth);
            return;
        }
        element = to_marker(source.peek());
        source.advance(1);
        write_block(out, element);
        if (!source.need(1)) return;
        source.advance(1);
        write_block(out, marker::count);
        count = notate_count(out, source);
    } else if (source.peek_marker() == marker::count) {
        source.advance(1);
        write_block(out, marker::count);
        count = notate_count(out, source);
    } else {
        unbounded = true;
    }

    const auto emit_typed_element = [&] {
        const auto width = payload_width(element);
        if (!source.need(width)) return;
        if (is_float(element)) {
            const auto value = element == marker::float16 ? decode_float16(load<std::uint16_t>(source.position))
                    : element == marker::float32          ? load<float>(source.position)
                                                          : static_cast<float>(load<double>(source.position));
            write_block(out, serpent::detail::format_real(value).view());
        } else if (element == marker::character) {
            write_block(out, std::string_view { reinterpret_cast<const char *>(source.position), 1 });
        } else {
            write_integer_block(out, load_integer(element, source.position));
        }
        source.advance(width);
    };

    const auto emit_element = [&] {
        if (!source.need(1)) return;
        const auto kind = to_marker(source.peek());
        source.advance(1);
        write_block(out, kind);
        notate_value(out, source, kind, depth + 1);
    };

    if (unbounded) {
        const auto terminator = object ? marker::object_end : marker::array_end;
        while (source.ok() && source.available(1)) {
            if (source.peek_marker() == terminator) {
                source.advance(1);
                write_block(out, terminator);
                return;
            }
            if (source.peek_marker() == marker::noop) {
                source.advance(1);
                write_block(out, marker::noop);
                continue;
            }
            if (object) notate_string_body(out, source);
            emit_element();
        }
        source.fail(errc::unterminated_container);
        return;
    }

    if (element != marker::invalid && !object) {
        for (std::uint64_t index = 0; index < count && source.ok(); ++index)
            emit_typed_element();
        return;
    }

    for (std::uint64_t index = 0; index < count && source.ok(); ++index) {
        while (source.available(1) && source.peek_marker() == marker::noop) {
            source.advance(1);
            write_block(out, marker::noop);
        }
        if (object) {
            notate_string_body(out, source);
            if (element != marker::invalid) {
                emit_typed_element();
                continue;
            }
        }
        emit_element();
    }
}

inline void notate_value(std::string &out, cursor &source, marker kind, int depth) {
    if (depth > max_depth) {
        source.fail(errc::depth_exceeded);
        return;
    }

    switch (kind) {
    case marker::null:
    case marker::boolean_true:
    case marker::boolean_false:
    case marker::noop: return;

    case marker::string:
    case marker::high_precision: notate_string_body(out, source); return;

    case marker::character: {
        if (!source.need(1)) return;
        write_block(out, std::string_view { reinterpret_cast<const char *>(source.position), 1 });
        source.advance(1);
        return;
    }

    case marker::float16:
    case marker::float32:
    case marker::float64: {
        const auto width = payload_width(kind);
        if (!source.need(width)) return;
        const double value = kind == marker::float16 ? decode_float16(load<std::uint16_t>(source.position))
                : kind == marker::float32            ? load<float>(source.position)
                                                     : load<double>(source.position);
        write_block(out, serpent::detail::format_real(value).view());
        source.advance(width);
        return;
    }

    case marker::array_begin: notate_container(out, source, false, depth); return;
    case marker::object_begin: notate_container(out, source, true, depth); return;

    default: {
        const auto width = payload_width(kind);
        if (width == variable_width) {
            source.fail(errc::unexpected_marker);
            return;
        }
        if (!source.need(width)) return;
        if (kind == marker::uint64) {
            write_integer_block(out, load<std::uint64_t>(source.position));
        } else {
            write_integer_block(out, load_integer(kind, source.position));
        }
        source.advance(width);
        return;
    }
    }
}

} // namespace detail

/**
 * @brief Renders a document as the reference implementation's block notation, e.g. [S][U][5][hello].
 *
 * A token-level trace of the byte stream rather than of the decoded value, so it catches
 * grammar drift that comparing decoded values would miss.
 */
[[nodiscard]] inline std::string block_notation(std::span<const std::byte> buffer) {
    std::string out;
    detail::cursor source { buffer, buffer.data() };

    if (!source.need(1)) return out;
    const auto root = to_marker(source.peek());
    source.advance(1);
    detail::write_block(out, root);
    detail::notate_value(out, source, root, 0);
    return out;
}

[[nodiscard]] inline std::string block_notation(const reader &value) { return block_notation(value.buffer()); }

} // namespace serpent::bjdata
