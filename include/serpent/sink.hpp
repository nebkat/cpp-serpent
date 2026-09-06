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
 * The return type is deliberately unconstrained. The firmware already has around twenty
 * classes shaped like this - bip_buffer, evlp::stream_buffer, storage::norse,
 * storage::nandseq, bsd::socket, http::client, ntrip::server, the OTA writers,
 * peripheral::uart - and they variously return void, bool, std::size_t or std::expected.
 * Accepting all of them as they stand is the point; see put() for how a result is read.
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
        (void) out.write(bytes);
        return true;
    }
}

}// namespace detail

/** Appends to any byte-like growable container: vector<byte>, vector<uint8_t>, string. */
template<typename Container>
class container_sink {
    using element = typename Container::value_type;
    static_assert(sizeof(element) == 1 && (std::same_as<element, std::byte> || std::same_as<element, char>
                                           || std::same_as<element, unsigned char> || std::same_as<element, signed char>),
                  "container_sink requires a container of a byte-sized, byte-aliasing element");

    Container *target = nullptr;

public:
    explicit container_sink(Container &target) noexcept: target(&target) {}

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
 * The policy is the one lib/gnss/src/gnss/rtcm/message_builder.hpp:31-33 states: writes that
 * do not fit are dropped and set overflowed(), which the caller checks once at the end. This
 * is the sink for the no-heap paths, where a document must land inside a fixed byte budget.
 */
class span_sink {
    std::span<std::byte> target {};
    std::size_t used = 0;
    bool overflow = false;

public:
    span_sink() = default;
    explicit span_sink(std::span<std::byte> target) noexcept: target(target) {}

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

/**
 * Counts bytes without storing any.
 *
 * Two uses: sizing a buffer before filling it, and hashing. app/src/app/function/trx/
 * recorder.cpp:285-286 currently serialises a whole metadata document into a fresh vector
 * purely to CRC it and discard it, which this replaces with no allocation at all.
 */
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
    explicit iterator_sink(Iterator target) noexcept: target(target) {}

    void write(std::span<const std::byte> bytes) { this->target = std::ranges::copy(bytes, this->target).out; }

    [[nodiscard]] Iterator iterator() const noexcept { return this->target; }
};

template<typename Iterator>
iterator_sink(Iterator) -> iterator_sink<Iterator>;

}// namespace serpent
