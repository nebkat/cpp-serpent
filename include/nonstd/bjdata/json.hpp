#pragma once

// JSON output. Write only - there is no JSON parser here, and BJData remains the format
// anything is read from.
//
// Two ways in. A json_writer emits directly, so a type using bjdata_convert or
// BJDATA_DEFINE_TYPE serialises to JSON with no intermediate at all: those take `auto
// &visitor` and never name the BJData writer. And write_json(sink, view) walks a document
// that already exists, which covers the to_bjdata form and is what you want for dumping a
// stored .bjd.

#include <nonstd/bjdata/emitter.hpp>
#include <nonstd/bjdata/ndarray.hpp>
#include <nonstd/bjdata/real_format.hpp>
#include <nonstd/bjdata/serializer.hpp>
#include <nonstd/bjdata/view.hpp>
#include <nonstd/bjdata/writer.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cstddef>
#include <cstdint>

namespace nonstd::bjdata {

struct json_options {
    /** Spaces per nesting level. Zero writes the document on one line. */
    std::size_t indent = 0;
};

class json_array_scope;
class json_object_scope;
class json_write_visitor;
class json_writer;

void write_json_value(json_writer &out, view source);

/** @brief Emits JSON text into a sink. */
class json_writer : public byte_emitter {
    json_options options {};
    std::uint32_t written_mask = 0;   ///< bit i: an element has already been written at level i
    bool pending_value = false;       ///< a key has been written and owes a value

    friend class json_array_scope;
    friend class json_object_scope;

    void indent_to(int level) noexcept {
        if (this->options.indent == 0) return;
        this->put_text("\n");
        static constexpr std::string_view spaces = "                                ";
        auto remaining = static_cast<std::size_t>(level) * this->options.indent;
        while (remaining > 0) {
            const auto chunk = std::min(remaining, spaces.size());
            this->put_text(spaces.substr(0, chunk));
            remaining -= chunk;
        }
    }

    /** Comma and indentation for the next element at this level. */
    void separate() noexcept {
        if (this->depth == 0) return;
        const auto bit = 1u << (this->depth - 1);
        if ((this->written_mask & bit) != 0) this->put_text(",");
        this->written_mask |= bit;
        this->indent_to(this->depth);
    }

    /** Called before every value: an array element separates, an object value follows its key. */
    void begin_value() noexcept {
        if (this->depth == 0) return;
        if (this->inside_object()) {
            if (!this->pending_value) {
                this->fail(errc::key_outside_object);
                return;
            }
            this->pending_value = false;
            return;
        }
        this->separate();
    }

    void write_quoted(std::string_view text) noexcept {
        this->put_text("\"");
        std::size_t run = 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
            const auto value = static_cast<unsigned char>(text[index]);
            std::string_view escape;
            char escaped[7] = { '\\', 'u', '0', '0', '0', '0', '\0' };
            switch (value) {
                case '"':  escape = "\\\""; break;
                case '\\': escape = "\\\\"; break;
                case '\b': escape = "\\b"; break;
                case '\f': escape = "\\f"; break;
                case '\n': escape = "\\n"; break;
                case '\r': escape = "\\r"; break;
                case '\t': escape = "\\t"; break;
                default:
                    if (value < 0x20) {
                        static constexpr char digits[] = "0123456789abcdef";
                        escaped[4] = digits[(value >> 4) & 0xF];
                        escaped[5] = digits[value & 0xF];
                        escape = std::string_view { escaped, 6 };
                    }
                    break;
            }
            if (escape.empty()) continue;

            // Flush the unescaped run in one write, then the escape.
            this->put_text(text.substr(run, index - run));
            this->put_text(escape);
            run = index + 1;
        }
        this->put_text(text.substr(run));
        this->put_text("\"");
    }

    void begin_array() noexcept {
        this->begin_value();
        this->put_text("[");
        if (!this->push(false)) return;
        this->written_mask &= ~(1u << (this->depth - 1));
    }

    void end_array() noexcept {
        const bool populated = this->depth > 0 && (this->written_mask & (1u << (this->depth - 1))) != 0;
        if (!this->pop(false)) return;
        if (populated) this->indent_to(this->depth);
        this->put_text("]");
    }

    void begin_object() noexcept {
        this->begin_value();
        this->put_text("{");
        if (!this->push(true)) return;
        this->written_mask &= ~(1u << (this->depth - 1));
    }

    void end_object() noexcept {
        const bool populated = this->depth > 0 && (this->written_mask & (1u << (this->depth - 1))) != 0;
        if (!this->pop(true)) return;
        if (populated) this->indent_to(this->depth);
        this->put_text("}");
    }

public:
    template<sink S>
    explicit json_writer(S &out, json_options options = {}) noexcept: byte_emitter(out), options(options) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit json_writer(F &callable, json_options options = {}) noexcept: byte_emitter(callable), options(options) {}

    void null() noexcept {
        this->begin_value();
        this->put_text("null");
    }

    void boolean(bool value) noexcept {
        this->begin_value();
        this->put_text(value ? "true" : "false");
    }

    void integer(std::int64_t value) noexcept { this->number(value); }
    void integer(std::uint64_t value) noexcept { this->number(value); }

    /**
     * JSON has no NaN or infinity, so a non-finite value is written as null.
     *
     * Finite values use the same rendering as the reference implementation's own JSON output,
     * so a document transcribed here is byte-identical to what dart-bjdata prints for it.
     */
    void real(double value) noexcept {
        this->begin_value();
        if (!std::isfinite(value)) {
            this->put_text("null");
            return;
        }
        this->put_text(detail::format_real(value));
    }

    void string(std::string_view text) noexcept {
        this->begin_value();
        this->write_quoted(text);
    }

    void character(char value) noexcept { this->string(std::string_view { &value, 1 }); }

    /** BJData's high precision type is a number too wide for a double; written unquoted. */
    void high_precision(std::string_view digits) noexcept {
        this->begin_value();
        this->put_text(digits.empty() ? "0" : digits);
    }

    void key(std::string_view name) noexcept {
        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        this->separate();
        this->write_quoted(name);
        this->put_text(":");
        if (this->options.indent != 0) this->put_text(" ");
        this->pending_value = true;
    }

    [[nodiscard]] json_array_scope array() noexcept;
    [[nodiscard]] json_object_scope object() noexcept;

    template<typename T>
    void value(const T &item) noexcept {
        emit_value(*this, item);
    }

    /** Binary has no JSON spelling; written as an array of integers. */
    template<detail::byte_range R>
    void bytes(const R &items) noexcept;

    template<std::ranges::input_range R>
    void range(const R &items) noexcept;

    template<typename T>
    void emit_custom(const T &item) noexcept;

private:
    template<typename T>
    void number(T value) noexcept {
        this->begin_value();
        char buffer[24];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
        this->put_text(std::string_view { buffer, static_cast<std::size_t>(converted.ptr - buffer) });
    }
};

class json_array_scope {
    json_writer *out = nullptr;

public:
    explicit json_array_scope(json_writer &out) noexcept: out(&out) { this->out->begin_array(); }
    json_array_scope(const json_array_scope &) = delete;
    json_array_scope &operator=(const json_array_scope &) = delete;
    json_array_scope(json_array_scope &&other) noexcept: out(std::exchange(other.out, nullptr)) {}
    json_array_scope &operator=(json_array_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_array();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~json_array_scope() {
        if (this->out != nullptr) this->out->end_array();
    }

    template<typename T>
    void value(const T &item) const noexcept { this->out->value(item); }
};

class json_object_scope {
    json_writer *out = nullptr;

public:
    explicit json_object_scope(json_writer &out) noexcept: out(&out) { this->out->begin_object(); }
    json_object_scope(const json_object_scope &) = delete;
    json_object_scope &operator=(const json_object_scope &) = delete;
    json_object_scope(json_object_scope &&other) noexcept: out(std::exchange(other.out, nullptr)) {}
    json_object_scope &operator=(json_object_scope &&other) noexcept {
        if (this != &other) {
            if (this->out != nullptr) this->out->end_object();
            this->out = std::exchange(other.out, nullptr);
        }
        return *this;
    }
    ~json_object_scope() {
        if (this->out != nullptr) this->out->end_object();
    }

    template<typename T>
    void member(std::string_view name, const T &item) const noexcept {
        this->out->key(name);
        this->out->value(item);
    }
};

inline json_array_scope json_writer::array() noexcept { return json_array_scope { *this }; }
inline json_object_scope json_writer::object() noexcept { return json_object_scope { *this }; }

/** Names each field on the way out, exactly as write_visitor does for BJData. */
class json_write_visitor {
    json_writer *out = nullptr;

public:
    static constexpr bool is_reading = false;

    explicit json_write_visitor(json_writer &out) noexcept: out(&out) {}

    template<typename T>
    void member(std::string_view name, const T &value) const noexcept {
        this->out->key(name);
        this->out->value(value);
    }

    [[nodiscard]] json_writer &target() const noexcept { return *this->out; }
};

template<detail::byte_range R>
void json_writer::bytes(const R &items) noexcept {
    const auto scope = this->array();
    for (const auto item : items) this->integer(static_cast<std::uint64_t>(std::to_integer<unsigned char>(item)));
}

template<std::ranges::input_range R>
void json_writer::range(const R &items) noexcept {
    const auto scope = this->array();
    for (const auto &item : items) emit_value(*this, item);
}

template<typename T>
void json_writer::emit_custom(const T &item) noexcept {
    using bare = std::remove_cvref_t<T>;
    if constexpr (convertible_type<bare>) {
        // The archive form never names the BJData writer, so it serves JSON unchanged.
        json_write_visitor visitor { *this };
        const auto scope = this->object();
        bjdata_convert(visitor, item);
    } else {
        // A to_bjdata-only type is bound to the BJData writer, so it goes through a document.
        std::vector<std::byte> buffer;
        container_sink intermediate { buffer };
        writer target { intermediate };
        target.value(item);
        if (!target.finish()) {
            this->fail(errc::type_mismatch);
            return;
        }
        write_json_value(*this, view::over(buffer));
    }
}

/** An N-D array is nested rather than flattened, which is what a JSON consumer expects. */
inline void write_json_ndarray(json_writer &out, const ndarray_view &source) {
    if (source.rank() == 0) {
        write_json_value(out, source.value());
        return;
    }
    const auto scope = out.array();
    for (std::size_t index = 0; index < source.size(); ++index) write_json_ndarray(out, source.at(index));
}

/** Transcribes one BJData value, and everything under it, as JSON. */
inline void write_json_value(json_writer &out, view source) {
    switch (source.type()) {
        case kind::null:
            out.null();
            return;
        case kind::boolean:
            out.boolean(source.as_bool() == true);
            return;
        case kind::integer:
            if (source.type_marker() == marker::uint64) out.integer(source.as_int<std::uint64_t>().value_or(0));
            else out.integer(source.as_int<std::int64_t>().value_or(0));
            return;
        case kind::real:
            out.real(source.as_float<double>().value_or(0.0));
            return;
        case kind::string: {
            const auto text = source.as_string().value_or("");
            if (source.type_marker() == marker::high_precision) out.high_precision(text);
            else out.string(text);
            return;
        }
        case kind::array: {
            if (const auto shaped = as_ndarray(source); shaped && shaped->rank() > 1) {
                write_json_ndarray(out, *shaped);
                return;
            }
            const auto scope = out.array();
            for (const auto element : source.array()) write_json_value(out, element);
            return;
        }
        case kind::object: {
            const auto scope = out.object();
            for (const auto [name, element] : source.items()) {
                out.key(name);
                write_json_value(out, element);
            }
            return;
        }
        default:
            out.fail(errc::type_mismatch);
            return;
    }
}

/**
 * Writes an existing BJData document to a sink as JSON.
 *
 * The value is checked once here rather than at every level: the reader is deliberately
 * total, so walking a truncated document would otherwise emit whatever happened to parse and
 * call it a success. payload_bytes() costs a single pass over the value.
 */
template<sink S>
std::expected<std::size_t, error> write_json(S &out, view source, json_options options = {}) {
    json_writer target { out, options };
    if (!source.payload_bytes()) {
        target.fail(errc::unexpected_end);
        return target.finish();
    }
    write_json_value(target, source);
    return target.finish();
}

/** Writes a C++ value to a sink as JSON. */
template<sink S, typename T>
    requires (!std::same_as<std::remove_cvref_t<T>, view>)
std::expected<std::size_t, error> write_json(S &out, const T &value, json_options options = {}) {
    json_writer target { out, options };
    target.value(value);
    return target.finish();
}

/** The allocating convenience over write_json. Empty when the document does not parse. */
[[nodiscard]] inline std::string to_json(view source, json_options options = {}) {
    std::string text;
    container_sink out { text };
    if (!write_json(out, source, options)) text.clear();
    return text;
}

template<typename T>
    requires (!std::same_as<std::remove_cvref_t<T>, view>)
[[nodiscard]] std::string to_json(const T &value, json_options options = {}) {
    std::string text;
    container_sink out { text };
    json_writer target { out, options };
    target.value(value);
    if (!target.finish()) text.clear();
    return text;
}

}// namespace nonstd::bjdata
