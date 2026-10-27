#pragma once

// Building a value tree from MessagePack in one pass, without recursing.
//
// The generic walk that serializer<value> makes over any source reads a value through a handle
// and its containers through iterators, each level of the document a call deeper and an
// iterator and a handle bigger. This one moves a single cursor through the bytes and keeps the
// containers still open in a stack of its own, on the heap, so the stack it takes is the same
// however deep the document goes, and every byte is looked at once. It steps through a document
// exactly as the skip in reader.hpp does - the same checks, failing where that fails - so a
// document builds here only if it would have been read and found whole.
//
// Two things a whole document can hold have no place in a tree, and are refused: an extension,
// which has no kind, and a map key that is not a str, which has no name.
//
// Found by serializer<value> through lookup on the source, as read_reflected is. A template on
// the tree so that this header need not include value.hpp, nor value.hpp this one.

#include <serpent/error.hpp>
#include <serpent/limits.hpp>
#include <serpent/msgpack/format.hpp>
#include <serpent/msgpack/reader.hpp>
#include <serpent/tree_stack.hpp>

#include <bit>
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

namespace serpent::msgpack {

template<typename Tree>
class tree_builder : serpent::detail::tree_stack<Tree, true> {
    detail::cursor scan;
    int base = 0; ///< how deep the value being built is, the root of the document being 0

public:
    tree_builder(std::span<const std::byte> buffer, const std::byte *payload, int depth) noexcept
    : scan { buffer.data(), payload, buffer.data() + buffer.size() }
    , base(depth) {}

    /** One past the value built. */
    [[nodiscard]] const std::byte *position() const noexcept { return this->scan.position; }

    /** The value whose format byte is `head` and whose payload is at the cursor, into `into`. */
    bool build(std::uint8_t head, Tree &into) {
        Tree held;
        bool complete = false;
        if (!this->begin(head, this->base, held, complete)) return false;
        while (true) {
            if (complete) {
                if (this->frames.empty()) {
                    into = std::move(held);
                    return true;
                }
                this->deliver(std::move(held));
            }
            auto &top = this->frames.back();
            if (top.remaining == 0) {
                held = this->close();
                complete = true;
                continue;
            }
            --top.remaining;
            if (top.object && !this->key()) return false;
            if (!this->take(head)) return false;
            if (!this->begin(head, this->base + static_cast<int>(this->frames.size()), held, complete)) return false;
        }
    }

private:
    /** The format byte at the cursor, stepped past. */
    bool take(std::uint8_t &head) {
        if (this->scan.position >= this->scan.limit) {
            this->scan.fail(errc::unexpected_end, this->scan.position);
            return false;
        }
        head = std::to_integer<std::uint8_t>(*this->scan.position);
        ++this->scan.position;
        return true;
    }

    /** A member's key, which has to be a str to be its name. */
    bool key() {
        std::uint8_t head = 0;
        if (!this->take(head)) return false;
        if (shape_of(head).type != family::string) {
            this->scan.fail(errc::type_mismatch, this->scan.position - 1);
            return false;
        }
        const auto info = detail::read_header(head, this->scan.position, this->scan.limit);
        if (!info.ok()) {
            this->scan.fail(errc::unexpected_end, this->scan.position);
            return false;
        }
        this->scan.position = info.body + info.length;
        this->name(Tree { std::string_view { reinterpret_cast<const char *>(info.body), static_cast<std::size_t>(info.length) } });
        return true;
    }

    /**
     * Starts the value whose format byte has just been read: complete at once unless it is a
     * container, which is opened for the loop to fill.
     */
    bool begin(std::uint8_t head, int depth, Tree &held, bool &complete) {
        if (head == format::never_used) {
            this->scan.fail(errc::invalid_marker, this->scan.position - 1);
            return false;
        }
        const auto info = detail::read_header(head, this->scan.position, this->scan.limit);
        if (!info.ok()) {
            this->scan.fail(errc::unexpected_end, this->scan.position);
            return false;
        }
        complete = true;
        switch (info.type) {
        case family::nil: held = Tree {}; break;
        case family::boolean: held = Tree { head == format::boolean_true }; break;
        case family::unsigned_integer:
        case family::signed_integer: {
            // The widest of its signedness that holds it, as the generic walk reads it.
            integer_value found {};
            std::ignore = load_integer(head, info.body, static_cast<std::size_t>(info.length), found);
            if (!found.negative && found.bits > std::uint64_t(std::numeric_limits<std::int64_t>::max())) held = Tree { found.bits };
            else held = Tree { static_cast<std::int64_t>(found.bits) };
            break;
        }
        case family::real:
            if (head == format::float32) held = Tree { static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(load_big(info.body, 4)))) };
            else held = Tree { std::bit_cast<double>(load_big(info.body, 8)) };
            break;
        case family::string:
            held = Tree { std::string_view { reinterpret_cast<const char *>(info.body), static_cast<std::size_t>(info.length) } };
            break;
        case family::binary: held = Tree { std::span<const std::byte> { info.body, static_cast<std::size_t>(info.length) } }; break;
        case family::array:
        case family::map:
            if (depth >= max_depth) {
                this->scan.fail(errc::depth_exceeded, this->scan.position - 1);
                return false;
            }
            // The header has held the count to the bytes left, so a count no buffer could hold
            // is refused before anything is gathered for it.
            this->open(info.type == family::map, info.length, false);
            this->scan.position = info.body;
            complete = false;
            return true;
        default: this->scan.fail(errc::extension_unsupported, this->scan.position - 1); return false;
        }
        this->scan.position = info.body + info.length;
        return true;
    }
};

/**
 * Fills a value tree from the MessagePack value a reader stands on, reading the document once.
 *
 * Returns false for a document that is malformed at or below that value, as reading it through
 * the reader and stepping over it would, and for one holding what a tree cannot. Whoever is
 * stepping through the container the value sits in can step to where it ended.
 */
template<std::same_as<serpent::value> Tree>
bool read_tree(const reader &source, Tree &into) {
    if (source.data() == nullptr) return false;
    tree_builder<Tree> builder { source.buffer(), source.data(), source.nesting() };
    if (!builder.build(source.format_byte(), into)) return false;
    source.note_end(builder.position());
    return true;
}

} // namespace serpent::msgpack
