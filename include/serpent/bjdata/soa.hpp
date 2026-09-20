#pragma once

// Draft 4 Structure-of-Arrays containers: a table of records with one schema.
//
// `[${` schema `}#count` then the records back to back (row-major); `{${` the same with every
// record's first field before any record's second (column-major). The schema is an object
// without values: each name is followed by what the field is - a fixed-width marker, `T` for a
// boolean byte, `Z` for a field that takes no bytes, `S` or `H` and a length for padded text,
// `[$S#n` and the entries for a dictionary indexed by an unsigned integer a record, `[$U]` for
// text held in a table of offsets after the payload, `{` for a nested record, `[` for a fixed
// run of typed elements. So every record is the same number of bytes and any field of any
// record is at a known place, which is what makes a table worth reading as one.
//
// The schema is read once into a descriptor - a flat list of fields, nested ones in a range
// after their parent - and everything else works from that: how many bytes a record is, where
// a field lies in it, and how long the tables after the payload are.
//
// Included by detail.hpp at its end, since the container skip there steps over tables, and
// includes detail.hpp itself for the cursor; either order works, since each is included once.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace serpent::bjdata::soa {

enum class field_kind : std::uint8_t {
    scalar,     ///< a fixed-width marker: the bytes are the value
    boolean,    ///< one byte, `T` or `F`
    null,       ///< no bytes; always null
    text,       ///< `S`/`H`: a fixed run of bytes, padded with zeros to its length
    dictionary, ///< an unsigned index into entries listed in the schema
    offsets,    ///< an integer index into a table of offsets after the payload
    record,     ///< a nested record: its fields follow in the descriptor
    array,      ///< a fixed run of elements: their types follow in the descriptor
};

struct field {
    std::string_view name;               ///< empty for an element of an array
    field_kind kind = field_kind::null;
    marker type = marker::null;          ///< the scalar's marker; the dictionary's or offset table's index marker; `S` or `H` for text
    std::uint32_t width = 0;             ///< payload bytes a record spends on this field, children included
    std::uint32_t offset = 0;            ///< where in a record the field starts
    std::uint32_t children = 0;          ///< fields of a record or elements of an array, immediately after this one
    std::uint32_t entries = 0;           ///< a dictionary's entry count
    const std::byte *detail = nullptr;   ///< a dictionary's first entry
    std::uint32_t next = 0;              ///< index of the field after this one and its subtree
};

/** The most fields a schema may declare, nested ones included; a bound so that no allocation is needed. */
inline constexpr std::size_t max_fields = 64;

struct schema {
    std::array<field, max_fields> fields {};
    std::uint32_t count = 0;             ///< fields in use
    std::uint32_t record_bytes = 0;      ///< payload bytes per record
    std::uint32_t top_level = 0;         ///< fields directly in the record

    [[nodiscard]] const field *begin() const noexcept { return this->fields.data(); }
    [[nodiscard]] const field *end() const noexcept { return this->fields.data() + this->count; }

    /** The top-level field of that name, or null. */
    [[nodiscard]] const field *find(std::string_view name) const noexcept {
        for (std::uint32_t index = 0; index < this->count; index = this->fields[index].next) {
            if (this->fields[index].name == name) return &this->fields[index];
        }
        return nullptr;
    }
};

namespace detail {

using bjdata::detail::cursor;

std::uint32_t parse_type(cursor &source, schema &into, field &field, int depth) noexcept;

/** The fields of a record whose `{` the cursor has passed; returns the bytes they take. */
inline std::uint32_t parse_fields(cursor &source, schema &into, int depth) noexcept {
    std::uint32_t offset = 0;
    while (source.ok()) {
        if (!source.need(1)) return offset;
        if (source.peek_marker() == marker::object_end) {
            source.advance(1);
            return offset;
        }
        if (into.count == max_fields) {
            source.fail(errc::dimension_overflow);
            return offset;
        }
        auto &field = into.fields[into.count++];
        // The link to the field after this one is set once its subtree is known; until then it
        // points just past this one, so a schema that fails to parse still walks forward.
        field.next = into.count;
        field.name = bjdata::detail::read_key(source);
        if (!source.ok()) return offset;
        field.offset = offset;
        offset += parse_type(source, into, field, depth);
    }
    return offset;
}

/** One type of the schema, at the cursor, into `field`; returns the bytes it takes of a record. */
inline std::uint32_t parse_type(cursor &source, schema &into, field &field, int depth) noexcept {
    if (depth > max_depth) {
        source.fail(errc::depth_exceeded);
        return 0;
    }
    const auto *const at = source.position;
    if (!source.need(1)) return 0;
    const auto kind = source.peek_marker();
    source.advance(1);

    const auto done = [&](std::uint32_t width) {
        field.width = width;
        field.next = into.count;
        return width;
    };

    switch (kind) {
    case marker::boolean_true:
        field.kind = field_kind::boolean;
        return done(1);
    case marker::null:
        field.kind = field_kind::null;
        return done(0);
    case marker::string:
    case marker::high_precision: {
        field.kind = field_kind::text;
        field.type = kind;
        const auto length = bjdata::detail::read_length(source);
        if (!source.ok()) return 0;
        if (length > std::numeric_limits<std::uint32_t>::max()) {
            source.fail(errc::dimension_overflow, at);
            return 0;
        }
        return done(static_cast<std::uint32_t>(length));
    }
    case marker::object_begin: {
        field.kind = field_kind::record;
        field.children = into.count;
        return done(parse_fields(source, into, depth + 1));
    }
    case marker::array_begin: {
        if (!source.need(1)) return 0;
        if (source.peek_marker() == marker::strong_type) {
            source.advance(1);
            if (!source.need(1)) return 0;
            const auto element = source.peek_marker();
            source.advance(1);
            if (!source.need(1)) return 0;
            if (source.peek_marker() == marker::array_end) {
                // `[$U]`: text in a table of offsets after the payload, one index a record.
                source.advance(1);
                if (!is_integer(element)) {
                    source.fail(errc::invalid_strong_type, at);
                    return 0;
                }
                field.kind = field_kind::offsets;
                field.type = element;
                return done(static_cast<std::uint32_t>(payload_width(element)));
            }
            if (source.peek_marker() != marker::count || (element != marker::string && element != marker::high_precision)) {
                source.fail(errc::invalid_strong_type, at);
                return 0;
            }
            // `[$S#n` and n strings: a dictionary, indexed by the narrowest unsigned that counts it.
            source.advance(1);
            const auto entries = bjdata::detail::read_length(source);
            if (!source.ok()) return 0;
            if (entries == 0 || entries > std::numeric_limits<std::uint32_t>::max()) {
                source.fail(errc::invalid_dimensions, at);
                return 0;
            }
            field.kind = field_kind::dictionary;
            field.entries = static_cast<std::uint32_t>(entries);
            field.detail = source.position;
            for (std::uint64_t index = 0; index < entries && source.ok(); ++index) bjdata::detail::skip_key(source);
            if (!source.ok()) return 0;
            field.type = entries <= 0xFF ? marker::uint8
                    : entries <= 0xFFFF   ? marker::uint16
                    : entries <= 0xFFFFFFFF ? marker::uint32
                                            : marker::uint64;
            return done(static_cast<std::uint32_t>(payload_width(field.type)));
        }
        // `[` types `]`: a fixed run of elements, each with a type of its own.
        field.kind = field_kind::array;
        field.children = into.count;
        std::uint32_t width = 0;
        while (source.ok()) {
            if (!source.need(1)) return 0;
            if (source.peek_marker() == marker::array_end) {
                source.advance(1);
                break;
            }
            if (into.count == max_fields) {
                source.fail(errc::dimension_overflow);
                return 0;
            }
            auto &element = into.fields[into.count++];
            element.next = into.count;
            element.offset = width;
            width += parse_type(source, into, element, depth + 1);
        }
        if (field.children == into.count) {
            source.fail(errc::invalid_dimensions, at);
            return 0;
        }
        return done(width);
    }
    default:
        if (!is_strong_type(kind)) {
            source.fail(errc::invalid_strong_type, at);
            return 0;
        }
        field.kind = field_kind::scalar;
        field.type = kind;
        return done(static_cast<std::uint32_t>(payload_width(kind)));
    }
}

} // namespace detail

/**
 * Reads a schema whose `{` the cursor has just passed, leaving the cursor after its `}`.
 *
 * Every field's offset is where it lies in a row-major record; a column-major table lays a
 * top-level field's values out together, and the same offsets serve inside each of those.
 */
[[nodiscard]] inline schema parse(bjdata::detail::cursor &source) noexcept {
    schema result;
    result.record_bytes = detail::parse_fields(source, result, 0);
    if (source.ok() && result.count == 0) source.fail(errc::invalid_dimensions);
    if (!source.ok()) return result;
    for (std::uint32_t index = 0; index < result.count; index = result.fields[index].next) ++result.top_level;
    return result;
}

/**
 * Steps the cursor over a table's payload and the offset tables after it, given its schema and
 * how many records it holds.
 */
inline void skip_payload(bjdata::detail::cursor &source, const schema &layout, std::uint64_t records) noexcept {
    if (layout.record_bytes != 0 && records > std::numeric_limits<std::uint64_t>::max() / layout.record_bytes) {
        source.fail(errc::dimension_overflow);
        return;
    }
    if (!source.need(records * layout.record_bytes)) return;
    source.advance(records * layout.record_bytes);

    // Each offset field owns a table of records + 1 offsets and then that many bytes of text,
    // the last offset being the text's length.
    for (const auto &field : layout) {
        if (field.kind != field_kind::offsets) continue;
        const auto width = payload_width(field.type);
        const auto table = (records + 1) * width;
        if (!source.need(table)) return;
        std::uint64_t length = 0;
        std::int64_t last = bjdata::detail::load_integer(field.type, source.position + records * width);
        if (last < 0) {
            source.fail(errc::negative_length);
            return;
        }
        length = static_cast<std::uint64_t>(last);
        source.advance(table);
        if (!source.need(length)) return;
        source.advance(length);
    }
}

/**
 * Where a reader handle stands in a table, when it does: which container, which field of the
 * schema - or none, for a whole record - and which record. A handle on text that has no length
 * prefix of its own carries the length too.
 */
struct place {
    static constexpr std::uint32_t whole_record = ~std::uint32_t { 0 };
    static constexpr std::uint32_t prefixed = ~std::uint32_t { 0 };

    const std::byte *container = nullptr; ///< the table's `[` or `{`
    std::uint32_t field = whole_record;
    std::uint32_t record = 0;
    std::uint32_t extent = prefixed;      ///< text length, or prefixed when the bytes carry their own

    [[nodiscard]] constexpr bool in_table() const noexcept { return this->container != nullptr; }
    [[nodiscard]] constexpr bool is_record() const noexcept { return this->in_table() && this->field == whole_record; }
};

/**
 * A table placed in its document: the schema, where the records are, how many, which way round,
 * and where each offset field's table and text lie after the payload. Everything that reads a
 * record - into a described type, into a tree, through a handle - asks this where a field is.
 */
class table {
    schema layout;
    const std::byte *body = nullptr;
    const std::byte *limit = nullptr;
    std::uint64_t records = 0;
    bool column_major = false;
    struct offset_field {
        std::uint32_t field = 0;
        const std::byte *offsets = nullptr; ///< records + 1 of them, of the field's index width
        const std::byte *text = nullptr;
        std::uint64_t length = 0;
    };
    std::array<offset_field, max_fields> offset_fields {};
    std::uint32_t offset_field_count = 0;
    bool placed = false;

public:
    table() = default;

    /**
     * From a container's parsed prefix. `buffer` is the whole document; the prefix says where
     * the schema and the records are. Not placed - and false - when the schema or the tables
     * after the payload do not parse or run past the buffer.
     */
    [[nodiscard]] bool place(std::span<const std::byte> buffer, const bjdata::detail::container_prefix &prefix, bool column_major) noexcept {
        this->placed = false;
        if (!prefix.structured()) return false;
        bjdata::detail::cursor source { buffer.data(), prefix.table + 1, buffer.data() + buffer.size() };
        this->layout = parse(source);
        if (!source.ok()) return false;
        this->body = prefix.body;
        this->limit = buffer.data() + buffer.size();
        this->records = prefix.count;
        this->column_major = column_major;

        if (this->layout.record_bytes != 0 && this->records > std::numeric_limits<std::uint64_t>::max() / this->layout.record_bytes) return false;
        const std::uint64_t payload = this->records * this->layout.record_bytes;
        if (static_cast<std::uint64_t>(this->limit - this->body) < payload) return false;
        const std::byte *after = this->body + payload;

        this->offset_field_count = 0;
        for (std::uint32_t index = 0; index < this->layout.count; ++index) {
            const auto &field = this->layout.fields[index];
            if (field.kind != field_kind::offsets) continue;
            const auto width = payload_width(field.type);
            const std::uint64_t entries = (this->records + 1) * width;
            if (static_cast<std::uint64_t>(this->limit - after) < entries) return false;
            const std::int64_t last = bjdata::detail::load_integer(field.type, after + this->records * width);
            if (last < 0 || static_cast<std::uint64_t>(this->limit - after - entries) < static_cast<std::uint64_t>(last)) return false;
            auto &placed_field = this->offset_fields[this->offset_field_count++];
            placed_field.field = index;
            placed_field.offsets = after;
            placed_field.text = after + entries;
            placed_field.length = static_cast<std::uint64_t>(last);
            after = placed_field.text + placed_field.length;
        }
        this->placed = true;
        return true;
    }

    /** From the marker of a container - `[` or `{` - whose prefix has not been parsed yet. */
    [[nodiscard]] bool place_at(std::span<const std::byte> buffer, const std::byte *container) noexcept {
        this->placed = false;
        if (container == nullptr || container >= buffer.data() + buffer.size()) return false;
        const auto opening = to_marker(*container);
        if (opening != marker::array_begin && opening != marker::object_begin) return false;
        bjdata::detail::cursor source { buffer.data(), container + 1, buffer.data() + buffer.size() };
        const auto prefix = bjdata::detail::parse_header_as<bjdata::detail::container_prefix>(source, opening == marker::object_begin);
        if (!source.ok()) return false;
        return this->place(buffer, prefix, opening == marker::object_begin);
    }

    [[nodiscard]] bool ok() const noexcept { return this->placed; }
    [[nodiscard]] bool by_column() const noexcept { return this->column_major; }
    [[nodiscard]] const schema &fields() const noexcept { return this->layout; }
    [[nodiscard]] std::uint64_t size() const noexcept { return this->records; }
    [[nodiscard]] const std::byte *payload_end() const noexcept {
        return this->body + this->records * this->layout.record_bytes;
    }
    /** Just past the whole table: payload and every offset table and its text. */
    [[nodiscard]] const std::byte *end() const noexcept {
        if (this->offset_field_count == 0) return this->payload_end();
        const auto &last = this->offset_fields[this->offset_field_count - 1];
        return last.text + last.length;
    }

    /**
     * Where a top-level field of a record is. Row-major, a record is contiguous and the field
     * at its offset in it; column-major, a field's values are contiguous and the record's is
     * at its index times the field's width. A nested field is at its own offset from there,
     * whichever way round the table is.
     */
    [[nodiscard]] const std::byte *at(const field &top, std::uint64_t record) const noexcept {
        if (this->column_major) return this->body + this->records * top.offset + record * top.width;
        return this->body + record * this->layout.record_bytes + top.offset;
    }

    /** The text a dictionary field's index names, or nothing for an index past the entries. */
    [[nodiscard]] std::optional<std::string_view> dictionary_entry(const field &field, std::uint64_t index) const noexcept {
        if (index >= field.entries) return std::nullopt;
        bjdata::detail::cursor source { this->body, field.detail, this->limit };
        for (std::uint64_t skipped = 0; skipped < index; ++skipped) bjdata::detail::skip_key(source);
        const auto text = bjdata::detail::read_key(source);
        if (!source.ok()) return std::nullopt;
        return text;
    }

    /** The text an offset field's index names, or nothing for an index past the table. */
    [[nodiscard]] std::optional<std::string_view> offset_entry(const field &field, std::uint64_t index) const noexcept {
        const offset_field *placed_field = nullptr;
        for (std::uint32_t at = 0; at < this->offset_field_count; ++at) {
            if (&this->layout.fields[this->offset_fields[at].field] == &field) placed_field = &this->offset_fields[at];
        }
        if (placed_field == nullptr || index >= this->records) return std::nullopt;
        const auto width = payload_width(field.type);
        const std::int64_t from = bjdata::detail::load_integer(field.type, placed_field->offsets + index * width);
        const std::int64_t to = bjdata::detail::load_integer(field.type, placed_field->offsets + (index + 1) * width);
        if (from < 0 || to < from || static_cast<std::uint64_t>(to) > placed_field->length) return std::nullopt;
        return std::string_view { reinterpret_cast<const char *>(placed_field->text) + from, static_cast<std::size_t>(to - from) };
    }

    /** Fixed text: the bytes up to the first zero, which pads a shorter value. */
    [[nodiscard]] static std::string_view fixed_text(const field &field, const std::byte *at) noexcept {
        const auto *text = reinterpret_cast<const char *>(at);
        std::size_t length = field.width;
        while (length > 0 && text[length - 1] == '\0') --length;
        return std::string_view { text, length };
    }
};

} // namespace serpent::bjdata::soa

namespace serpent::bjdata::detail {

inline void skip_schema(cursor &source) noexcept {
    std::ignore = soa::parse(source);
}

inline void skip_structured(cursor &source, const container_prefix &info) noexcept {
    cursor at_schema = source;
    at_schema.position = info.table + 1;
    const auto layout = soa::parse(at_schema);
    if (!at_schema.ok()) {
        source.fail(at_schema.failure, at_schema.origin + at_schema.failure_offset);
        return;
    }
    soa::skip_payload(source, layout, info.count);
}

} // namespace serpent::bjdata::detail
