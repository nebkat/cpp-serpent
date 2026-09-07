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

public:
    explicit container_sink(Container &target) noexcept : target(&target) {}

    void write(std::span<const std::byte> bytes) {
        const auto *first = reinterpret_cast<const element *>(bytes.data());
        this->target->insert(this->target->end(), first, first + bytes.size());
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
