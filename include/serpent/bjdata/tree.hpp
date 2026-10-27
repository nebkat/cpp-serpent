#pragma once

// Building a value tree from BJData in one pass, without recursing.
//
// The generic walk that serializer<value> makes over any source reads a value through a handle
// and its containers through iterators, each level of the document a call deeper and an
// iterator and a handle bigger - a few hundred bytes of stack a level. This one moves a single
// cursor through the bytes and keeps the containers still open in a stack of its own, on the
// heap, so the stack it takes is the same however deep the document goes, and every byte is
// looked at once. It steps through a document exactly as the container skip in detail.hpp does -
// the same checks in the same order, failing where that fails - so a document builds here if and
// only if it would have been read and found whole.
//
// A table's records have their shape from its schema rather than from bytes of their own, so
// they are read from where the schema places each field, as a handle placed in the table reads
// them. That recurses through the schema, which is bounded by max_depth and soa::max_fields.
//
// Found by serializer<value> through lookup on the source, as read_reflected is. A template on
// the tree so that this header need not include value.hpp, nor value.hpp this one.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/direct.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/bjdata/reader.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/limits.hpp>
#include <serpent/tree_stack.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace serpent {
class value;
} // namespace serpent

namespace serpent::bjdata {

template<typename Tree>
class tree_builder : serpent::detail::tree_stack<Tree> {
    using stack = serpent::detail::tree_stack<Tree>;
    using typename stack::array_type;
    using typename stack::frame;

    std::span<const std::byte> buffer;
    detail::cursor scan;
    int base = 0; ///< how deep the value being built is, the root of the document being 0

public:
    tree_builder(std::span<const std::byte> buffer, const std::byte *payload, int depth) noexcept
    : buffer(buffer)
    , scan(buffer, payload)
    , base(depth) {}

    /** One past the value built. */
    [[nodiscard]] const std::byte *position() const noexcept { return this->scan.position; }

    /** The value whose marker is `kind` and whose payload is at the cursor, into `into`. */
    bool build(marker kind, Tree &into) {
        Tree held;
        bool complete = false;
        if (!this->begin(kind, this->base, held, complete)) return false;
        while (true) {
            if (complete) {
                if (this->frames.empty()) {
                    into = std::move(held);
                    return true;
                }
                this->deliver(std::move(held));
            }
            const marker child = this->next(this->frames.back());
            if (!this->scan.ok()) return false;
            if (child == marker::invalid) {
                held = this->close();
                complete = true;
                continue;
            }
            if (!this->begin(child, this->base + static_cast<int>(this->frames.size()), held, complete)) return false;
        }
    }

private:
    /**
     * Starts the value whose marker has just been read: complete at once unless it is a
     * container with markers of its own inside, which is opened for the loop to fill.
     */
    bool begin(marker kind, int depth, Tree &held, bool &complete) {
        if (depth > max_depth) {
            this->scan.fail(errc::depth_exceeded);
            return false;
        }
        complete = true;
        if (payload_width(kind) != variable_width) return this->scalar(kind, held);
        switch (kind) {
        case marker::string:
        case marker::high_precision: {
            const auto text = detail::read_key(this->scan);
            if (!this->scan.ok()) return false;
            held = Tree { text };
            return true;
        }
        case marker::array_begin: return this->container(kind, depth, held, complete);
        case marker::object_begin: return this->container(kind, depth, held, complete);
        case marker::extension: this->scan.fail(errc::extension_unsupported); return false;
        default: this->scan.fail(errc::unexpected_marker); return false;
        }
    }

    /** A fixed-width value: one with a marker of its own, or an element of a typed container. */
    bool scalar(marker kind, Tree &held) {
        const auto width = payload_width(kind);
        if (!this->scan.need(width)) return false;
        const std::byte *at = this->scan.position;
        this->scan.advance(width);
        if (scalar_at(kind, at, held)) return true;
        this->scan.fail(errc::unexpected_marker, at - 1);
        return false;
    }

    /** A fixed-width value whose payload, all there, is at `at`; false for a marker that is not one. */
    static bool scalar_at(marker kind, const std::byte *at, Tree &held) {
        switch (kind) {
        case marker::null: held = Tree {}; return true;
        case marker::boolean_true: held = Tree { true }; return true;
        case marker::boolean_false: held = Tree { false }; return true;
        // An integer is the widest of its signedness that holds it, as the generic walk reads it.
        case marker::uint64:
            if (const auto whole = detail::load<std::uint64_t>(at); whole > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                held = Tree { whole };
                return true;
            }
            [[fallthrough]];
        case marker::uint8:
        case marker::int8:
        case marker::uint16:
        case marker::int16:
        case marker::uint32:
        case marker::int32:
        case marker::int64:
        case marker::byte: held = Tree { detail::load_integer(kind, at) }; return true;
        case marker::float16: held = Tree { static_cast<double>(decode_float16(detail::load<std::uint16_t>(at))) }; return true;
        case marker::float32: held = Tree { static_cast<double>(detail::load<float>(at)) }; return true;
        case marker::float64: held = Tree { detail::load<double>(at) }; return true;
        case marker::character: held = Tree { std::string_view { reinterpret_cast<const char *>(at), 1 } }; return true;
        default: return false;
        }
    }

    /** A container whose marker has just been read, at `depth`. */
    bool container(marker kind, int depth, Tree &held, bool &complete) {
        const bool object = kind == marker::object_begin;
        const std::byte *payload = this->scan.position;
        const auto info = detail::parse_header(this->scan, object);
        if (!this->scan.ok()) return false;

        if (info.structured()) return this->table(kind, payload, depth, held);

        // As the skip refuses one: a typed container's elements are a level below it.
        if (info.typed() && info.count > 0 && depth >= max_depth) {
            this->scan.fail(errc::depth_exceeded);
            return false;
        }
        if (info.typed() && !object) return this->typed_elements(info, held);
        if (info.typed()) return this->typed_members(info, held);

        this->open(object, info.count, info.unbounded);
        complete = false;
        return true;
    }

    /** A typed array, whose elements are all the one width: bytes if they are bytes. */
    bool typed_elements(const detail::header &info, Tree &held) {
        // The header has held the count to the bytes left, so this cannot overflow or overrun.
        const auto width = payload_width(info.element);
        if (info.element == marker::byte || info.element == marker::uint8) {
            if (!this->scan.need(info.count)) return false;
            held = Tree { std::span<const std::byte> { this->scan.position, static_cast<std::size_t>(info.count) } };
            this->scan.advance(info.count);
            return true;
        }
        if (info.count > std::numeric_limits<std::uint32_t>::max()) {
            this->scan.fail(errc::dimension_overflow);
            return false;
        }
        if (!this->scan.need(info.count * width)) return false;
        auto *block = array_type::reserved(static_cast<std::uint32_t>(info.count));
        for (std::uint64_t index = 0; index < info.count; ++index) {
            Tree element;
            std::ignore = this->scalar(info.element, element);
            array_type::place_back(block, std::move(element));
        }
        held = Tree { block };
        return true;
    }

    /** A typed object, whose every value is the one width and has no marker. */
    bool typed_members(const detail::header &info, Tree &held) {
        const std::size_t start = this->gathered_members.size();
        for (std::uint64_t index = 0; index < info.count; ++index) {
            this->skip_noops();
            const auto name = detail::read_key(this->scan);
            if (!this->scan.ok()) return false;
            Tree element;
            if (!this->scalar(info.element, element)) return false;
            this->gathered_members.emplace_back(Tree { name }, std::move(element));
        }
        held = this->members_from(start);
        return true;
    }

    /**
     * A table, once the skip has found it whole: its records from where the schema places each
     * field, as a handle in the table would read them. The schema nests no deeper than
     * max_depth, and its fields number at most soa::max_fields, so the recursion through it
     * is bounded by the schema rather than by the document.
     */
    bool table(marker kind, const std::byte *payload, int depth, Tree &held) {
        detail::cursor probe { this->buffer, payload };
        detail::skip_value(probe, kind, depth);
        if (!probe.ok()) {
            this->scan.fail(probe.failure, probe.origin + probe.failure_offset);
            return false;
        }
        const bool by_column = kind == marker::object_begin;
        detail::cursor header { this->buffer, payload };
        const auto prefix = detail::parse_header_as<detail::container_prefix>(header, by_column);
        soa::table layout;
        if (!header.ok() || !layout.place(this->buffer, prefix, by_column)) {
            this->scan.fail(errc::invalid_dimensions, payload - 1);
            return false;
        }

        // Records are gathered rather than reserved for: a table whose records take no bytes
        // can claim any number of them, and a reservation would be made before any is read.
        const std::size_t start = this->gathered_elements.size();
        const auto &schema = layout.fields();
        for (std::uint64_t record = 0; record < layout.size(); ++record) {
            const std::size_t members = this->gathered_members.size();
            for (std::uint32_t index = 0; index < schema.count(); index = schema.fields[index].next) {
                const auto &field = schema.fields[index];
                Tree element;
                if (!this->table_field(layout, field, layout.at(field, record), element)) return false;
                this->gathered_members.emplace_back(Tree { field.name }, std::move(element));
            }
            this->gathered_elements.push_back(this->members_from(members));
        }
        held = this->elements_from(start);
        this->scan.position = probe.position;
        return true;
    }

    /** One field of a record, whose bytes begin at `at`. */
    bool table_field(const soa::table &layout, const soa::field &field, const std::byte *at, Tree &held) {
        using soa::field_kind;
        const auto refuse = [&] {
            this->scan.fail(errc::type_mismatch, at);
            return false;
        };
        switch (field.kind) {
        // The payload is placed whole, so a field's bytes are all there.
        case field_kind::scalar: return scalar_at(field.type, at, held) || refuse();
        case field_kind::boolean: {
            const auto truth = to_marker(*at);
            if (truth != marker::boolean_true && truth != marker::boolean_false) return refuse();
            held = Tree { truth == marker::boolean_true };
            return true;
        }
        case field_kind::null: held = Tree {}; return true;
        case field_kind::text: held = Tree { soa::table::fixed_text(field, at) }; return true;
        case field_kind::dictionary:
        case field_kind::offsets: {
            std::uint64_t index = 0;
            if (!direct::load_integer(field.type, at, field.width, index)) return refuse();
            const auto entry = field.kind == field_kind::dictionary ? layout.dictionary_entry(field, index) : layout.offset_entry(field, index);
            if (!entry) return refuse();
            held = Tree { *entry };
            return true;
        }
        case field_kind::record: {
            const std::size_t start = this->gathered_members.size();
            const auto &schema = layout.fields();
            for (std::uint32_t index = field.children; index < field.next; index = schema.fields[index].next) {
                const auto &child = schema.fields[index];
                Tree element;
                if (!this->table_field(layout, child, at + child.offset, element)) return false;
                this->gathered_members.emplace_back(Tree { child.name }, std::move(element));
            }
            held = this->members_from(start);
            return true;
        }
        case field_kind::array: {
            const std::size_t start = this->gathered_elements.size();
            const auto &schema = layout.fields();
            for (std::uint32_t index = field.children; index < field.next; index = schema.fields[index].next) {
                const auto &child = schema.fields[index];
                Tree element;
                if (!this->table_field(layout, child, at + child.offset, element)) return false;
                this->gathered_elements.push_back(std::move(element));
            }
            held = this->elements_from(start);
            return true;
        }
        }
        return refuse();
    }

    void skip_noops() noexcept {
        while (this->scan.available(1) && this->scan.peek_marker() == marker::noop) this->scan.advance(1);
    }

    /**
     * Moves the cursor to the next child of the container on top, past its name if it is a
     * member, and answers its marker - or marker::invalid when the container has ended, with
     * the cursor past it, or when the document is broken, with the cursor failed.
     */
    marker next(frame &top) {
        if (top.unbounded) {
            this->skip_noops();
            if (!this->scan.need(1)) {
                this->scan.fail(errc::unterminated_container);
                return marker::invalid;
            }
            if (this->scan.peek_marker() == (top.object ? marker::object_end : marker::array_end)) {
                this->scan.advance(1);
                return marker::invalid;
            }
        } else {
            if (top.remaining == 0) return marker::invalid;
            --top.remaining;
            this->skip_noops();
        }
        if (top.object) {
            const auto name = detail::read_key(this->scan);
            if (!this->scan.ok()) return marker::invalid;
            this->name(Tree { name });
        }

        if (!this->scan.need(1)) {
            this->scan.fail(errc::unterminated_container);
            return marker::invalid;
        }
        const auto *at = this->scan.position;
        const auto kind = to_marker(this->scan.peek());
        if (kind == marker::extension) {
            this->scan.fail(errc::extension_unsupported, at);
            return marker::invalid;
        }
        if (!is_value(kind)) {
            this->scan.fail(errc::unexpected_marker, at);
            return marker::invalid;
        }
        this->scan.advance(1);
        return kind;
    }
};

/**
 * Fills a value tree from the BJData value a reader stands on, reading the document once.
 *
 * Returns false for a document that is malformed at or below that value, as reading it through
 * the reader and stepping over it would. Whoever is stepping through the container the value
 * sits in can step to where it ended.
 */
template<std::same_as<serpent::value> Tree>
bool read_tree(const reader &source, Tree &into) {
    // A record or field of a table has its shape from the schema, not from bytes of its own.
    if (source.place_in_table().in_table()) return serializer<Tree>::walk(source, into);
    tree_builder<Tree> builder { source.buffer(), source.data(), source.nesting() };
    if (!builder.build(source.type_marker(), into)) return false;
    source.note_end(builder.position());
    return true;
}

} // namespace serpent::bjdata
