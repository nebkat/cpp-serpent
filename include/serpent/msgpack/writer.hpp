#pragma once

// MessagePack output.
//
// Every container states its count before its first element, and nothing closes it. Where the
// count is known on opening - array(n), object(n), a sized range, a reflected type - the header
// goes out at once and the elements follow it straight to the sink. Where it is not - object()
// as json_convert and to_json open one, a map, a tagged variant - the container is gathered
// instead, from the outermost one so opened, with a gap left for each header; when that one
// closes every count is known, and the lot goes to the sink with each header at its narrowest.

#include <serpent/config.hpp>
#include <serpent/emitter.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/limits.hpp>
#include <serpent/msgpack/format.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>
#include <nonstd/unaligned.hpp>

#include <array>
#include <concepts>
#include <cstring>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdint>

namespace serpent::msgpack {

class writer;
class array_scope;
class object_scope;

namespace detail {

/** A str header and the bytes after it: what a constant key is on the wire, assembled once. */
template<const std::string_view &Name>
inline constexpr auto encoded_key = [] {
    constexpr std::size_t size = Name.size();
    constexpr std::size_t header = size < 32 ? 1 : size <= 0xff ? 2 : size <= 0xffff ? 3 : 5;
    std::array<std::byte, header + size> bytes {};
    if constexpr (header == 1) {
        bytes[0] = static_cast<std::byte>(format::fixstr | size);
    } else {
        bytes[0] = static_cast<std::byte>(header == 2 ? format::str8 : header == 3 ? format::str16 : format::str32);
        for (std::size_t index = 1; index < header; ++index)
            bytes[index] = static_cast<std::byte>((size >> (8 * (header - 1 - index))) & 0xff);
    }
    for (std::size_t index = 0; index < size; ++index) bytes[header + index] = static_cast<std::byte>(Name[index]);
    return bytes;
}();

} // namespace detail

/** @brief Emits MessagePack into a sink. */
class writer : public byte_emitter {
    friend class array_scope;
    friend class object_scope;

    static constexpr std::uint64_t uncounted = std::numeric_limits<std::uint64_t>::max();

    /** One open container: what has been written into it, and what it promised, if anything. */
    struct level {
        std::uint64_t written = 0;
        std::uint64_t declared = uncounted;
        std::size_t gap = 0; ///< where in `gaps` its header waits, when it has no count yet
    };

    /** A header not yet written, and the place in `gathered` it goes. */
    struct gap {
        std::size_t at = 0;
        std::uint64_t count = 0;
        bool map = false;
    };

    std::array<level, max_depth> levels {};
    std::vector<std::byte> gathered;
    std::vector<gap> gaps;
    int gathering_from = 0; ///< the level of the outermost container opened without a count, or 0
    bool pending_value = false; ///< a key has been written and owes a value

    /** Bytes for the sink, or for the gathered container while one is open. */
    void emit(std::span<const std::byte> bytes) noexcept {
        if (this->gathering_from == 0) {
            this->put(bytes);
            return;
        }
        if (!this->ok()) return;
        this->gathered.insert(this->gathered.end(), bytes.begin(), bytes.end());
    }

    void emit_byte(std::uint8_t value) noexcept {
        if (this->gathering_from == 0) {
            this->put_byte(static_cast<std::byte>(value));
            return;
        }
        if (!this->ok()) return;
        this->gathered.push_back(static_cast<std::byte>(value));
    }

    /** A format byte and a big-endian payload, as one piece. */
    template<typename T>
    void emit_headed(std::uint8_t head, T value) noexcept {
        const nonstd::unaligned_big<T> stored { value };
        std::array<std::byte, 1 + sizeof(T)> piece {};
        piece[0] = static_cast<std::byte>(head);
        std::memcpy(piece.data() + 1, stored.data(), sizeof(T));
        this->emit(piece);
    }

    /**
     * A length or count, under the narrowest of the three forms that holds it: in the format byte
     * itself below `inline_limit`, else after one of `head8`, `head16`, `head32`. A form with no
     * eight-bit width - an array's, a map's - passes zero for it.
     */
    void emit_length(std::uint64_t length, std::uint8_t inline_base, std::uint64_t inline_limit, std::uint8_t head8,
            std::uint8_t head16, std::uint8_t head32) noexcept {
        if (length < inline_limit) {
            this->emit_byte(static_cast<std::uint8_t>(inline_base | length));
        } else if (head8 != 0 && length <= 0xff) {
            this->emit_headed(head8, static_cast<std::uint8_t>(length));
        } else if (length <= 0xffff) {
            this->emit_headed(head16, static_cast<std::uint16_t>(length));
        } else if (length <= 0xffffffff) {
            this->emit_headed(head32, static_cast<std::uint32_t>(length));
        } else {
            this->fail(errc::out_of_range);
        }
    }

    void container_header(bool map, std::uint64_t count) noexcept {
        if (map) {
            this->emit_length(count, format::fixmap, 16, 0, format::map16, format::map32);
        } else {
            this->emit_length(count, format::fixarray, 16, 0, format::array16, format::array32);
        }
    }

    void string_header(std::uint64_t length) noexcept {
        this->emit_length(length, format::fixstr, 32, format::str8, format::str16, format::str32);
    }

    void binary_header(std::uint64_t length) noexcept {
        // No inline form: a limit of zero is never above a length.
        this->emit_length(length, 0, 0, format::bin8, format::bin16, format::bin32);
    }

    /** Before every value: an array counts it, and an object's value follows its key. */
    SERPENT_ALWAYS_INLINE void begin_value() noexcept {
        if (this->depth == 0) return;
        if (this->inside_object()) {
            if (!this->pending_value) {
                this->fail(errc::key_outside_object);
                return;
            }
            this->pending_value = false;
            return;
        }
        ++this->levels[this->depth - 1].written;
    }

    void open(bool map, std::uint64_t declared) noexcept {
        this->begin_value();
        if (!this->push(map)) return;
        auto &here = this->levels[this->depth - 1];
        here.written = 0;
        here.declared = declared;
        if (declared != uncounted) {
            this->container_header(map, declared);
            return;
        }
        if (this->gathering_from == 0) this->gathering_from = this->depth;
        here.gap = this->gaps.size();
        this->gaps.push_back({ this->gathered.size(), 0, map });
    }

    void close(bool map) noexcept {
        if (this->depth == 0) {
            this->fail(errc::unbalanced_container);
            return;
        }
        if (map && this->pending_value) {
            // A key with no value would leave the count one pair ahead of the bytes.
            this->fail(errc::unbalanced_container);
            return;
        }
        const level here = this->levels[this->depth - 1];
        const int closing = this->depth;
        if (!this->pop(map)) return;
        if (here.declared != uncounted) {
            // A count is a promise made before the elements: one that was not kept is a document
            // a reader would walk out of step with.
            if (here.written != here.declared) this->fail(errc::unbalanced_container);
            return;
        }
        this->gaps[here.gap].count = here.written;
        if (closing == this->gathering_from) this->release_gathered();
    }

    /** Hands what was gathered to the sink, each header in its gap at the width its count needs. */
    void release_gathered() noexcept {
        this->gathering_from = 0;
        std::size_t from = 0;
        for (const auto &waiting : this->gaps) {
            this->put(std::span<const std::byte> { this->gathered.data() + from, waiting.at - from });
            this->container_header(waiting.map, waiting.count);
            from = waiting.at;
        }
        this->put(std::span<const std::byte> { this->gathered.data() + from, this->gathered.size() - from });
        this->gathered.clear();
        this->gaps.clear();
    }

    void unsigned_integer(std::uint64_t value) noexcept {
        if (value <= 0x7f) this->emit_byte(static_cast<std::uint8_t>(value));
        else if (value <= 0xff) this->emit_headed(format::uint8, static_cast<std::uint8_t>(value));
        else if (value <= 0xffff) this->emit_headed(format::uint16, static_cast<std::uint16_t>(value));
        else if (value <= 0xffffffff) this->emit_headed(format::uint32, static_cast<std::uint32_t>(value));
        else this->emit_headed(format::uint64, value);
    }

    void signed_integer(std::int64_t value) noexcept {
        // Not negative is unsigned, as every other encoder writes it and every decoder expects.
        if (value >= 0) this->unsigned_integer(static_cast<std::uint64_t>(value));
        else if (value >= -32) this->emit_byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
        else if (value >= std::numeric_limits<std::int8_t>::min()) this->emit_headed(format::int8, static_cast<std::int8_t>(value));
        else if (value >= std::numeric_limits<std::int16_t>::min()) this->emit_headed(format::int16, static_cast<std::int16_t>(value));
        else if (value >= std::numeric_limits<std::int32_t>::min()) this->emit_headed(format::int32, static_cast<std::int32_t>(value));
        else this->emit_headed(format::int64, value);
    }

public:
    template<sink S>
    explicit writer(S &out) noexcept : byte_emitter(out) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit writer(F &callable) noexcept : byte_emitter(callable) {}

    // ---------------- scalars ----------------

    void null() noexcept {
        this->begin_value();
        this->emit_byte(format::nil);
    }

    void boolean(bool value) noexcept {
        this->begin_value();
        this->emit_byte(value ? format::boolean_true : format::boolean_false);
    }

    /** An integer at the narrowest width that holds it. */
    void integer(std::int64_t value) noexcept {
        this->begin_value();
        this->signed_integer(value);
    }

    void integer(std::uint64_t value) noexcept {
        this->begin_value();
        this->unsigned_integer(value);
    }

    /** A float64: a double is never narrowed, since a float32 that holds it is not what was asked for. */
    void real(double value) noexcept {
        this->begin_value();
        this->emit_headed(format::float64, value);
    }

    /** A number as the type it has: an integer at its narrowest, a float as a float32 and a double as a float64. */
    template<typename T>
        requires (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>
    void number(T value) noexcept {
        this->begin_value();
        if constexpr (std::floating_point<T> && sizeof(T) == sizeof(float)) {
            this->emit_headed(format::float32, static_cast<float>(value));
        } else if constexpr (std::floating_point<T>) {
            this->emit_headed(format::float64, static_cast<double>(value));
        } else if constexpr (std::is_signed_v<T>) {
            this->signed_integer(static_cast<std::int64_t>(value));
        } else {
            this->unsigned_integer(static_cast<std::uint64_t>(value));
        }
    }

    void string(std::string_view text) noexcept {
        this->begin_value();
        this->string_header(text.size());
        this->emit(std::as_bytes(std::span { text }));
    }

    void character(char value) noexcept { this->string(std::string_view { &value, 1 }); }

    /** bin 8, 16 or 32: bytes, kept apart from text. */
    void binary(std::span<const std::byte> bytes) noexcept {
        this->begin_value();
        this->binary_header(bytes.size());
        this->emit(bytes);
    }

    /**
     * An extension: a type the application gives a meaning to, and its bytes. Zero to 127 are the
     * application's; -1 is a timestamp, and the rest of the negatives are reserved.
     */
    void extension(std::int8_t type, std::span<const std::byte> data) noexcept {
        this->begin_value();
        switch (data.size()) {
        case 1: this->emit_byte(format::fixext1); break;
        case 2: this->emit_byte(format::fixext2); break;
        case 4: this->emit_byte(format::fixext4); break;
        case 8: this->emit_byte(format::fixext8); break;
        case 16: this->emit_byte(format::fixext16); break;
        default: this->emit_length(data.size(), 0, 0, format::ext8, format::ext16, format::ext32); break;
        }
        this->emit_byte(static_cast<std::uint8_t>(type));
        this->emit(data);
    }

    /** A value that is already MessagePack, copied in as it stands: what splicing one document into another is. */
    void encoded(std::span<const std::byte> value) noexcept {
        this->begin_value();
        this->emit(value);
    }

    // ---------------- containers ----------------

    /** A key: always a str, since a member's name is text. */
    void key(std::string_view name) noexcept {
        if (!this->begin_key()) return;
        this->string_header(name.size());
        this->emit(std::as_bytes(std::span { name }));
    }

    /** A constant key, whose header and bytes were put together once, at compile time. */
    template<const std::string_view &Name>
    void key_literal() noexcept {
        if (!this->begin_key()) return;
        this->emit(detail::encoded_key<Name>);
    }

    /** An array or map whose count is not known yet: gathered until it closes. */
    [[nodiscard]] array_scope array() noexcept;
    [[nodiscard]] object_scope object() noexcept;

    /** An array or map that will hold exactly this many: written as it goes. Closing it short or long fails the writer. */
    [[nodiscard]] array_scope array(std::size_t count) noexcept;
    [[nodiscard]] object_scope object(std::size_t members) noexcept;

    // ---------------- generic ----------------

    template<typename T>
    void value(const T &item) noexcept {
        emit_value(*this, item);
    }

    /** A range of bytes: binary. Called by emit_value. */
    template<serpent::detail::byte_range R>
    void bytes(const R &items) noexcept {
        if constexpr (std::ranges::contiguous_range<R>) {
            this->binary(std::span<const std::byte> { std::ranges::data(items), std::ranges::size(items) });
        } else {
            this->binary(std::ranges::to<std::vector<std::byte>>(items));
        }
    }

    /** A list: counted wherever the range can say how long it is without being consumed. */
    template<std::ranges::input_range R>
    void range(const R &items) noexcept;

    /** Anything with no built-in meaning: goes to the user's customization. */
    template<typename T>
    void emit_custom(const T &item) noexcept {
        serializer<std::remove_cvref_t<T>>::write(*this, item);
    }

private:
    [[nodiscard]] bool begin_key() noexcept {
        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return false;
        }
        if (this->pending_value) {
            // Two keys in a row: the first would be a pair with no value.
            this->fail(errc::unbalanced_container);
            return false;
        }
        ++this->levels[this->depth - 1].written;
        this->pending_value = true;
        return true;
    }
};

/**
 * @brief Closes its container on destruction. The only way to open one.
 *
 * Movable, as the other formats' scopes are, so a container may be opened in one place and
 * closed in another. A moved-from scope closes nothing.
 */
class array_scope {
    writer *out = nullptr;

public:
    explicit array_scope(writer &out) noexcept : out(&out) { this->out->open(false, writer::uncounted); }
    array_scope(writer &out, std::uint64_t count) noexcept : out(&out) { this->out->open(false, count); }

    array_scope(const array_scope &) = delete;
    array_scope &operator=(const array_scope &) = delete;
    array_scope(array_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    array_scope &operator=(array_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->close(false);
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~array_scope() {
        if (this->out != nullptr) this->out->close(false);
    }

    template<typename T>
    void value(const T &item) const noexcept {
        this->out->value(item);
    }
};

class object_scope {
    writer *out = nullptr;

public:
    explicit object_scope(writer &out) noexcept : out(&out) { this->out->open(true, writer::uncounted); }
    object_scope(writer &out, std::uint64_t members) noexcept : out(&out) { this->out->open(true, members); }

    object_scope(const object_scope &) = delete;
    object_scope &operator=(const object_scope &) = delete;
    object_scope(object_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    object_scope &operator=(object_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->close(true);
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~object_scope() {
        if (this->out != nullptr) this->out->close(true);
    }

    template<typename T>
    void member(std::string_view name, const T &item) const noexcept {
        this->out->key(name);
        this->out->value(item);
    }
};

inline array_scope writer::array() noexcept { return array_scope { *this }; }
inline object_scope writer::object() noexcept { return object_scope { *this }; }
inline array_scope writer::array(std::size_t count) noexcept { return array_scope { *this, count }; }
inline object_scope writer::object(std::size_t members) noexcept { return object_scope { *this, members }; }

template<std::ranges::input_range R>
void writer::range(const R &items) noexcept {
    // A range that can be measured without being used up is counted, and so written straight
    // through; only a range that can be walked once is gathered.
    if constexpr (std::ranges::forward_range<R>) {
        const auto scope = this->array(static_cast<std::size_t>(std::ranges::distance(items)));
        for (serpent::detail::range_element_t<decltype(items)> item : items)
            this->value(item);
    } else {
        const auto scope = this->array();
        for (serpent::detail::range_element_t<decltype(items)> item : items)
            this->value(item);
    }
}

} // namespace serpent::msgpack
