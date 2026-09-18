#pragma once

#include <serpent/config.hpp>
#include <serpent/emitter.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/sink.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
#include <bit>
#include <array>
#include <concepts>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace serpent::bjdata {

/**
 * What a document is written for.
 *
 * It is either to be as small as it can be, or as quick to write and to read as it can be. There
 * is nothing else to want of an encoding, so there is nothing else to choose, and everything the
 * writer decides follows from which:
 *
 *                        size                                  speed
 *   a number             the narrowest marker that holds it    the marker of its own type
 *   a range of numbers   typed, [$T#n, when that is smaller,   always typed, at its own type: one
 *                        at the narrowest marker for them all  copy to write, one to read, a span to reader
 *   any other range      unbounded, since a count costs bytes  counted, so a reader sizes its container once
 *
 * Both are read by the same reader, which takes whatever marker it finds.
 */
enum class prefer { size, speed };

// Defaulted here, the way basic_writer is, so a scope can be named in a signature without
// spelling out an options set the caller never chose.
template<prefer Preference = prefer::size>
class array_scope;
template<prefer Preference = prefer::size>
class object_scope;

namespace detail {

/** The marker a number goes under when nothing is done to it: the one of its own type and width. */
template<typename T>
[[nodiscard]] consteval marker own_marker() noexcept {
    if constexpr (std::floating_point<T>) {
        return sizeof(T) == sizeof(float) ? marker::float32 : marker::float64;
    } else if constexpr (std::is_signed_v<T>) {
        return sizeof(T) == 1 ? marker::int8 : sizeof(T) == 2 ? marker::int16 : sizeof(T) == 4 ? marker::int32 : marker::int64;
    } else {
        return sizeof(T) == 1 ? marker::uint8
                : sizeof(T) == 2 ? marker::uint16
                : sizeof(T) == 4 ? marker::uint32
                                 : marker::uint64;
    }
}

} // namespace detail

/** @brief Emits BJData into a sink, holding no buffer of its own. */
template<prefer Preference = prefer::size>
class basic_writer : public byte_emitter {
    friend class array_scope<Preference>;
    friend class object_scope<Preference>;

    void begin_array() noexcept {
        if (!this->push(false)) return;
        this->put_marker(marker::array_begin);
    }
    /** A count with no type marker: the elements still carry their own. */
    void begin_counted_array(std::uint64_t count) noexcept {
        if (!this->push(false)) return;
        this->put_marker(marker::array_begin);
        this->put_marker(marker::count);
        this->put_length(count);
    }
    /** A counted container ends when its count runs out, so nothing closes it. */
    void end_counted_array() noexcept { this->pop(false); }
    void end_array() noexcept {
        if (!this->pop(false)) return;
        this->put_marker(marker::array_end);
    }
    void begin_object() noexcept {
        if (!this->push(true)) return;
        this->put_marker(marker::object_begin);
    }
    void end_object() noexcept {
        if (!this->pop(true)) return;
        this->put_marker(marker::object_end);
    }

public:
    template<sink S>
    explicit basic_writer(S &out) noexcept : byte_emitter(out) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit basic_writer(F &callable) noexcept : byte_emitter(callable) {}

    static constexpr prefer preference = Preference;

    // ---------------- raw output ----------------

    void put_marker(marker value) noexcept { this->put_byte(static_cast<std::byte>(value)); }

    template<typename T>
    void put_raw(T value) noexcept {
        const nonstd::unaligned_little<T> storage { value };
        this->put(std::span<const std::byte> { storage.data(), nonstd::unaligned_little<T>::storage_bytes });
    }

    /** A length or count: its own compact marker, then its value. Never a bare width. */
    void put_length(std::uint64_t length) noexcept { this->marked(length_form(length)); }

    // ---------------- a scalar as its marker and payload ----------------

    /** A marker, and the payload it announces as the low bytes of one word. */
    struct marked_scalar {
        marker kind;
        std::uint64_t payload;
    };

    /**
     * The one place that says which marker a value on its own goes under: the narrowest that
     * holds it exactly where size is preferred, the one of its own type where speed is.
     *
     * Given the value as the type it has, so that an int32 preferred for speed is four bytes
     * under `l` rather than eight under `L`, and a member written as part of a run and one
     * written on its own cannot come out differently.
     */
    template<typename T>
        requires std::integral<T> || std::floating_point<T>
    [[nodiscard]] static constexpr marked_scalar marked_form(T value) noexcept {
        if constexpr (std::same_as<T, bool>) {
            return { value ? marker::boolean_true : marker::boolean_false, 0 };
        } else if constexpr (Preference == prefer::speed) {
            if constexpr (std::floating_point<T>) {
                using bits = std::conditional_t<sizeof(T) == sizeof(float), std::uint32_t, std::uint64_t>;
                return { detail::own_marker<T>(), std::bit_cast<bits>(value) };
            } else {
                return { detail::own_marker<T>(), static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)) };
            }
        } else if constexpr (std::floating_point<T>) {
            const auto wide = static_cast<double>(value);
            switch (float_marker(wide)) {
            case marker::float16: return { marker::float16, encode_float16(static_cast<float>(wide)) };
            case marker::float32: return { marker::float32, std::bit_cast<std::uint32_t>(static_cast<float>(wide)) };
            default: return { marker::float64, std::bit_cast<std::uint64_t>(wide) };
            }
        } else if constexpr (std::is_signed_v<T>) {
            const auto wide = static_cast<std::int64_t>(value);
            return { integer_marker(wide, wide), static_cast<std::uint64_t>(wide) };
        } else {
            return length_form(static_cast<std::uint64_t>(value));
        }
    }

    /** A length is always under the narrowest marker that holds it, whatever is preferred for values. */
    [[nodiscard]] static constexpr marked_scalar length_form(std::uint64_t length) noexcept {
        const bool beyond_signed = length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return { beyond_signed ? integer_marker(length)
                               : integer_marker(static_cast<std::int64_t>(length), static_cast<std::int64_t>(length)),
            length };
    }

    /** The most write_marked() stores: a marker and the eight bytes of the widest payload. */
    static constexpr std::size_t widest_marked = 1 + sizeof(std::uint64_t);

    /**
     * A marker and its payload, written at a pointer with room for widest_marked bytes.
     *
     * The payload goes out as all eight bytes of the word, and as many of them are kept as the
     * marker says: in little-endian order the low bytes of a wide integer are the narrow one, so
     * there is nothing to choose between but how far to advance.
     */
    SERPENT_ALWAYS_INLINE [[nodiscard]] static char *write_marked(char *to, marked_scalar scalar) noexcept {
        const nonstd::unaligned_little<std::uint64_t> stored { scalar.payload };
        to[0] = static_cast<char>(scalar.kind);
        std::memcpy(to + 1, stored.data(), sizeof(std::uint64_t));
        return to + 1 + payload_width(scalar.kind);
    }

    SERPENT_ALWAYS_INLINE void marked(marked_scalar scalar) noexcept {
        if (char *const to = this->room_for(widest_marked)) {
            this->used(static_cast<std::size_t>(write_marked(to, scalar) - to));
            return;
        }
        // Not nine bytes to be had in one piece - the end of a fixed buffer, where there may
        // still be room for the bytes that are kept - so those and no more, the ordinary way.
        std::array<char, widest_marked> piece {};
        const char *const end = write_marked(piece.data(), scalar);
        this->put_text(std::string_view { piece.data(), end });
    }

    /**
     * Writes whole members of the object that is open - keys and values - into room claimed
     * once for all of them, where each would otherwise ask for its own.
     *
     * `write` is handed room for `at_most` bytes and returns how many it used; what it writes
     * has to be exactly what key() and value() would have. False where the destination has not
     * that much room in one piece, and nothing has been written: the members are then written
     * the usual way, one at a time.
     */
    template<typename Write>
    [[nodiscard]] bool compose_members(std::size_t at_most, Write write) noexcept {
        if (!this->inside_object()) return false;
        char *const to = this->room_for(at_most);
        if (to == nullptr) return false;
        this->used(write(to));
        return true;
    }

    // ---------------- scalars ----------------

    void null() noexcept { this->put_marker(marker::null); }
    void noop() noexcept { this->put_marker(marker::noop); }
    void boolean(bool value) noexcept { this->put_marker(value ? marker::boolean_true : marker::boolean_false); }

    void character(char value) noexcept {
        this->put_marker(marker::character);
        this->put_raw(static_cast<std::uint8_t>(value));
    }

    void integer(std::int64_t value) noexcept { this->marked(marked_form(value)); }
    void integer(std::uint64_t value) noexcept { this->marked(marked_form(value)); }
    void real(double value) noexcept { this->marked(marked_form(value)); }

    /** A number as the type it has, which is how emit_value hands one over to a writer that asks. */
    template<typename T>
        requires (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>
    void number(T value) noexcept {
        this->marked(marked_form(value));
    }

    void string(std::string_view text) noexcept {
        // The marker and the length as one piece where there is room, which there nearly always is.
        if (char *const to = this->room_for(1 + widest_marked)) {
            to[0] = static_cast<char>(marker::string);
            this->used(static_cast<std::size_t>(write_marked(to + 1, length_form(text.size())) - to));
        } else {
            this->put_marker(marker::string);
            this->put_length(text.size());
        }
        this->put_text(text);
    }

    /** Decimal digits, written as-is. Use for values too wide for int64. */
    void high_precision(std::string_view digits) noexcept {
        this->put_marker(marker::high_precision);
        this->put_length(digits.size());
        this->put_text(digits);
    }

    /** A [$B#n array, which is how binary round-trips through DOM libraries. */
    void binary(std::span<const std::byte> bytes) noexcept {
        this->typed_header(marker::byte, bytes.size());
        this->put(bytes);
    }

    // ---------------- containers ----------------

    /** A bare length and UTF-8 bytes. An object key never carries an S marker. */
    void key(std::string_view name) noexcept {
        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        this->put_length(name.size());
        this->put_text(name);
    }

    /** A constant key: its length marker, its length and its bytes are all known, so they are
     *  assembled once at compile time and written in one piece. */
    template<const std::string_view &Name>
    void key_literal() noexcept {
        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        this->put_constant(detail::encoded_key<Name>);
    }

    [[nodiscard]] array_scope<Preference> array() noexcept;
    [[nodiscard]] object_scope<Preference> object() noexcept;

    /**
     * A strongly typed array written in place: header, then the payload in one copy.
     *
     * At the elements' own type whichever is preferred. Narrowing is for a value on its own; a
     * run of numbers narrowed is a store for each where this is one copy, and cannot be viewed
     * as a span of what it was.
     */
    template<typename T>
    void typed_array(std::span<const T> values) noexcept {
        static_assert(strong_type_for<T>() != marker::invalid, "T does not correspond to a BJData strong type");
        this->typed_header(strong_type_for<T>(), values.size());
        this->put(std::as_bytes(values));
    }

    template<typename T>
    void typed_array(nonstd::unaligned_little_span<const T> values) noexcept {
        this->typed_header(strong_type_for<T>(), values.size());
        this->put(values.bytes());
    }

    /** `[$T#n`: what stands before the payload of a typed array. */
    void typed_header(marker element, std::uint64_t count) noexcept {
        this->put_constant(std::array { static_cast<char>(marker::array_begin), static_cast<char>(marker::strong_type),
            static_cast<char>(element), static_cast<char>(marker::count) });
        this->put_length(count);
    }

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

    /** Anything with no built-in meaning: goes to the user's customization. */
    template<typename T>
    void emit_custom(const T &item) noexcept {
        serializer<std::remove_cvref_t<T>>::write(*this, item);
    }

    /** A list: unbounded, or strongly typed when numeric packing pays for itself. */
    template<std::ranges::input_range R>
    void range(const R &items) noexcept;
};

/**
 * @brief Closes its container on destruction. The only way to open one.
 *
 * Movable so that a container may outlive the statement that opened it - hold one in a
 * std::optional to open in one place and close in another, which a state machine or a
 * chunked encoder needs. A moved-from scope closes nothing.
 */
template<prefer Preference>
class array_scope {
    basic_writer<Preference> *out = nullptr;

public:
    explicit array_scope(basic_writer<Preference> &out) noexcept : out(&out) { this->out->begin_array(); }

    array_scope(const array_scope &) = delete;
    array_scope &operator=(const array_scope &) = delete;

    array_scope(array_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    array_scope &operator=(array_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_array();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }

    ~array_scope() {
        if (this->out != nullptr) this->out->end_array();
    }

    template<typename T>
    void value(const T &item) const noexcept {
        this->out->value(item);
    }
};

template<prefer Preference>
class object_scope {
    basic_writer<Preference> *out = nullptr;

public:
    explicit object_scope(basic_writer<Preference> &out) noexcept : out(&out) { this->out->begin_object(); }

    object_scope(const object_scope &) = delete;
    object_scope &operator=(const object_scope &) = delete;

    object_scope(object_scope &&other) noexcept : out(std::exchange(other.out, nullptr)) {}
    object_scope &operator=(object_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_object();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }

    ~object_scope() {
        if (this->out != nullptr) this->out->end_object();
    }

    template<typename T>
    void member(std::string_view name, const T &item) const noexcept {
        this->out->key(name);
        this->out->value(item);
    }
};

template<prefer Preference>
array_scope<Preference> basic_writer<Preference>::array() noexcept {
    return array_scope<Preference> { *this };
}

template<prefer Preference>
object_scope<Preference> basic_writer<Preference>::object() noexcept {
    return object_scope<Preference> { *this };
}

/** The default: everything the reference encoder does. */
using writer = basic_writer<>;
using compact_writer = basic_writer<prefer::size>;
using fast_writer = basic_writer<prefer::speed>;

template<prefer Preference>
template<std::ranges::input_range R>
void basic_writer<Preference>::range(const R &items) noexcept {
    using element = std::remove_cvref_t<std::ranges::range_value_t<R>>;

    // A range of numbers is a typed array at the width the numbers already have, whichever is
    // preferred: a header, and where the range is contiguous one copy. Narrowing is for a value
    // on its own - here it would mean walking the range to measure it and then a store for each
    // element, to produce an array that cannot be read back in one copy either.
    constexpr bool numbers = std::ranges::forward_range<R>
            && ((std::integral<element> && !std::same_as<element, bool> && !std::same_as<element, char>)
                    || std::floating_point<element>);
    if constexpr (numbers) {
        this->typed_header(detail::own_marker<element>(), static_cast<std::uint64_t>(std::ranges::distance(items)));
        if constexpr (std::ranges::contiguous_range<R>) {
            this->put(std::as_bytes(std::span<const element> { std::ranges::data(items), std::ranges::size(items) }));
        } else {
            for (const element item : items) this->put_raw(item);
        }
        return;
    }

    // A count lets a reader size its container once rather than grow it, and costs a few bytes.
    if constexpr (Preference == prefer::speed && std::ranges::sized_range<R>) {
        this->begin_counted_array(static_cast<std::uint64_t>(std::ranges::size(items)));
        for (serpent::detail::range_element_t<decltype(items)> item : items)
            this->value(item);
        this->end_counted_array();
        return;
    }

    const auto scope = this->array();
    for (serpent::detail::range_element_t<decltype(items)> item : items)
        this->value(item);
}

} // namespace serpent::bjdata
