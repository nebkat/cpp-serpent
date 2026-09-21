#pragma once

// Memory for a document's nodes, handed out and never given back until the whole document goes.
//
// Chunks are allocated once and never moved or resized, which is what lets a value point
// straight into one. A single growable block would have to reallocate, and every value pointing
// into it would dangle; an offset would have survived that, but an offset cannot be read without
// the base it counts from, and a value is read on its own - as<T>(), as_array() and the rest
// take no arena argument, and a node inside a container has no way to be handed one. Chunks
// that never move are what buys pointers in a node, so growth adds a chunk rather than widening
// one.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace serpent::detail {

/**
 * A bump allocator: a chain of chunks, each filled from its front, all freed together.
 *
 * There is no way to free one allocation. That is the point - a document's nodes all die at the
 * same moment, so nothing has to be tracked per node and nothing has to be walked to tear it
 * down.
 */
class arena {
    struct chunk {
        chunk *earlier;
        std::size_t capacity;
    };

    /// The elements begin past the header, at the alignment ::operator new already promised.
    static constexpr std::size_t header =
            (sizeof(chunk) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t) * alignof(std::max_align_t);

    static constexpr std::size_t first_capacity = 4096 - header;

    /// Chunks double up to here and then stay put, so the last one wastes at most this much.
    static constexpr std::size_t widest_capacity = 1024 * 1024;

    // Where the next allocation lands and where the chunk ends, held directly rather than as
    // offsets into the chunk: the whole of a hit is then two loads, a mask and a compare, with
    // nothing read through the chunk pointer at all.
    chunk *latest = nullptr;
    std::byte *next = nullptr;
    std::byte *limit = nullptr;
    std::size_t next_capacity = first_capacity;

    void *from_new_chunk(std::size_t bytes, std::size_t alignment) {
        std::size_t capacity = this->next_capacity;
        while (capacity < bytes + alignment) capacity *= 2;
        auto *block = static_cast<chunk *>(::operator new(header + capacity));
        block->earlier = this->latest;
        block->capacity = capacity;
        this->latest = block;
        this->next = reinterpret_cast<std::byte *>(block) + header;
        this->limit = this->next + capacity;
        this->next_capacity = std::min(capacity * 2, widest_capacity);

        const auto at = (reinterpret_cast<std::uintptr_t>(this->next) + alignment - 1)
                & ~(static_cast<std::uintptr_t>(alignment) - 1);
        this->next = reinterpret_cast<std::byte *>(at + bytes);
        return reinterpret_cast<void *>(at);
    }

    void discard() noexcept {
        for (chunk *block = this->latest; block != nullptr;) {
            chunk *earlier = block->earlier;
            ::operator delete(static_cast<void *>(block));
            block = earlier;
        }
        this->latest = nullptr;
        this->next = nullptr;
        this->limit = nullptr;
    }

public:
    arena() = default;
    arena(const arena &) = delete;
    arena &operator=(const arena &) = delete;

    // Moving hands over the chain; the chunks themselves stay where they are, so everything
    // already pointing into them still does.
    arena(arena &&other) noexcept
            : latest(std::exchange(other.latest, nullptr)),
              next(std::exchange(other.next, nullptr)),
              limit(std::exchange(other.limit, nullptr)),
              next_capacity(std::exchange(other.next_capacity, first_capacity)) {}

    arena &operator=(arena &&other) noexcept {
        if (this != &other) {
            this->discard();
            this->latest = std::exchange(other.latest, nullptr);
            this->next = std::exchange(other.next, nullptr);
            this->limit = std::exchange(other.limit, nullptr);
            this->next_capacity = std::exchange(other.next_capacity, first_capacity);
        }
        return *this;
    }

    ~arena() { this->discard(); }

    /**
     * Asks for the first chunk to be about this big.
     *
     * Chunks double, so without a hint a large document ends in a chunk half of which is never
     * used. A caller who knows roughly how much a document will take - the length of the text it
     * came from is a fair guess - pays one allocation and no slack instead.
     */
    void reserve(std::size_t bytes) {
        if (this->latest != nullptr || bytes == 0) return;
        this->next_capacity = bytes;
    }


    /** Room for as many bytes at that alignment, out of the current chunk or a new one. */
    [[nodiscard]] void *allocate(std::size_t bytes, std::size_t alignment) {
        const auto at = (reinterpret_cast<std::uintptr_t>(this->next) + alignment - 1)
                & ~(static_cast<std::uintptr_t>(alignment) - 1);
        const auto end = reinterpret_cast<std::uintptr_t>(this->limit);
        // Empty arena: next and limit are both null, so at and end are zero and any real
        // request falls through to the chunk that has not been made yet.
        if (at <= end && bytes <= end - at) {
            this->next = reinterpret_cast<std::byte *>(at + bytes);
            return reinterpret_cast<void *>(at);
        }
        return this->from_new_chunk(bytes, alignment);
    }

    /** A copy of some bytes in the arena, for text a node is to borrow. */
    [[nodiscard]] std::byte *keep(const std::byte *from, std::size_t bytes) {
        auto *block = static_cast<std::byte *>(this->allocate(bytes, alignof(std::max_align_t)));
        std::memcpy(block, from, bytes);
        return block;
    }

    /** How many chunks are held, and how much they add up to - for tests and for reporting. */
    [[nodiscard]] std::size_t chunks() const noexcept {
        std::size_t count = 0;
        for (const chunk *block = this->latest; block != nullptr; block = block->earlier) ++count;
        return count;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        std::size_t total = 0;
        for (const chunk *block = this->latest; block != nullptr; block = block->earlier) total += block->capacity;
        return total;
    }

    [[nodiscard]] bool empty() const noexcept { return this->latest == nullptr; }
};

} // namespace serpent::detail
