#pragma once

// What the BJData writer and the JSON writer have in common: an erased sink, a latched
// failure, the container-nesting bookkeeping, and the dispatch that turns a C++ value into
// calls on whichever of them you are holding.

#include <nonstd/bjdata/error.hpp>
#include <nonstd/bjdata/fwd.hpp>
#include <nonstd/bjdata/marker.hpp>
#include <nonstd/bjdata/sink.hpp>

#include <concepts>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace nonstd::bjdata {

namespace detail {

/** Anything a string_view can be built from, string literals and char arrays included. */
template<typename T>
concept string_like = std::convertible_to<const T &, std::string_view>;

template<typename T>
concept byte_range = std::ranges::input_range<T>
        && std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, std::byte>;

/** A keyed container whose keys are strings: written as an object, not an array of pairs. */
template<typename T>
concept map_like = std::ranges::input_range<T> && requires {
    typename T::key_type;
    typename T::mapped_type;
} && std::convertible_to<const typename T::key_type &, std::string_view>;

template<typename T>
concept optional_like = requires(const T &value) {
    { value.has_value() } -> std::convertible_to<bool>;
    { *value };
};

}// namespace detail

/**
 * @brief The output half shared by every emitter: where bytes go and whether it went wrong.
 *
 * The sink is type-erased into a function pointer and a context pointer, so an emitter is a
 * concrete class rather than a template over its destination. That is what lets a user's
 * customization be a plain function taking `writer &`, and it means one instantiation rather
 * than one per sink.
 *
 * Failures latch: the first error is kept and every later call becomes a no-op, so callers
 * check once at the end rather than after each field. This is the policy
 * lib/gnss/src/gnss/rtcm/message_builder.hpp:31-33 states for the same reason.
 */
class byte_emitter {
    using write_function = bool (*)(void *, std::span<const std::byte>);

    write_function write_bytes = nullptr;
    void *context = nullptr;
    errc failure = errc::ok;
    std::size_t produced = 0;

protected:
    std::uint32_t object_mask = 0;   ///< bit i: nesting level i is an object
    int depth = 0;

    bool push(bool object) noexcept {
        if (!this->ok()) return false;
        if (this->depth >= max_depth) {
            this->fail(errc::depth_exceeded);
            return false;
        }
        if (object) this->object_mask |= 1u << this->depth;
        else this->object_mask &= ~(1u << this->depth);
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
          }),
          context(std::addressof(out)) {}

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
          }),
          context(std::addressof(callable)) {}

    [[nodiscard]] bool ok() const noexcept { return this->failure == errc::ok; }
    [[nodiscard]] errc error_code() const noexcept { return this->failure; }
    [[nodiscard]] std::size_t size() const noexcept { return this->produced; }

    void fail(errc code) noexcept {
        if (this->ok()) this->failure = code;
    }

    void put(std::span<const std::byte> bytes) noexcept {
        if (!this->ok()) return;
        if (!this->write_bytes(this->context, bytes)) {
            this->fail(errc::sink_failed);
            return;
        }
        this->produced += bytes.size();
    }

    void put_text(std::string_view text) noexcept {
        this->put(std::span<const std::byte> { reinterpret_cast<const std::byte *>(text.data()), text.size() });
    }

    /** The single check at the end: the bytes written, or the first failure. */
    [[nodiscard]] std::expected<std::size_t, error> finish() noexcept {
        if (this->depth != 0) this->fail(errc::unterminated_container);
        if (!this->ok()) return std::unexpected { error { this->failure, this->produced } };
        return this->produced;
    }
};

/**
 * Turns a C++ value into calls on an emitter.
 *
 * Shared by the BJData and JSON writers so the two can never disagree about what a
 * std::optional, a std::map or a range of bytes means. Where they genuinely differ - typed
 * arrays, how binary is spelled, what a user type falls back to - the emitter supplies the
 * member and decides for itself.
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
        if (item.has_value()) emit_value(out, *item);
        else out.null();
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

}// namespace nonstd::bjdata
