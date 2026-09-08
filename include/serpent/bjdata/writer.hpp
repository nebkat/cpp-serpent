#pragma once

#include <serpent/emitter.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/sink.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
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

namespace serpent::bjdata {

/**
 * What the writer is allowed to do to shrink the output. Mirrors the reference encoder's configuration.
 *
 * A template argument rather than a member, so a build that does not want an optimization
 * does not carry its code. The measuring and marker-selection routines themselves stay
 * ordinary free functions in marker.hpp - this decides whether to call them, it does not
 * reimplement anything.
 */
struct writer_options {
    /** Choose the narrowest marker that holds a value exactly. */
    bool compact_types = true;
    /** Rewrite a uniform numeric list as [$T#n when that is strictly smaller. */
    bool numeric_packing = true;
    /**
     * How much larger the output may be to gain a single-copy payload, in percent.
     *
     * A contiguous range packed at the element's own width is already in wire order and goes
     * out in one copy; narrowed or generic costs a store per element. Zero reproduces the
     * reference encoder, which only asks which is smaller. Five buys the copy where it is
     * nearly free - a thousand real doubles are 7802 bytes generic against 8007 copied - and
     * still refuses it where it costs four times the space.
     */
    unsigned copy_tolerance_percent = 0;
    /**
     * Give a sized array its element count, as [#n rather than an unbounded [, once it holds
     * at least this many elements.
     *
     * A reader that knows how many elements are coming sizes its container once instead of
     * growing it. The count costs two bytes, and what it buys depends on how many elements
     * there are: at one it saves nothing, at two it saves one allocation, at three it saves
     * two, at a thousand it saves ten and a megabyte of copying. Three is where it stops being
     * arguable.
     *
     * Set it to `never_counted` for output byte-identical to the reference encoder, which
     * writes a count only beside a type marker.
     */
    std::size_t counted_containers_from = 3;
};

/** For writer_options::counted_containers_from: never write a bare count. */
inline constexpr std::size_t never_counted = std::numeric_limits<std::size_t>::max();

template<writer_options Options>
class array_scope;
template<writer_options Options>
class object_scope;

namespace detail {

/** The bytes one value costs in a generic array, its own marker included. */
template<typename T>
[[nodiscard]] constexpr std::size_t generic_value_size(T item) noexcept {
    if constexpr (std::floating_point<T>) {
        return 1 + payload_width(float_marker(static_cast<double>(item)));
    } else if constexpr (std::is_signed_v<T>) {
        return 1 + payload_width(integer_marker(item, item));
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return 1 + payload_width(integer_marker(static_cast<std::uint64_t>(item)));
    } else {
        const auto widened = static_cast<std::int64_t>(item);
        return 1 + payload_width(integer_marker(widened, widened));
    }
}

} // namespace detail

/** @brief Emits BJData into a sink, holding no buffer of its own. */
template<writer_options Options = writer_options {}>
class basic_writer : public byte_emitter {
    friend class array_scope<Options>;
    friend class object_scope<Options>;

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

    template<typename T>
    [[nodiscard]] static marker narrowest_marker(std::span<const T> values) noexcept {
        if constexpr (std::is_signed_v<T>) {
            std::int64_t minimum = values.front();
            std::int64_t maximum = minimum;
            for (const auto item : values) {
                minimum = std::min<std::int64_t>(minimum, item);
                maximum = std::max<std::int64_t>(maximum, item);
            }
            return integer_marker(minimum, maximum);
        } else {
            std::uint64_t maximum = 0;
            for (const auto item : values)
                maximum = std::max<std::uint64_t>(maximum, item);
            return integer_marker(maximum);
        }
    }

public:
    template<sink S>
    explicit basic_writer(S &out) noexcept : byte_emitter(out) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit basic_writer(F &callable) noexcept : byte_emitter(callable) {}

    static constexpr writer_options configuration = Options;

    // ---------------- raw output ----------------

    void put_marker(marker value) noexcept {
        const auto byte = static_cast<std::byte>(value);
        this->put(std::span<const std::byte> { &byte, 1 });
    }

    template<typename T>
    void put_raw(T value) noexcept {
        const nonstd::unaligned_little<T> storage { value };
        this->put(std::span<const std::byte> { storage.data(), nonstd::unaligned_little<T>::storage_bytes });
    }

    /** The payload of an integer marker, truncated to that marker's width. */
    void put_integer_payload(marker kind, std::uint64_t bits) noexcept {
        switch (kind) {
        case marker::uint8: this->put_raw(static_cast<std::uint8_t>(bits)); return;
        case marker::int8: this->put_raw(static_cast<std::int8_t>(bits)); return;
        case marker::uint16: this->put_raw(static_cast<std::uint16_t>(bits)); return;
        case marker::int16: this->put_raw(static_cast<std::int16_t>(bits)); return;
        case marker::uint32: this->put_raw(static_cast<std::uint32_t>(bits)); return;
        case marker::int32: this->put_raw(static_cast<std::int32_t>(bits)); return;
        case marker::uint64: this->put_raw(static_cast<std::uint64_t>(bits)); return;
        case marker::int64: this->put_raw(static_cast<std::int64_t>(bits)); return;
        case marker::byte:
        case marker::character: this->put_raw(static_cast<std::uint8_t>(bits)); return;
        default: this->fail(errc::unexpected_marker); return;
        }
    }

    void put_float_payload(marker kind, double value) noexcept {
        switch (kind) {
        case marker::float16: this->put_raw(encode_float16(static_cast<float>(value))); return;
        case marker::float32: this->put_raw(static_cast<float>(value)); return;
        case marker::float64: this->put_raw(value); return;
        default: this->fail(errc::unexpected_marker); return;
        }
    }

    /** A length or count: its own compact marker, then its value. Never a bare width. */
    void put_length(std::uint64_t length) noexcept {
        const auto kind = length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? integer_marker(length)
                : integer_marker(static_cast<std::int64_t>(length), static_cast<std::int64_t>(length));
        this->put_marker(kind);
        this->put_integer_payload(kind, length);
    }

    // ---------------- scalars ----------------

    void null() noexcept { this->put_marker(marker::null); }
    void noop() noexcept { this->put_marker(marker::noop); }
    void boolean(bool value) noexcept { this->put_marker(value ? marker::boolean_true : marker::boolean_false); }

    void character(char value) noexcept {
        this->put_marker(marker::character);
        this->put_raw(static_cast<std::uint8_t>(value));
    }

    void integer(std::int64_t value) noexcept {
        const auto kind = Options.compact_types ? integer_marker(value, value) : marker::int64;
        this->put_marker(kind);
        this->put_integer_payload(kind, static_cast<std::uint64_t>(value));
    }

    void integer(std::uint64_t value) noexcept {
        const auto kind = !Options.compact_types ? marker::uint64
                : value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? integer_marker(value)
                : integer_marker(static_cast<std::int64_t>(value), static_cast<std::int64_t>(value));
        this->put_marker(kind);
        this->put_integer_payload(kind, value);
    }

    void real(double value) noexcept {
        const auto kind = Options.compact_types ? float_marker(value) : marker::float64;
        this->put_marker(kind);
        this->put_float_payload(kind, value);
    }

    void string(std::string_view text) noexcept {
        this->put_marker(marker::string);
        this->put_length(text.size());
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
        this->put_marker(marker::array_begin);
        this->put_marker(marker::strong_type);
        this->put_marker(marker::byte);
        this->put_marker(marker::count);
        this->put_length(bytes.size());
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
        static constexpr auto preamble = [] {
            constexpr auto kind =
                    integer_marker(static_cast<std::int64_t>(Name.size()), static_cast<std::int64_t>(Name.size()));
            std::array<std::byte, 1 + payload_width(kind) + Name.size()> bytes {};
            bytes[0] = static_cast<std::byte>(kind);
            for (std::size_t index = 0; index < payload_width(kind); ++index)
                bytes[1 + index] = static_cast<std::byte>((Name.size() >> (8 * index)) & 0xFF);
            for (std::size_t index = 0; index < Name.size(); ++index)
                bytes[1 + payload_width(kind) + index] = static_cast<std::byte>(Name[index]);
            return bytes;
        }();

        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        this->put(preamble);
    }

    [[nodiscard]] array_scope<Options> array() noexcept;
    [[nodiscard]] object_scope<Options> object() noexcept;

    /**
     * A strongly typed array written in place: header, then the payload in one copy.
     *
     * Unlike a plain list it is always given a typed header, even when empty, and with
     * compact_types it re-types the elements only when the narrower marker is strictly fewer
     * bytes - never demoting to a generic array even where that would be smaller.
     */
    template<typename T>
    void typed_array(std::span<const T> values) noexcept {
        constexpr marker declared = strong_type_for<T>();
        static_assert(declared != marker::invalid, "T does not correspond to a BJData strong type");

        marker element = declared;
        if constexpr (std::integral<T> && !std::same_as<T, char>) {
            if constexpr (Options.compact_types)
                if (!values.empty()) {
                    const auto narrowed = basic_writer::narrowest_marker(values);
                    if (payload_width(narrowed) < payload_width(declared)) element = narrowed;
                }
        }

        this->put_marker(marker::array_begin);
        this->put_marker(marker::strong_type);
        this->put_marker(element);
        this->put_marker(marker::count);
        this->put_length(values.size());

        if (element == declared) {
            // Native order is the wire order, so the payload goes out as one copy.
            this->put(std::as_bytes(values));
        } else {
            for (const auto item : values)
                this->put_integer_payload(element, static_cast<std::uint64_t>(item));
        }
    }

    template<typename T>
    void typed_array(nonstd::unaligned_little_span<const T> values) noexcept {
        this->put_marker(marker::array_begin);
        this->put_marker(marker::strong_type);
        this->put_marker(strong_type_for<T>());
        this->put_marker(marker::count);
        this->put_length(values.size());
        this->put(values.bytes());
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
template<writer_options Options>
class array_scope {
    basic_writer<Options> *out = nullptr;

public:
    explicit array_scope(basic_writer<Options> &out) noexcept : out(&out) { this->out->begin_array(); }

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

template<writer_options Options>
class object_scope {
    basic_writer<Options> *out = nullptr;

public:
    explicit object_scope(basic_writer<Options> &out) noexcept : out(&out) { this->out->begin_object(); }

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

template<writer_options Options>
array_scope<Options> basic_writer<Options>::array() noexcept {
    return array_scope<Options> { *this };
}

template<writer_options Options>
object_scope<Options> basic_writer<Options>::object() noexcept {
    return object_scope<Options> { *this };
}

/** The default: everything the reference encoder does. */
using writer = basic_writer<>;

/** Every value at its declared width, with no measuring - and none of that code emitted. */
using plain_writer = basic_writer<writer_options { .compact_types = false, .numeric_packing = false }>;

/**
 * A list, written the way the reference encoder writes one.
 *
 * A generic array gives every value its own marker and so stores each at its own width,
 * while a strongly typed array must be wide enough for the largest value and pays that width
 * throughout. Which wins depends entirely on the spread of the values, so both are measured
 * - in closed form, with no buffering. A tie keeps the generic form.
 */
template<writer_options Options>
template<std::ranges::input_range R>
void basic_writer<Options>::range(const R &items) noexcept {
    using element = std::remove_cvref_t<std::ranges::range_value_t<R>>;
    constexpr bool packable = std::ranges::forward_range<R>
            && ((std::integral<element> && !std::same_as<element, bool> && !std::same_as<element, char>)
                    || std::floating_point<element>);

    if constexpr (packable) {
        if constexpr (Options.numeric_packing && Options.compact_types)
            if (!std::ranges::empty(items)) {
                const auto count = static_cast<std::size_t>(std::ranges::distance(items));

                // What the generic form would frame this with. An unbounded array is '[' and
                // ']'; a counted one is '[' '#' and the count, and comparing against the wrong
                // one of those would leave a typed array unpacked when it is in fact smaller.
                const bool generic_is_counted =
                        Options.counted_containers_from != never_counted && count >= Options.counted_containers_from;
                const auto generic_count_marker =
                        integer_marker(static_cast<std::int64_t>(count), static_cast<std::int64_t>(count));
                std::size_t generic = generic_is_counted ? 3 + payload_width(generic_count_marker) : 2;

                marker element_marker = marker::invalid;

                if constexpr (std::floating_point<element>) {
                    bool all_fit_16 = true;
                    bool all_fit_32 = true;
                    for (const auto item : items) {
                        const auto widened = static_cast<double>(item);
                        all_fit_16 = all_fit_16 && fits_float16(widened);
                        all_fit_32 = all_fit_32 && fits_float32(widened);
                        generic += detail::generic_value_size(item);
                    }
                    element_marker = float_marker(all_fit_16, all_fit_32);
                } else {
                    std::int64_t minimum = 0;
                    std::int64_t maximum = 0;
                    std::uint64_t unsigned_maximum = 0;
                    bool above_signed_range = false;
                    bool first = true;
                    for (const auto item : items) {
                        if constexpr (std::is_signed_v<element>) {
                            const auto widened = static_cast<std::int64_t>(item);
                            minimum = first ? widened : std::min(minimum, widened);
                            maximum = first ? widened : std::max(maximum, widened);
                        } else {
                            const auto widened = static_cast<std::uint64_t>(item);
                            unsigned_maximum = std::max(unsigned_maximum, widened);
                            if (widened > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                                above_signed_range = true;
                            } else {
                                const auto narrowed = static_cast<std::int64_t>(widened);
                                minimum = first ? narrowed : std::min(minimum, narrowed);
                                maximum = first ? narrowed : std::max(maximum, narrowed);
                            }
                        }
                        first = false;
                        generic += detail::generic_value_size(item);
                    }
                    element_marker =
                            above_signed_range ? integer_marker(unsigned_maximum) : integer_marker(minimum, maximum);
                }

                const auto count_marker =
                        integer_marker(static_cast<std::int64_t>(count), static_cast<std::int64_t>(count));
                // '[' '$' type '#', then the count with its own marker, then the payload.
                const std::size_t header = 4 + (1 + payload_width(count_marker));
                std::size_t packed = header + count * payload_width(element_marker);

                // A contiguous range packed at the element's own width copies in one go. Consider
                // it whenever the chosen marker would not, and take it if the size it costs is
                // within what the caller allows.
                if constexpr (std::ranges::contiguous_range<R> && strong_type_for<element>() != marker::invalid) {
                    const std::size_t copyable = header + count * sizeof(element);
                    const std::size_t best = std::min(packed, generic);
                    if (copyable * 100 <= best * (100 + std::size_t { Options.copy_tolerance_percent })) {
                        // Keep the chosen marker when it is already the element's width, so a
                        // positive int32 range stays uint32 as the reference writes it.
                        if (payload_width(element_marker) != sizeof(element)) {
                            element_marker = strong_type_for<element>();
                        }
                        packed = copyable;
                        generic = copyable + 1; // force the packed branch below
                    }
                }

                if (packed < generic) {
                    this->put_marker(marker::array_begin);
                    this->put_marker(marker::strong_type);
                    this->put_marker(element_marker);
                    this->put_marker(marker::count);
                    this->put_length(count);

                    // When the chosen marker stores each element at exactly the width it already
                    // occupies, the payload is in wire order and goes out in one copy rather than
                    // a store per element. The marker need not be the element's own: a positive
                    // int32 range packs as uint32, and two's complement makes those bytes
                    // identical. A caller never asks for this - it is the writer's business, which
                    // is the point of value() taking whatever range you have.
                    if constexpr (std::ranges::contiguous_range<R>) {
                        const bool same_width = payload_width(element_marker) == sizeof(element);
                        const bool same_family = is_float(element_marker) == std::floating_point<element>;
                        if (same_width && same_family) {
                            this->put(std::as_bytes(std::span<const element> { std::ranges::data(items), count }));
                            return;
                        }
                    }

                    for (const auto item : items) {
                        if constexpr (std::floating_point<element>) {
                            this->put_float_payload(element_marker, static_cast<double>(item));
                        } else if constexpr (std::is_signed_v<element>) {
                            this->put_integer_payload(
                                    element_marker, static_cast<std::uint64_t>(static_cast<std::int64_t>(item)));
                        } else {
                            this->put_integer_payload(element_marker, static_cast<std::uint64_t>(item));
                        }
                    }
                    return;
                }
            }
    }

    if constexpr (std::ranges::sized_range<R>) {
        const auto count = static_cast<std::uint64_t>(std::ranges::size(items));
        if (Options.counted_containers_from != never_counted && count >= Options.counted_containers_from) {
            this->begin_counted_array(count);
            for (const auto &item : items)
                this->value(item);
            this->end_counted_array();
            return;
        }
    }

    const auto scope = this->array();
    for (const auto &item : items)
        this->value(item);
}

} // namespace serpent::bjdata
