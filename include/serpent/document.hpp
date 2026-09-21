#pragma once

// A tree and the storage it points into, built in one pass and freed in one go.
//
// serpent::value is a tree that owns every node: each container is an allocation, each long
// string another, and tearing it down walks all of them. A document is the other arrangement -
// one arena holds every block, no node owns anything, and the whole tree goes when the arena
// does. What it costs is that the tree is read-only while it is borrowed, and that it cannot
// outlive the arena; what it buys is a handful of allocations for a document of any size, and
// a teardown that frees them without walking a single node.
//
// The root is an ordinary serpent::value, so everything that reads a value reads a document:
// as<T>(), as_array(), operator[], from_value(), the writers. A value copied out of one owns
// its own storage and is free of the document.

#include <serpent/arena.hpp>
#include <serpent/error.hpp>
#include <serpent/json/reader.hpp>
#include <serpent/value.hpp>

#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace serpent {

/**
 * Where a document's blocks come from: one arena, and nothing owned by any node.
 *
 * Text short enough still lives in the node itself - that costs the arena nothing and saves an
 * indirection - so only a long string reaches the arena at all.
 */
class borrowing_store {
    detail::arena *storage;

public:
    explicit borrowing_store(detail::arena &storage) noexcept : storage(&storage) {}

    [[nodiscard]] value text(std::string_view from) const {
        if (from.size() <= value::inline_text_limit) {
            value held;
            held.assign(from);
            return held;
        }
        const auto *bytes = this->storage->keep(reinterpret_cast<const std::byte *>(from.data()), from.size());
        return value::borrowing(std::string_view { reinterpret_cast<const char *>(bytes), from.size() });
    }

    /**
     * A string as the scan left it.
     *
     * The document's text is already in the arena, so a string with no escape in it is taken
     * where it lies - no copy, no decode, nothing but a pointer and a length. Only an escaped
     * one is decoded, and only then does it need room of its own.
     */
    template<typename Span>
    [[nodiscard]] value scanned_text(const Span &span, std::string &scratch) const {
        if (!span.escaped) {
            if (span.contents.size() <= value::inline_text_limit) {
                value held;
                held.assign(span.contents);
                return held;
            }
            return value::borrowing(span.contents);
        }
        scratch.clear();
        decode_string(span, scratch);
        return this->text(scratch);
    }

    [[nodiscard]] value array_of(std::span<value> items) const {
        if (items.empty()) return value { static_cast<value::array *>(nullptr) };
        const auto count = static_cast<std::uint32_t>(items.size());
        void *memory = this->storage->allocate(value::array::footprint(count), value::array::alignment);
        auto *block = value::array::placed(memory, count);
        value::array::place_all(block, items);
        return value::borrowing(*block);
    }

    [[nodiscard]] value object_of(std::span<member> members) const {
        if (members.empty()) return value::empty_object();
        using member_run = detail::run<member>;
        const auto count = static_cast<std::uint32_t>(members.size());
        void *memory = this->storage->allocate(member_run::footprint(count), member_run::alignment);
        auto *block = member_run::placed(memory, count);
        member_run::place_all(block, members);
        // Coalescing only ever drops members, so it cannot move the block out of the arena.
        detail::coalesce_run(block);
        return value::borrowing(block);
    }
};

/**
 * A tree, and the arena its nodes point into.
 *
 * Move-only: the root borrows from the arena, so the two travel together or not at all. Moving
 * hands over the chunks without moving them, which is why the root survives it.
 */
class document {
    detail::arena storage;
    value tree;

public:
    document() = default;

    document(const document &) = delete;
    document &operator=(const document &) = delete;

    document(document &&other) noexcept
            : storage(std::move(other.storage)), tree(std::exchange(other.tree, value {})) {}

    document &operator=(document &&other) noexcept {
        if (this != &other) {
            this->tree = value {};
            this->storage = std::move(other.storage);
            this->tree = std::exchange(other.tree, value {});
        }
        return *this;
    }

    /** The document's root, read exactly as any other value is. */
    [[nodiscard]] const value &root() const noexcept { return this->tree; }

    /** A copy of the root that owns its own storage, for something that must outlive this. */
    [[nodiscard]] value owned() const { return this->tree; }

    [[nodiscard]] const value &operator[](std::string_view name) const noexcept { return this->tree[name]; }
    [[nodiscard]] const value &at(std::string_view name) const { return this->tree.at(name); }
    [[nodiscard]] const value &at(std::size_t index) const { return this->tree.at(index); }
    [[nodiscard]] std::size_t size() const noexcept { return this->tree.size(); }

    /** How many blocks the arena holds and what they add up to. */
    [[nodiscard]] std::size_t blocks() const noexcept { return this->storage.chunks(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return this->storage.capacity(); }

    /// For a builder filling this document: its arena, and where its root goes.
    [[nodiscard]] detail::arena &store() noexcept { return this->storage; }

    /**
     * Takes a copy of the document's text into the arena, and answers where it landed.
     *
     * Strings are then views of this rather than copies of their own, so the document depends on
     * nothing the caller keeps. A zero byte is put after it, so the scan that reads it can be the
     * one that trusts a terminator.
     */
    [[nodiscard]] std::string_view keep_text(std::string_view from) {
        auto *bytes = static_cast<char *>(this->storage.allocate(from.size() + 1, 1));
        std::memcpy(bytes, from.data(), from.size());
        bytes[from.size()] = '\0';
        return std::string_view { bytes, from.size() };
    }
    void adopt(value root) noexcept { this->tree = std::move(root); }
};

namespace json {

/**
 * Builds a document from JSON text, borrowing the arena for every block.
 *
 * The text itself is not kept: a long string is copied into the arena, so the document does not
 * depend on the caller's buffer once this returns.
 */
template<bool Terminated = false>
[[nodiscard]] std::expected<document, error> parse_document(std::string_view text) {
    document built;
    // The text goes into the arena once, and every string that needs no decoding is then a view
    // of it rather than a copy - which is most strings in most documents.
    const std::string_view kept = built.keep_text(text);
    scanner::basic_cursor<Terminated> scan { kept, kept.data() };
    value root;
    {
        borrowing_store into { built.store() };
        tree_builder<Terminated, borrowing_store> builder { scan, into };
        if (!builder.build(root, 0)) return std::unexpected(error { scan.failure, scan.failure_offset });
    }
    scanner::skip_whitespace(scan);
    if (scan.available(1))
        return std::unexpected(error { errc::trailing_data, static_cast<std::size_t>(scan.position - scan.origin) });
    built.adopt(std::move(root));
    return built;
}

/// The same over a std::string, which has a zero byte after it to read against.
[[nodiscard]] inline std::expected<document, error> parse_document(const std::string &text) {
    return parse_document<true>(std::string_view { text });
}

} // namespace json

} // namespace serpent
