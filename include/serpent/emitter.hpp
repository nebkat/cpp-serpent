#pragma once

#include <serpent/config.hpp>
#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/limits.hpp>
#include <serpent/sink.hpp>

#include <concepts>
#include <expected>
#include <ranges>
#include <span>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

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
    std::size_t settled = 0; ///< written in room already handed back; size() adds what is used of the room in hand

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

    /**
     * Where writes actually land.
     *
     * For a sink that can lend its own storage this points into the destination, so a document
     * is written once instead of being gathered here and copied again - and the batch above is
     * never touched. For any other sink it points at that batch.
     */
    std::byte *room = nullptr;
    std::size_t room_size = 0;
    std::size_t room_used = 0;

    /** Asks the sink for more room. Null when it does not lend, which selects the batch. */
    std::span<std::byte> (*lend_room)(void *, std::size_t, std::size_t) = nullptr;
    void (*keep_room)(void *, std::size_t) = nullptr;

    /**
     * How much room to ask for, doubling up to a cap.
     *
     * Starting small keeps a short document from reserving kilobytes it will hand straight
     * back; doubling keeps a long one from asking many times.
     */
    static constexpr std::size_t first_chunk = 256;
    static constexpr std::size_t largest_chunk = 16384;
    std::size_t next_chunk = first_chunk;

    /** Hands back what was used and asks for the next chunk. */
    bool renew_room(std::size_t at_least, std::size_t preferred = 0) noexcept {
        this->keep_room(this->context, this->room_used);
        this->next_chunk = std::min(this->next_chunk * 2, largest_chunk);
        // The floor and the preference go separately: what must land in one piece is not
        // negotiable, the rest is, and the sink is the only one that knows its own storage.
        const auto next = this->lend_room(this->context, at_least, std::max(preferred, this->next_chunk));
        if (next.empty()) {
            // The room in hand stays in hand, used as far as it was: finish() keeps it again, and
            // keeping the same room twice has to land on the same answer.
            this->fail(errc::sink_failed);
            return false;
        }
        this->settled += this->room_used;
        this->room = next.data();
        this->room_size = next.size();
        this->room_used = 0;
        return true;
    }

    /** Hands whatever is gathered to the sink. Clears first, so a failure cannot re-enter. */
    bool flush() noexcept {
        if (this->lend_room != nullptr) {
            // Already written into the destination; only the length needs settling, and then a
            // fresh chunk to carry on in.
            this->keep_room(this->context, this->room_used);
            this->next_chunk = std::min(this->next_chunk * 2, largest_chunk);
            const auto next = this->lend_room(this->context, 0, this->next_chunk);
            if (next.empty()) {
                this->fail(errc::sink_failed);
                return false;
            }
            this->settled += this->room_used;
            this->room = next.data();
            this->room_size = next.size();
            this->room_used = 0;
            return true;
        }

        if (this->room_used == 0) return true;
        const std::size_t count = this->room_used;
        this->settled += count;
        this->room_used = 0;
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
    , context(std::addressof(out)) {
        if constexpr (lending_sink<S>) {
            this->lend_room = [](void *target, std::size_t at_least, std::size_t preferred) {
                return static_cast<S *>(target)->lend(at_least, preferred);
            };
            this->keep_room = [](void *target, std::size_t bytes) { static_cast<S *>(target)->keep(bytes); };
            const auto first = out.lend(0, first_chunk);
            this->room = first.data();
            this->room_size = first.size();
        } else {
            this->room = this->buffer;
            this->room_size = buffer_capacity;
        }
    }

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
    , context(std::addressof(callable)) {
        this->room = this->buffer;
        this->room_size = buffer_capacity;
    }

    // Copying would give two emitters one sink and flush the batch twice.
    byte_emitter(const byte_emitter &) = delete;
    byte_emitter &operator=(const byte_emitter &) = delete;

    /** Flushes whatever is still gathered, so dropping a writer cannot silently truncate. */
    ~byte_emitter() {
        if (this->lend_room != nullptr)
            this->keep_room(this->context, this->room_used);
        else
            this->flush();
    }

    [[nodiscard]] bool ok() const noexcept { return this->failure == errc::ok; }
    [[nodiscard]] errc error_code() const noexcept { return this->failure; }
    [[nodiscard]] std::size_t size() const noexcept { return this->settled + this->room_used; }

    void fail(errc code) noexcept {
        if (this->ok()) this->failure = code;
    }

    /**
     * The common case: room in the batch, so a copy and two counters.
     *
     * Everything that is not that - flushing, a write too large to batch, a sink that has
     * already failed - lives in put_overflowing, out of line. Kept apart deliberately: with the
     * flush inlined here this whole function was too large to inline into its callers, so a
     * one-byte marker became a call, and the copy inside it became a call to memcpy. Callers
     * pass a constant size almost every time, and that only pays if they can see the copy.
     */
    void put(std::span<const std::byte> bytes) noexcept {
        if (this->failure == errc::ok && bytes.size() <= this->room_size - this->room_used) {
            std::memcpy(this->room + this->room_used, bytes.data(), bytes.size());
            this->room_used += bytes.size();
            return;
        }
        this->put_overflowing(bytes);
    }

    /** One byte, without going through a span and the stack slot that implies. */
    SERPENT_ALWAYS_INLINE void put_byte(std::byte value) noexcept {
        if (this->failure == errc::ok && this->room_used < this->room_size) {
            this->room[this->room_used++] = value;
            return;
        }
        this->put_overflowing(std::span<const std::byte> { &value, 1 });
    }

    /**
     * Writes text whose length is known when the program is compiled - a marker, a key, a piece
     * of punctuation - which is most of what a writer writes.
     *
     * The same as put(), with the width in the type so that the copy is of a fixed size wherever
     * this ends up, rather than only where put() happens to be inlined. See constant_text.hpp.
     */
    template<typename Character, std::size_t Width>
        requires (sizeof(Character) == 1)
    SERPENT_ALWAYS_INLINE void put_constant(const std::array<Character, Width> &text) noexcept {
        if (this->failure == errc::ok && Width <= this->room_size - this->room_used) {
            std::memcpy(this->room + this->room_used, text.data(), Width);
            this->room_used += Width;
            return;
        }
        this->put_overflowing(std::as_bytes(std::span { text }));
    }

    /** The same for a literal, whose terminator is not written. */
    template<std::size_t Size>
    void put_constant(const char (&literal)[Size]) noexcept {
        std::array<char, Size - 1> text {};
        for (std::size_t index = 0; index < text.size(); ++index) text[index] = literal[index];
        this->put_constant(text);
    }

    /**
     * Room for `bytes` more characters in one piece, to be written into directly, or null where
     * the destination cannot give that much at once. What is written there is claimed with used().
     *
     * Usually the room in hand already has it and this is a comparison. Otherwise a sink that
     * lends is asked for a chunk at least that large, and one that does not is flushed so that
     * the batch is empty - which helps only if the batch is large enough, and that is the case
     * that answers null.
     */
    SERPENT_ALWAYS_INLINE [[nodiscard]] char *room_for(std::size_t bytes) noexcept {
        if (this->failure == errc::ok && bytes <= this->room_size - this->room_used)
            return reinterpret_cast<char *>(this->room + this->room_used);
        return this->make_room_for(bytes);
    }

    /** Claims the first `bytes` of what room_for() handed out. */
    SERPENT_ALWAYS_INLINE void used(std::size_t bytes) noexcept { this->room_used += bytes; }

    /** The most compose() can be asked for: enough for any number as text, with room to spare. */
    static constexpr std::size_t composed_capacity = 64;

    /**
     * Writes a value that is composed where it will be kept, rather than somewhere else and then
     * copied in - a number, which a conversion writes out a digit at a time.
     *
     * `write` is handed room for `at_most` characters and returns how many it used. That room is
     * the destination's own whenever it can be had, which is nearly always; where it cannot, it
     * is a scratch buffer instead, put in the ordinary way. So nothing is asked of a sink that it
     * could not already do, and the common case loses a copy.
     */
    template<typename Write>
    void compose(std::size_t at_most, Write write) noexcept {
        if (char *const to = this->room_for(at_most)) {
            this->used(write(to));
            return;
        }
        char scratch[composed_capacity];
        const std::size_t written = write(scratch);
        this->put_text(std::string_view { scratch, written });
    }

    void put_text(std::string_view text) noexcept {
        this->put(std::span<const std::byte> { reinterpret_cast<const std::byte *>(text.data()), text.size() });
    }

    /** The part of room_for() that has to go and get the room. */
    [[gnu::noinline]] char *make_room_for(std::size_t bytes) noexcept {
        if (!this->ok()) return nullptr;
        if (this->lend_room != nullptr) {
            // Preferred, not demanded. What is asked for here is a worst case - the longest an
            // integer could be, not the length of this one - so a fixed buffer near its end may
            // well not have it and still have room for what is actually written. Demanding it
            // would fail a document that fits.
            if (!this->renew_room(0, bytes)) return nullptr;
        } else if (bytes > buffer_capacity || !this->flush()) {
            return nullptr;
        }
        if (bytes > this->room_size - this->room_used) return nullptr;
        return reinterpret_cast<char *>(this->room + this->room_used);
    }

    /** Everything put() is not: a flush, an oversized write, or a sink that has failed. */
    [[gnu::noinline]] void put_overflowing(std::span<const std::byte> bytes) noexcept {
        if (!this->ok()) return;

        if (this->lend_room != nullptr) {
            if (!this->renew_room(bytes.size())) return;
            std::memcpy(this->room + this->room_used, bytes.data(), bytes.size());
            this->room_used += bytes.size();
            return;
        }

        if (!this->flush()) return;
        if (bytes.size() >= buffer_capacity) {
            // Larger than the batch, so gathering it would only add a copy.
            if (!this->write_bytes(this->context, bytes)) {
                this->fail(errc::sink_failed);
                return;
            }
            this->settled += bytes.size();
        } else {
            std::memcpy(this->buffer, bytes.data(), bytes.size());
            this->room_used = bytes.size();
        }
    }

    /** The single check at the end: the bytes written, or the first failure. */
    [[nodiscard]] std::expected<std::size_t, error> finish() noexcept {
        if (this->lend_room != nullptr) {
            // Settled: hand back the unused tail and disarm, so the destructor does not commit
            // a second time and truncate what was just kept.
            this->keep_room(this->context, this->room_used);
            this->settled += this->room_used;
            this->lend_room = nullptr;
            this->room_used = 0;
            this->room_size = 0;
        } else {
            this->flush();
        }
        if (this->depth != 0) this->fail(errc::unterminated_container);
        if (!this->ok()) return std::unexpected { error { this->failure, this->size() } };
        return this->size();
    }
};

/**
 * Writes a tagged variant.
 *
 * An alternative written as an object gets the name, in the same object as its members. Anything
 * else - a number, a string, an array - already says what it is and goes out as itself.
 */
template<typename Emitter, tagged Tag, typename Variant>
void emit_tagged(Emitter &out, const tagged_variant<Tag, Variant> &item) {
    const std::size_t active = item.target.index();
    std::visit(
            [&out, active]<typename Held>(const Held &held) {
                if constexpr (detail::object_like<std::remove_cvref_t<Held>>) {
                    const auto scope = out.object();
                    out.key(Tag.key());
                    out.string(Tag.name(active));
                    serializer<std::remove_cvref_t<Held>>::write_members(out, held);
                } else {
                    emit_value(out, held);
                }
            },
            item.target);
}

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
    } else if constexpr (std::same_as<bare, std::nullptr_t> || std::same_as<bare, std::monostate>) {
        out.null();
    } else if constexpr (std::is_enum_v<bare>) {
        // An enumeration that names its values writes those; one that says nothing is a number.
        if (!emit_mapped_enum(out, item)) emit_value(out, std::to_underlying(item));
    } else if constexpr (requires { out.number(item); }) {
        // A writer that cares what type a number has - a float's digits, a marker's width - is
        // given it as the type it has.
        out.number(item);
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
    } else if constexpr (requires { emit_tagged(out, item); }) {
        emit_tagged(out, item);
    } else if constexpr (detail::variant_like<bare>) {
        // Untagged: whichever alternative is held is written as itself, and the value on the
        // wire says what it is.
        if (item.valueless_by_exception()) {
            out.null();
        } else {
            std::visit([&out](const auto &held) { emit_value(out, held); }, item);
        }
    } else if constexpr (detail::byte_range<bare>) {
        out.bytes(item);
    } else if constexpr (detail::pair_like<bare>) {
        // A key that is not text cannot be a key, so a keyed container becomes a sequence of
        // these and this is what one of them looks like.
        const auto scope = out.array();
        emit_value(out, item.first);
        emit_value(out, item.second);
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
