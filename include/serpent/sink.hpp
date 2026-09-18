#pragma once

#include <algorithm>
#include <concepts>
#include <iterator>
#include <span>
#include <type_traits>

#include <cstddef>

namespace serpent {

/**
 * @brief Anything with a member write(std::span<const std::byte>).
 *
 * The return type is unconstrained on purpose: classes shaped like this variously return
 * void, bool, a byte count or an expected, and all of them should qualify unmodified.
 */
template<typename S>
concept sink = requires(S &out, std::span<const std::byte> bytes) { out.write(bytes); };

/**
 * A sink that can hand out its own memory to be written into.
 *
 * `lend(at_least, preferred)` returns somewhere to write, or an empty span if it cannot. It
 * must never return fewer than `at_least` bytes, which is a single value that has to land in
 * one piece; `preferred` is how much the caller would rather have, and a sink is free to stop
 * short of it at a boundary of its own - the edge of a buffer, the end of the room a container
 * already holds. `keep(n)` then says how much was used.
 *
 * Two numbers rather than one because only the sink knows where its storage ends: told a single
 * figure it must read it as a demand, and a container sized exactly for its document would grow
 * anyway to satisfy the last request. A sink over contiguous storage can do this, and it saves
 * the whole document being copied twice - once into the writer's batch and again into the sink -
 * and saves carrying that batch around at all.
 */
template<typename S>
concept lending_sink = sink<S> && requires(S &out, std::size_t bytes) {
    { out.lend(bytes, bytes) } -> std::same_as<std::span<std::byte>>;
    out.keep(bytes);
};

namespace detail {

/** Writes to a sink, treating any bool-testable result as a success flag. */
template<sink S>
[[nodiscard]] inline bool put(S &out, std::span<const std::byte> bytes) {
    using result_type = decltype(out.write(bytes));
    if constexpr (std::is_void_v<result_type>) {
        out.write(bytes);
        return true;
    } else if constexpr (requires { static_cast<bool>(out.write(bytes)); }) {
        // bool, std::size_t (0 bytes written is a failure), std::expected, a pointer...
        return static_cast<bool>(out.write(bytes));
    } else {
        (void)out.write(bytes);
        return true;
    }
}

} // namespace detail

/** Appends to any byte-like growable container: vector<byte>, vector<uint8_t>, string. */
template<typename Container>
class container_sink {
    using element = typename Container::value_type;
    static_assert(sizeof(element) == 1
                    && (std::same_as<element, std::byte> || std::same_as<element, char>
                            || std::same_as<element, unsigned char> || std::same_as<element, signed char>),
            "container_sink requires a container of a byte-sized, byte-aliasing element");

    Container *target = nullptr;
    std::size_t lent = 0;

public:
    explicit container_sink(Container &target) noexcept : target(&target) {}

    void write(std::span<const std::byte> bytes) {
        const auto *first = reinterpret_cast<const element *>(bytes.data());
        this->target->insert(this->target->end(), first, first + bytes.size());
    }

    /** Grows the container and lends out the new room, so the writer fills it in place. */
    [[nodiscard]] std::span<std::byte> lend(std::size_t at_least, std::size_t preferred) {
        this->lent = this->target->size();
        // Room the container already holds costs nothing to hand over, so one reserved to the
        // size of its document is filled without ever growing past what its caller asked for.
        const auto spare = this->target->capacity() - this->lent;
        auto bytes = std::max(at_least, std::min(preferred, spare));
        if (bytes == 0) bytes = std::max(preferred, std::size_t { 1 });
        this->grow_to(this->lent + bytes);
        return { reinterpret_cast<std::byte *>(this->target->data()) + this->lent, bytes };
    }

    /** Keeps that much of what was lent, and gives the rest back. */
    void keep(std::size_t bytes) { this->target->resize(this->lent + bytes); }

private:
    /**
     * Room that is about to be written into does not need to be zeroed first, and resize() zeroes
     * it: every byte of a document written twice. A string can be told not to; a vector cannot.
     */
    void grow_to(std::size_t size) {
        if constexpr (requires { this->target->resize_and_overwrite(size, [](char *, std::size_t count) { return count; }); }) {
            this->target->resize_and_overwrite(size, [](char *, std::size_t count) { return count; });
        } else {
            this->target->resize(size);
        }
    }

    [[nodiscard]] const Container &container() const noexcept { return *this->target; }
};

template<typename Container>
container_sink(Container &) -> container_sink<Container>;

/**
 * Writes into a caller-supplied buffer, latching overflow rather than throwing.
 *
 * For the paths that may not allocate. A write that does not fit is dropped and sets
 * overflowed(), which the caller checks once at the end.
 */
class span_sink {
    std::span<std::byte> target {};
    std::size_t used = 0;
    std::size_t lent = 0;
    bool overflow = false;

public:
    span_sink() = default;
    explicit span_sink(std::span<std::byte> target) noexcept : target(target) {}

    bool write(std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > this->target.size() - this->used) {
            this->overflow = true;
            return false;
        }
        std::ranges::copy(bytes, this->target.begin() + static_cast<std::ptrdiff_t>(this->used));
        this->used += bytes.size();
        return true;
    }

    /**
     * Lends out what is left of the buffer, so a document is written straight into it.
     *
     * A fixed buffer knows its room exactly, which is the whole of what lend asks: it can always
     * offer the remainder and never has to find more. Only a value that will not fit at all
     * overflows, and that is the same condition write() latches.
     */
    [[nodiscard]] std::span<std::byte> lend(std::size_t at_least, std::size_t preferred) noexcept {
        const auto spare = this->target.size() - this->used;
        if (spare == 0 || spare < at_least) {
            this->overflow = true;
            return {};
        }
        this->lent = this->used;
        return this->target.subspan(this->lent, std::min(preferred, spare));
    }

    /**
     * Keeps that much of what was lent, and gives the rest back.
     *
     * Says where the used part ends rather than how much was added, so committing the same
     * chunk twice - which a writer does when a lend fails and the failure is settled later -
     * lands on the same answer both times.
     */
    void keep(std::size_t bytes) noexcept { this->used = this->lent + bytes; }

    [[nodiscard]] std::size_t size() const noexcept { return this->used; }
    [[nodiscard]] bool overflowed() const noexcept { return this->overflow; }
    [[nodiscard]] std::span<const std::byte> written() const noexcept { return this->target.first(this->used); }
};

/** Counts bytes without storing any: for sizing a buffer, or hashing in one pass. */
class counting_sink {
    std::size_t total = 0;

public:
    void write(std::span<const std::byte> bytes) noexcept { this->total += bytes.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return this->total; }
};

/** Adapts any output iterator over bytes, so std::back_inserter works. */
template<std::output_iterator<std::byte> Iterator>
class iterator_sink {
    Iterator target {};

public:
    explicit iterator_sink(Iterator target) noexcept : target(target) {}

    void write(std::span<const std::byte> bytes) { this->target = std::ranges::copy(bytes, this->target).out; }

    [[nodiscard]] Iterator iterator() const noexcept { return this->target; }
};

template<typename Iterator>
iterator_sink(Iterator) -> iterator_sink<Iterator>;

} // namespace serpent
