#pragma once

// JSON output. Write only - there is no JSON parser here, and BJData remains the format
// anything is read from.
//
// Two ways in. A writer emits directly, so a type using json_convert or
// a reflected type serialises to JSON with no intermediate at all: those take `auto
// &visitor` and never name the BJData writer. And write_TMP(sink, view) walks a document
// that already exists, which is what you want for dumping a stored .bjd.

#include <serpent/constant_text.hpp>
#include <serpent/emitter.hpp>
#include <serpent/json/scan.hpp>
#include <serpent/json/text.hpp>
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
#include <cstring>

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
    SERPENT_ALWAYS_INLINE void separate() noexcept {
        if (this->depth == 0) return;
        const auto bit = 1u << (this->depth - 1);
        if ((this->written_mask & bit) != 0) this->put_constant(",");
        this->written_mask |= bit;
        this->indent_to(this->depth);
    }

    /** Called before every value: an array element separates, an object value follows its key. */
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
        this->separate();
    }

    /**
     * Writes a string between quotes, escaping what JSON will not hold as itself.
     *
     * Which characters those are is the reader's definition, not a second one: the same table
     * that tells the scanner where a string's plain text stops tells this where to stop copying.
     * Almost every string is plain from end to end and goes out in one piece.
     */
    void write_quoted(std::string_view text) noexcept {
        const char *run = text.data();
        const char *const end = run + text.size();
        const char *stop = scanner::end_of_plain_text(run, end);
        // Nearly every string needs no escape, and then quotes and text are one piece, put into
        // room claimed once.
        if (stop == end) {
            if (char *const to = this->room_for(text.size() + 2)) {
                to[0] = '"';
                std::memcpy(to + 1, run, text.size());
                to[text.size() + 1] = '"';
                this->used(text.size() + 2);
                return;
            }
        }
        this->put_constant("\"");
        while (true) {
            this->put_text(std::string_view { run, stop });
            if (stop == end) break;
            this->write_escape(*stop);
            run = stop + 1;
            stop = scanner::end_of_plain_text(run, end);
        }
        this->put_constant("\"");
    }

public:
    /**
     * `text` between quotes with its escapes, stored at `to`, returning where it ends. For room
     * already claimed: at most six bytes per character, which is what \u00XX takes.
     */
    static char *quote_into(char *to, std::string_view text) noexcept {
        *to++ = '"';
        const char *run = text.data();
        const char *const end = run + text.size();
        while (true) {
            const char *const stop = scanner::end_of_plain_text(run, end);
            std::memcpy(to, run, static_cast<std::size_t>(stop - run));
            to += stop - run;
            if (stop == end) break;
            to = escape_into(to, *stop);
            run = stop + 1;
        }
        *to++ = '"';
        return to;
    }

    /** One character JSON cannot hold as itself, stored at `to`: a short escape where there is one, else \u00XX. */
    static char *escape_into(char *to, char character) noexcept {
        const auto store = [&](std::string_view escaped) {
            std::memcpy(to, escaped.data(), escaped.size());
            return to + escaped.size();
        };
        switch (character) {
        case '"': return store("\\\"");
        case '\\': return store("\\\\");
        case '\b': return store("\\b");
        case '\f': return store("\\f");
        case '\n': return store("\\n");
        case '\r': return store("\\r");
        case '\t': return store("\\t");
        default: break;
        }
        static constexpr char digits[] = "0123456789abcdef";
        const auto value = static_cast<unsigned char>(character);
        const char escaped[6] { '\\', 'u', '0', '0', digits[(value >> 4) & 0xF], digits[value & 0xF] };
        std::memcpy(to, escaped, 6);
        return to + 6;
    }

private:
    /** One character JSON cannot hold as itself: a short escape where there is one, else \u00XX. */
    void write_escape(char character) noexcept {
        switch (character) {
        case '"': return this->put_constant("\\\"");
        case '\\': return this->put_constant("\\\\");
        case '\b': return this->put_constant("\\b");
        case '\f': return this->put_constant("\\f");
        case '\n': return this->put_constant("\\n");
        case '\r': return this->put_constant("\\r");
        case '\t': return this->put_constant("\\t");
        default: break;
        }
        static constexpr char digits[] = "0123456789abcdef";
        const auto value = static_cast<unsigned char>(character);
        const std::array<char, 6> escaped { '\\', 'u', '0', '0', digits[(value >> 4) & 0xF], digits[value & 0xF] };
        this->put_constant(escaped);
    }

    void begin_array() noexcept {
        this->begin_value();
        this->put_constant("[");
        if (!this->push(false)) return;
        this->written_mask &= ~(1u << (this->depth - 1));
    }

    void end_array() noexcept {
        const bool populated = this->depth > 0 && (this->written_mask & (1u << (this->depth - 1))) != 0;
        if (!this->pop(false)) return;
        if (populated) this->indent_to(this->depth);
        this->put_constant("]");
    }

    void begin_object() noexcept {
        this->begin_value();
        this->put_constant("{");
        if (!this->push(true)) return;
        this->written_mask &= ~(1u << (this->depth - 1));
    }

    void end_object() noexcept {
        const bool populated = this->depth > 0 && (this->written_mask & (1u << (this->depth - 1))) != 0;
        if (!this->pop(true)) return;
        if (populated) this->indent_to(this->depth);
        this->put_constant("}");
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
        this->put_constant("null");
    }

    void boolean(bool value) noexcept {
        this->begin_value();
        this->scalar(value);
    }

    /** A scalar's text, composed where it will be kept. How it is spelled is write_text's business. */
    template<typename T>
    void scalar(T value) noexcept {
        this->compose(widest_text<T>, [value](char *to) { return static_cast<std::size_t>(write_text(to, value) - to); });
    }

    /**
     * Writes whole members of the object that is open - keys, values and the commas between them -
     * into room claimed once for all of them, where each would otherwise ask for its own.
     *
     * `write` is handed room for `at_most` characters and returns how many it used; what it
     * writes has to be exactly what key() and value() would have written, commas included. False
     * where that cannot be done - this writer is indenting, or the destination has not that much
     * room in one piece - and nothing has been written: the members are then written the usual
     * way, one at a time.
     */
    template<typename Write>
    [[nodiscard]] bool compose_members(std::size_t at_most, Write write) noexcept {
        if (this->options.indent != 0 || !this->inside_object() || this->pending_value) return false;
        char *const to = this->room_for(at_most);
        if (to == nullptr) return false;
        this->used(write(to));
        this->written_mask |= 1u << (this->depth - 1);
        return true;
    }

    /**
     * Writes a whole value as one piece - the comma that separates it from the element before,
     * its braces and everything between - into room claimed once, where opening it, each member
     * and closing it would otherwise ask for their own.
     *
     * `write` is handed room for `at_most` characters after the comma, if any, and returns how
     * many it used; what it writes has to be exactly what object() and the members would have.
     * False, with nothing written, where this writer is indenting, where a value is not due here,
     * or where the room cannot be had in one piece: the object is then written the usual way.
     */
    template<typename Write>
    [[nodiscard]] bool compose_value(std::size_t at_most, Write write) noexcept {
        if (this->options.indent != 0) return false;
        if (this->inside_object() && !this->pending_value) return false;
        const bool comma = !this->inside_object() && this->has_members();

        char *const to = this->room_for(at_most + 1);
        if (to == nullptr) return false;
        if (comma) to[0] = ',';
        this->used((comma ? 1 : 0) + write(to + (comma ? 1 : 0)));

        if (this->inside_object()) this->pending_value = false;
        if (this->depth > 0) this->written_mask |= 1u << (this->depth - 1);
        return true;
    }

    /** Whether the object that is open has had a member written yet, and so whether the next needs a comma. */
    [[nodiscard]] bool has_members() const noexcept {
        return this->depth > 0 && (this->written_mask & (1u << (this->depth - 1))) != 0;
    }

    void integer(std::int64_t value) noexcept { this->number(value); }
    void integer(std::uint64_t value) noexcept { this->number(value); }

    /**
     * JSON has no NaN or infinity, so a non-finite value is written as null. How a finite one is
     * spelled is real_format.hpp's business.
     */
    void real(double value) noexcept {
        this->begin_value();
        this->scalar(value);
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
        if (this->options.indent != 0 || !scanner::is_plain_text(Name)) {
            this->key(Name);
            return;
        }

        // The key as it stands in the text, and the same behind the comma that separates it from
        // the member before: one write either way, where a comma and a key would be two.
        static constexpr auto framed = detail::joined<Name.size() + 3>({ "\"", Name, "\":" });
        static constexpr auto framed_after_comma = detail::joined<Name.size() + 4>({ ",\"", Name, "\":" });

        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        const auto level = 1u << (this->depth - 1);
        if ((this->written_mask & level) != 0) this->put_constant(framed_after_comma);
        else this->put_constant(framed);
        this->written_mask |= level;
        this->pending_value = true;
    }

    void key(std::string_view name) noexcept {
        if (!this->inside_object()) {
            this->fail(errc::key_outside_object);
            return;
        }
        // A name that needs no escape, in a document that is not indented, is framed - the
        // comma before it, its quotes, the colon after - in one piece.
        if (this->options.indent == 0 && scanner::end_of_plain_text(name.data(), name.data() + name.size()) == name.data() + name.size()) {
            if (char *const to = this->room_for(name.size() + 4)) {
                const auto level = 1u << (this->depth - 1);
                char *at = to;
                if ((this->written_mask & level) != 0) *at++ = ',';
                *at++ = '"';
                std::memcpy(at, name.data(), name.size());
                at += name.size();
                *at++ = '"';
                *at++ = ':';
                this->used(static_cast<std::size_t>(at - to));
                this->written_mask |= level;
                this->pending_value = true;
                return;
            }
        }
        this->separate();
        this->write_quoted(name);
        this->put_constant(":");
        if (this->options.indent != 0) this->put_constant(" ");
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

    /** A number as the type it has: a float gets a float's digits, not the double's it widens to. */
    template<typename T>
        requires (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>
    void number(T value) noexcept {
        this->begin_value();
        this->scalar(value);
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
    using element = std::remove_cvref_t<std::ranges::range_value_t<R>>;

    // Numbers and booleans have a longest text, so a batch of them is written into room claimed
    // once, comma and all, where each would otherwise ask for its own and then for its comma's.
    // What a batch cannot have - indentation, room, an element that is not so simple - each
    // element is written the usual way.
    if constexpr (widest_text<element> != 0) {
        if (this->options.indent == 0) {
            constexpr std::size_t batch = 16;
            // A short array is one piece, brackets and all, and is never opened.
            if constexpr (std::ranges::sized_range<R>) {
                const auto count = std::ranges::size(items);
                if (count <= batch) {
                    const bool composed = this->compose_value(2 + count * (widest_text<element> + 1), [&](char *const to) {
                        char *cursor = to;
                        *cursor++ = '[';
                        bool separated = false;
                        for (const element item : items) {
                            if (separated) *cursor++ = ',';
                            cursor = write_text(cursor, item);
                            separated = true;
                        }
                        *cursor++ = ']';
                        return static_cast<std::size_t>(cursor - to);
                    });
                    if (composed) return;
                }
            }
        }
    }

    const auto scope = this->array();

    if constexpr (widest_text<element> != 0) {
        if (this->options.indent == 0) {
            auto at = std::ranges::begin(items);
            const auto end = std::ranges::end(items);
            bool separated = this->has_members();
            while (at != end) {
                constexpr std::size_t batch = 16;
                char *const to = this->room_for(batch * (widest_text<element> + 1));
                if (to == nullptr) break;
                char *cursor = to;
                for (std::size_t written = 0; written < batch && at != end; ++written, ++at) {
                    if (separated) *cursor++ = ',';
                    cursor = write_text(cursor, static_cast<element>(*at));
                    separated = true;
                }
                this->used(static_cast<std::size_t>(cursor - to));
                this->written_mask |= 1u << (this->depth - 1);
            }
            for (; at != end; ++at) emit_value(*this, static_cast<element>(*at));
            return;
        }
    }

    for (detail::range_element_t<decltype(items)> item : items)
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

/**
 * Writes a C++ value as JSON into a string, replacing what it held and keeping its capacity -
 * for writing many documents into the one buffer. What the string held is gone either way;
 * on failure it is left empty.
 */
template<typename T>
    requires (!sink<T>)
std::expected<std::size_t, error> write(const T &value, std::string &into, writer_options options = {}) {
    into.clear();
    container_sink out { into };
    auto written = write(out, value, options);
    if (!written) into.clear();
    return written;
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
