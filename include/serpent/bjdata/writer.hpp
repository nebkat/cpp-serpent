#pragma once

#include <serpent/emitter.hpp>
#include <serpent/error.hpp>
#include <serpent/fwd.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/sink.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
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

class array_scope;
class object_scope;

/** What the writer is allowed to do to shrink the output. Mirrors dart-bjdata's BjdataConfig. */
struct writer_options {
    /** Choose the narrowest marker that holds a value exactly. */
    bool compact_types = true;
    /** Rewrite a uniform numeric list as [$T#n when that is strictly smaller. */
    bool numeric_packing = true;
};

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

}// namespace detail

/** @brief Emits BJData into a sink, holding no buffer of its own. */
class writer : public byte_emitter {
    writer_options options {};

    friend class array_scope;
    friend class object_scope;

    void begin_array() noexcept {
        if (!this->push(false)) return;
        this->put_marker(marker::array_begin);
    }
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
            for (const auto item : values) maximum = std::max<std::uint64_t>(maximum, item);
            return integer_marker(maximum);
        }
    }

public:
    template<sink S>
    explicit writer(S &out, writer_options options = {}) noexcept: byte_emitter(out), options(options) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit writer(F &callable, writer_options options = {}) noexcept: byte_emitter(callable), options(options) {}

    [[nodiscard]] const writer_options &configuration() const noexcept { return this->options; }

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
            case marker::uint8:  this->put_raw(static_cast<std::uint8_t>(bits)); return;
            case marker::int8:   this->put_raw(static_cast<std::int8_t>(bits)); return;
            case marker::uint16: this->put_raw(static_cast<std::uint16_t>(bits)); return;
            case marker::int16:  this->put_raw(static_cast<std::int16_t>(bits)); return;
            case marker::uint32: this->put_raw(static_cast<std::uint32_t>(bits)); return;
            case marker::int32:  this->put_raw(static_cast<std::int32_t>(bits)); return;
            case marker::uint64: this->put_raw(static_cast<std::uint64_t>(bits)); return;
            case marker::int64:  this->put_raw(static_cast<std::int64_t>(bits)); return;
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
        const auto kind = this->options.compact_types ? integer_marker(value, value) : marker::int64;
        this->put_marker(kind);
        this->put_integer_payload(kind, static_cast<std::uint64_t>(value));
    }

    void integer(std::uint64_t value) noexcept {
        const auto kind = !this->options.compact_types ? marker::uint64
                : value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? integer_marker(value)
                : integer_marker(static_cast<std::int64_t>(value), static_cast<std::int64_t>(value));
        this->put_marker(kind);
        this->put_integer_payload(kind, value);
    }

    void real(double value) noexcept {
        const auto kind = this->options.compact_types ? float_marker(value) : marker::float64;
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

    /** A [$B#n array, which is how binary round-trips through nlohmann's get_binary(). */
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

    [[nodiscard]] array_scope array() noexcept;
    [[nodiscard]] object_scope object() noexcept;

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
            if (this->options.compact_types && !values.empty()) {
                const auto narrowed = writer::narrowest_marker(values);
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
            for (const auto item : values) this->put_integer_payload(element, static_cast<std::uint64_t>(item));
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
class array_scope {
    writer *out = nullptr;

public:
    explicit array_scope(writer &out) noexcept: out(&out) { this->out->begin_array(); }

    array_scope(const array_scope &) = delete;
    array_scope &operator=(const array_scope &) = delete;

    array_scope(array_scope &&other) noexcept: out(std::exchange(other.out, nullptr)) {}
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
    void value(const T &item) const noexcept { this->out->value(item); }
};

class object_scope {
    writer *out = nullptr;

public:
    explicit object_scope(writer &out) noexcept: out(&out) { this->out->begin_object(); }

    object_scope(const object_scope &) = delete;
    object_scope &operator=(const object_scope &) = delete;

    object_scope(object_scope &&other) noexcept: out(std::exchange(other.out, nullptr)) {}
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

inline array_scope writer::array() noexcept { return array_scope { *this }; }
inline object_scope writer::object() noexcept { return object_scope { *this }; }

/**
 * A list, written the way the reference encoder writes one.
 *
 * A generic array gives every value its own marker and so stores each at its own width,
 * while a strongly typed array must be wide enough for the largest value and pays that width
 * throughout. Which wins depends entirely on the spread of the values, so both are measured
 * - in closed form, with no buffering. A tie keeps the generic form.
 */
template<std::ranges::input_range R>
void writer::range(const R &items) noexcept {
    using element = std::remove_cvref_t<std::ranges::range_value_t<R>>;
    constexpr bool packable = std::ranges::forward_range<R>
            && ((std::integral<element> && !std::same_as<element, bool> && !std::same_as<element, char>)
                || std::floating_point<element>);

    if constexpr (packable) {
        if (this->options.numeric_packing && this->options.compact_types && !std::ranges::empty(items)) {
            const auto count = static_cast<std::size_t>(std::ranges::distance(items));
            std::size_t generic = 2;   // '[' and ']'
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
                element_marker = above_signed_range ? integer_marker(unsigned_maximum)
                                                    : integer_marker(minimum, maximum);
            }

            const auto count_marker = integer_marker(static_cast<std::int64_t>(count), static_cast<std::int64_t>(count));
            // '[' '$' type '#', then the count with its own marker, then the payload.
            const std::size_t packed = 4 + (1 + payload_width(count_marker)) + count * payload_width(element_marker);

            if (packed < generic) {
                this->put_marker(marker::array_begin);
                this->put_marker(marker::strong_type);
                this->put_marker(element_marker);
                this->put_marker(marker::count);
                this->put_length(count);
                for (const auto item : items) {
                    if constexpr (std::floating_point<element>) {
                        this->put_float_payload(element_marker, static_cast<double>(item));
                    } else if constexpr (std::is_signed_v<element>) {
                        this->put_integer_payload(element_marker,
                                                  static_cast<std::uint64_t>(static_cast<std::int64_t>(item)));
                    } else {
                        this->put_integer_payload(element_marker, static_cast<std::uint64_t>(item));
                    }
                }
                return;
            }
        }
    }

    const auto scope = this->array();
    for (const auto &item : items) this->value(item);
}

}// namespace serpent::bjdata
