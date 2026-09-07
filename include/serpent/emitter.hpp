#pragma once

#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/limits.hpp>
#include <serpent/sink.hpp>

#include <concepts>
#include <expected>
#include <ranges>
#include <span>

#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace serpent {

/**
 * @brief Where bytes go, whether it went wrong, and how deep the nesting is.
 *
 * The sink is erased into a function pointer and a context pointer, so an emitter is a
 * concrete class rather than a template over its destination.
 *
 * Failures latch: the first is kept and every later call is a no-op, so a caller checks once
 * at the end rather than after each field.
 */
class byte_emitter {
    using write_function = bool (*)(void *, std::span<const std::byte>);

    write_function write_bytes = nullptr;
    void *context = nullptr;
    errc failure = errc::ok;
    std::size_t produced = 0;

    /**
     * Emitted bytes are gathered here and handed to the sink in batches.
     *
     * The sink is reached through a function pointer, so the copy behind it can never be
     * inlined and a three-byte write costs about what a hundred-byte one does. Writers emit at
     * token granularity - a brace, a key, a separator - so without this the plumbing costs more
     * than the encoding.
     */
    static constexpr std::size_t buffer_capacity = 256;
    std::byte buffer[buffer_capacity] {};
    std::size_t buffered = 0;

    /** Hands whatever is gathered to the sink. Clears first, so a failure cannot re-enter. */
    bool flush() noexcept {
        if (this->buffered == 0) return true;
        const std::size_t count = this->buffered;
        this->buffered = 0;
        if (!this->write_bytes(this->context, std::span<const std::byte> { this->buffer, count })) {
            this->fail(errc::sink_failed);
            return false;
        }
        return true;
    }

protected:
    std::uint32_t object_mask = 0; ///< bit i: nesting level i is an object
    int depth = 0;

    bool push(bool object) noexcept {
        if (!this->ok()) return false;
        if (this->depth >= max_depth) {
            this->fail(errc::depth_exceeded);
            return false;
        }
        if (object)
            this->object_mask |= 1u << this->depth;
        else
            this->object_mask &= ~(1u << this->depth);
        ++this->depth;
        return true;
    }

    bool pop(bool object) noexcept {
        if (!this->ok()) return false;
        if (this->depth == 0) {
            this->fail(errc::unbalanced_container);
            return false;
        }
        if (this->inside_object() != object) {
            this->fail(errc::unbalanced_container);
            return false;
        }
        --this->depth;
        return true;
    }

    [[nodiscard]] bool inside_object() const noexcept {
        return this->depth > 0 && (this->object_mask & (1u << (this->depth - 1))) != 0;
    }

public:
    template<sink S>
    explicit byte_emitter(S &out) noexcept
    : write_bytes([](void *target, std::span<const std::byte> bytes) {
        return detail::put(*static_cast<S *>(target), bytes);
    })
    , context(std::addressof(out)) {}

    /** Also accepts a plain callable, so a lambda needs no sink wrapper. */
    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit byte_emitter(F &callable) noexcept
    : write_bytes([](void *target, std::span<const std::byte> bytes) {
        using result_type = std::invoke_result_t<F &, std::span<const std::byte>>;
        if constexpr (std::is_void_v<result_type>) {
            (*static_cast<F *>(target))(bytes);
            return true;
        } else {
            return static_cast<bool>((*static_cast<F *>(target))(bytes));
        }
    })
    , context(std::addressof(callable)) {}

    // Copying would give two emitters one sink and flush the batch twice.
    byte_emitter(const byte_emitter &) = delete;
    byte_emitter &operator=(const byte_emitter &) = delete;

    /** Flushes whatever is still gathered, so dropping a writer cannot silently truncate. */
    ~byte_emitter() { this->flush(); }

    [[nodiscard]] bool ok() const noexcept { return this->failure == errc::ok; }
    [[nodiscard]] errc error_code() const noexcept { return this->failure; }
    [[nodiscard]] std::size_t size() const noexcept { return this->produced; }

    void fail(errc code) noexcept {
        if (this->ok()) this->failure = code;
    }

    void put(std::span<const std::byte> bytes) noexcept {
        if (!this->ok()) return;

        if (bytes.size() <= buffer_capacity - this->buffered) {
            std::memcpy(this->buffer + this->buffered, bytes.data(), bytes.size());
            this->buffered += bytes.size();
            this->produced += bytes.size();
            return;
        }

        if (!this->flush()) return;
        if (bytes.size() >= buffer_capacity) {
            // Larger than the buffer, so batching it would only add a copy.
            if (!this->write_bytes(this->context, bytes)) {
                this->fail(errc::sink_failed);
                return;
            }
        } else {
            std::memcpy(this->buffer, bytes.data(), bytes.size());
            this->buffered = bytes.size();
        }
        this->produced += bytes.size();
    }

    void put_text(std::string_view text) noexcept {
        this->put(std::span<const std::byte> { reinterpret_cast<const std::byte *>(text.data()), text.size() });
    }

    /** The single check at the end: the bytes written, or the first failure. */
    [[nodiscard]] std::expected<std::size_t, error> finish() noexcept {
        this->flush();
        if (this->depth != 0) this->fail(errc::unterminated_container);
        if (!this->ok()) return std::unexpected { error { this->failure, this->produced } };
        return this->produced;
    }
};

/**
 * Turns a C++ value into calls on an emitter.
 *
 * One definition of what an optional, a map or a range of bytes means. Where formats differ -
 * how binary is spelled, what an unknown type falls back to - the emitter supplies the member
 * and decides for itself.
 */
template<typename Emitter, typename T>
void emit_value(Emitter &out, const T &item) {
    using bare = std::remove_cvref_t<T>;

    if constexpr (std::same_as<bare, bool>) {
        out.boolean(item);
    } else if constexpr (std::same_as<bare, std::nullptr_t>) {
        out.null();
    } else if constexpr (std::is_enum_v<bare>) {
        emit_value(out, std::to_underlying(item));
    } else if constexpr (std::floating_point<bare>) {
        out.real(static_cast<double>(item));
    } else if constexpr (std::integral<bare> && std::is_signed_v<bare>) {
        out.integer(static_cast<std::int64_t>(item));
    } else if constexpr (std::integral<bare>) {
        out.integer(static_cast<std::uint64_t>(item));
    } else if constexpr (detail::string_like<bare>) {
        out.string(std::string_view { item });
    } else if constexpr (detail::optional_like<bare>) {
        if (item.has_value())
            emit_value(out, *item);
        else
            out.null();
    } else if constexpr (detail::byte_range<bare>) {
        out.bytes(item);
    } else if constexpr (detail::map_like<bare>) {
        const auto scope = out.object();
        for (const auto &[name, mapped] : item) {
            out.key(std::string_view { name });
            emit_value(out, mapped);
        }
    } else if constexpr (std::ranges::input_range<bare>) {
        out.range(item);
    } else {
        out.emit_custom(item);
    }
}

} // namespace serpent
