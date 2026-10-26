#pragma once

// Building a value tree from BJData in one pass, without recursing.
//
// The generic walk that serializer<value> makes over any source reads a value through a handle
// and its containers through iterators, each level of the document a call deeper and an
// iterator and a handle bigger - a few hundred bytes of stack a level. This one moves a single
// cursor through the bytes and keeps the containers still open in a fixed stack of its own, so
// the stack it takes is the same however deep the document goes, and every byte is looked at
// once. It steps through a document exactly as the container skip in detail.hpp does - the same
// checks in the same order, failing where that fails - so a document builds here if and only
// if it would have been read and found whole.
//
// A table is the exception: its records are read through a handle placed in the table, as the
// generic walk reads them, since their shape is the schema's and not the bytes'. A schema is
// bounded by max_depth already, so that recursion is too.
//
// Found by serializer<value> through lookup on the source, as read_reflected is. A template on
// the tree so that this header need not include value.hpp, nor value.hpp this one.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/bjdata/reader.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/limits.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace serpent {
class value;
} // namespace serpent

namespace serpent::bjdata {

template<typename Tree>
class tree_builder {
    using member = std::pair<Tree, Tree>;
    using array_type = typename Tree::array_type;
    using object_type = typename Tree::object_type;

    /** A container still open: what is left of it, and where its children start being gathered. */
    struct frame {
        std::uint64_t remaining = 0;
        std::size_t start = 0;
        bool object = false;
        bool unbounded = false;
    };

    std::span<const std::byte> buffer;
    detail::cursor scan;
    int base = 0; ///< how deep the value being built is, the root of the document being 0

    // One frame a level. A value is refused past max_depth before anything is opened for it, so
    // no more can be open than there are levels from where the build started down to the limit.
    std::array<frame, max_depth + 1> frames {};
    int open = 0;

    // The members and elements of the containers open, as the JSON builder gathers them: used as
    // a stack, each container's after its parent's, cut back when it closes, and moved into a
    // run sized exactly to it. A member is gathered with its name when the name is read, and its
    // value put in place when that is complete, which for a container is when it closes.
    std::vector<member> gathered_members;
    std::vector<Tree> gathered_elements;

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
                if (this->open == 0) {
                    into = std::move(held);
                    return true;
                }
                this->deliver(std::move(held));
            }
            const marker child = this->next(this->frames[this->open - 1]);
            if (!this->scan.ok()) return false;
            if (child == marker::invalid) {
                held = this->close();
                complete = true;
                continue;
            }
            if (!this->begin(child, this->base + this->open, held, complete)) return false;
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
        default: this->scan.fail(errc::unexpected_marker, at - 1); return false;
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

        frame &opened = this->frames[this->open++];
        opened.remaining = info.count;
        opened.unbounded = info.unbounded;
        opened.object = object;
        opened.start = object ? this->gathered_members.size() : this->gathered_elements.size();
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

    /** A table, read a record at a time through a handle on it, once the skip has found it whole. */
    bool table(marker kind, const std::byte *payload, int depth, Tree &held) {
        detail::cursor probe { this->buffer, payload };
        detail::skip_value(probe, kind, depth);
        if (!probe.ok()) {
            this->scan.fail(probe.failure, probe.origin + probe.failure_offset);
            return false;
        }
        const reader handle { kind, this->buffer, payload, depth };
        if (!serializer<Tree>::walk(handle, held)) {
            this->scan.fail(errc::type_mismatch, payload - 1);
            return false;
        }
        this->scan.position = probe.position;
        return true;
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
            this->gathered_members.emplace_back(Tree { name }, Tree {});
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

    /** Puts a completed child where the container on top keeps it. */
    void deliver(Tree held) {
        if (this->frames[this->open - 1].object) this->gathered_members.back().second = std::move(held);
        else this->gathered_elements.push_back(std::move(held));
    }

    /** The members gathered since `start`, moved into a run of their own with one per name. */
    Tree members_from(std::size_t start) {
        const auto gathered = std::span { this->gathered_members }.subspan(start);
        auto *block = object_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        object_type::place_all(block, gathered);
        this->gathered_members.resize(start);
        Tree held { block };
        held.coalesce_members();
        return held;
    }

    /** Closes the container on top, answering it. */
    Tree close() {
        const frame &top = this->frames[--this->open];
        if (top.object) return this->members_from(top.start);
        const auto gathered = std::span { this->gathered_elements }.subspan(top.start);
        auto *block = array_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        array_type::place_all(block, gathered);
        this->gathered_elements.resize(top.start);
        return Tree { block };
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
