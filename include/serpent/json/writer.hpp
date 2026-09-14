#pragma once

// JSON output. Write only - there is no JSON parser here, and BJData remains the format
// anything is read from.
//
// Two ways in. A writer emits directly, so a type using json_convert or
// a reflected type serialises to JSON with no intermediate at all: those take `auto
// &visitor` and never name the BJData writer. And write_TMP(sink, view) walks a document
// that already exists, which is what you want for dumping a stored .bjd.

#include <serpent/emitter.hpp>
#include <serpent/real_format.hpp>
#include <serpent/serializer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <cstddef>
#include <cstdint>

namespace serpent::json {

struct writer_options {
    /** Spaces per nesting level. Zero writes the document on one line. */
    std::size_t indent = 0;
};

class array_scope;
class object_scope;
class writer;

/** @brief Emits JSON text into a sink. */
class writer : public byte_emitter {
    writer_options options {};
    std::uint32_t written_mask = 0; ///< bit i: an element has already been written at level i
    bool pending_value = false; ///< a key has been written and owes a value

    friend class array_scope;
    friend class object_scope;

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
            case '"': escape = "\\\""; break;
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
    explicit writer(S &out, writer_options options = {}) noexcept : byte_emitter(out)
                                                                  , options(options) {}

    template<typename F>
        requires (!sink<F> && std::invocable<F &, std::span<const std::byte>>)
    explicit writer(F &callable, writer_options options = {}) noexcept : byte_emitter(callable)
                                                                       , options(options) {}

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
     * so a document transcribed here is byte-identical to what the reference implementation prints for it.
     */
    void real(double value) noexcept {
        this->begin_value();
        if (!std::isfinite(value)) {
            this->put_text("null");
            return;
        }
        this->put_text(detail::format_real(value).view());
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

    /**
     * A constant key, framed once at compile time: the quotes, the bytes and the colon.
     *
     * Only for a name that needs no escaping and only when not indenting, so the general path
     * below stays the one that decides what a key looks like.
     */
    template<const std::string_view &Name>
    void key_literal() noexcept {
        static constexpr bool plain = [] {
            for (const char value : Name)
                if (value == '"' || value == '\\' || static_cast<unsigned char>(value) < 0x20) return false;
            return true;
        }();

        if (this->options.indent != 0 || !plain) {
            this->key(Name);
            return;
        }

        static constexpr auto framed = [] {
            std::array<char, Name.size() + 3> text {};
            text[0] = '"';
            for (std::size_t index = 0; index < Name.size(); ++index)
                text[1 + index] = Name[index];
            text[Name.size() + 1] = '"';
            text[Name.size() + 2] = ':';
            return text;
        }();

        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        this->separate();
        this->put_text(std::string_view { framed.data(), framed.size() });
        this->pending_value = true;
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

    [[nodiscard]] array_scope array() noexcept;
    [[nodiscard]] object_scope object() noexcept;

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

class array_scope {
    writer *out = nullptr;

public:
    explicit array_scope(writer &out) noexcept : out(&out) { this->out->begin_array(); }
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

class object_scope {
    writer *out = nullptr;

public:
    explicit object_scope(writer &out) noexcept : out(&out) { this->out->begin_object(); }
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

inline array_scope writer::array() noexcept { return array_scope { *this }; }
inline object_scope writer::object() noexcept { return object_scope { *this }; }

template<detail::byte_range R>
void writer::bytes(const R &items) noexcept {
    const auto scope = this->array();
    for (const auto item : items)
        this->integer(static_cast<std::uint64_t>(std::to_integer<unsigned char>(item)));
}

template<std::ranges::input_range R>
void writer::range(const R &items) noexcept {
    const auto scope = this->array();
    for (const auto &item : items)
        emit_value(*this, item);
}

template<typename T>
void writer::emit_custom(const T &item) noexcept {
    // serializer dispatches on the writer: json_convert serves every format, and
    // to_json is resolved against this writer, so a type may have an overload for JSON
    // specifically. A type with neither is a compile error naming both options.
    serializer<std::remove_cvref_t<T>>::write(*this, item);
}

/** Writes a C++ value to a sink as JSON. */
template<sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value, writer_options options = {}) {
    writer target { out, options };
    target.value(value);
    return target.finish();
}

/** The allocating convenience over write(). */
template<typename T>
[[nodiscard]] std::string encode(const T &value, writer_options options = {}) {
    std::string text;
    container_sink out { text };
    writer target { out, options };
    target.value(value);
    if (!target.finish()) text.clear();
    return text;
}

} // namespace serpent::json
