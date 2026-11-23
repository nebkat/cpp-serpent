#pragma once

// Where a document stopped fitting its type, for decode to report.
//
// Every reader answers a bare bool, so a value deep inside a document that does not fit reaches
// the top as nothing more than false. Asked, decode finds the value from the top down instead:
// a part of a value that did not fit is read again on its own, and the first part that does not
// fit either is where to look next. The readers are untouched, so a decode that succeeds - or one
// that fails without asking - pays nothing for this.

#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/limits.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace serpent {

/** How decode reports a document that does not fit its type. */
struct decode_options {
    /**
     * Read a document that parses but does not fit again, part by part, to give the error the
     * path(), offset and found() kind of the value at fault and to find a missing key at any
     * depth. Off, only a key missing from the root object is named.
     */
    bool trace = false;
};

} // namespace serpent

namespace serpent::detail {

class failure_locator {
public:
    /**
     * Finds what in `value`'s reading of `source`, which did not fit, is at fault: the first of its
     * members or elements that does not fit on its own, and within that the same again, down to a
     * value with no part to blame. That value is then where the error is.
     */
    template<typename Source, typename T>
    void locate(const Source &source, T &value) {
        this->settle(source);
        using U = std::remove_cvref_t<T>;

        if constexpr (std::is_enum_v<U> || variant_like<U> || byte_range<U> || string_like<U>) {
            // Read whole or not at all. A variant in particular: which of its alternatives was
            // nearly right is anyone's guess, so it is the value at fault itself.
        } else if constexpr (optional_like<U>) {
            if (!source.is_valid() || source.is_null()) return;
            std::remove_cvref_t<decltype(*value)> item {};
            this->locate(source, item);
        } else if constexpr (back_insertable<U> || insertable<U> || fixed_sequence<U>) {
            if (!source.is_array()) return;
            std::size_t index = 0;
            for (const auto &element : source.array()) {
                std::ranges::range_value_t<U> item {};
                if (!read_into(element, item)) return this->descend(element, item, index);
                ++index;
            }
        } else if constexpr (pair_like<U>) {
            if (!source.is_array() || source.size() != 2) return;
            auto elements = source.array().begin();
            if (!read_into(*elements, value.first)) return this->descend(*elements, value.first, 0);
            ++elements;
            if (!read_into(*elements, value.second)) return this->descend(*elements, value.second, 1);
        } else if constexpr (keyed_but_not_an_object<U>) {
            if (!source.is_array()) return;
            std::size_t index = 0;
            for (const auto &element : source.array()) {
                std::pair<typename U::key_type, typename U::mapped_type> entry {};
                if (!read_into(element, entry)) return this->descend(element, entry, index);
                ++index;
            }
        } else if constexpr (keyed_insertable<U>) {
            if (!source.is_object()) return;
            for (const auto &entry : source.items()) {
                typename U::mapped_type slot {};
                if (read_into(entry.value, slot)) continue;
                if constexpr (std::convertible_to<const decltype(entry.key) &, std::string_view>) {
                    this->push(segment { .key = entry.key, .keyed = true });
                } else {
                    // JSON keeps a key encoded until asked, and it goes into the path as it stands.
                    this->push(segment { .key = entry.key.contents, .keyed = true, .escaped = entry.key.escaped });
                }
                return this->locate(entry.value, slot);
            }
        } else if constexpr (convertible_type<U> || tabulated_type<U> || reflected_type<U>) {
            if (!source.is_object()) return;
            member_finder<Source> finder { *this, source };
            if constexpr (convertible_type<U>) {
                json_convert(finder, value);
            } else if constexpr (tabulated_type<U>) {
                table_convert(finder, value);
            } else {
                reflect_convert(finder, value);
            }
        }
        // Anything else - a number, a read written by hand - is read whole, and is at fault itself.
    }

    /** The failure found, as decode reports it. */
    [[nodiscard]] error result() const noexcept {
        error failure = this->absent.empty() ? error { errc::type_mismatch, this->leaf_offset }
                                             : error { errc::missing_key, this->leaf_offset, this->absent };
        this->describe(failure);
        return failure;
    }

private:
    struct segment {
        std::string_view key {};
        std::size_t index = 0;
        bool keyed = false;
        /** A key that stands as JSON spelled it, escapes and all. */
        bool escaped = false;
    };

    /** The member walk of a type, reading each member on its own until one does not fit. */
    template<typename Source>
    class member_finder {
        failure_locator *locator;
        const Source *object;
        bool settled = false;

    public:
        static constexpr bool is_reading = true;

        member_finder(failure_locator &locator, const Source &object) noexcept
        : locator(&locator)
        , object(&object) {}

        template<typename T>
        void member(std::string_view name, T &value) {
            if (!this->take(name, value) && !optional_like<std::remove_cvref_t<T>>) this->missing(name);
        }

        template<const std::string_view &Name, typename T>
        void member(T &value) {
            this->member(Name, value);
        }

        template<typename T>
        bool member_if_present(std::string_view name, T &value) {
            return this->take(name, value);
        }

        template<const std::string_view &Name, typename T>
        bool member_if_present(T &value) {
            return this->take(Name, value);
        }

        /** The object lacks a key it needs: it is the value at fault, and the key says why. */
        void missing(std::string_view name) noexcept {
            if (std::exchange(this->settled, true)) return;
            this->locator->absent = name;
        }

    private:
        template<typename T>
        bool take(std::string_view name, T &value) {
            // The first fault is the one reported; once it is found the rest of the walk is
            // passed over.
            if (this->settled) return true;
            const auto child = (*this->object)[name];
            if (!child.is_valid()) return false;
            if (!read_into(child, value)) {
                this->settled = true;
                this->locator->push(segment { .key = name, .keyed = true });
                this->locator->locate(child, value);
            }
            return true;
        }
    };

    template<typename Source, typename T>
    void descend(const Source &element, T &value, std::size_t index) {
        this->push(segment { .index = index });
        this->locate(element, value);
    }

    void push(segment step) noexcept {
        // A path is no deeper than the document, and validate() has held that to max_depth.
        if (this->count < this->segments.size()) this->segments[this->count++] = step;
    }

    template<typename Source>
    void settle(const Source &value) noexcept {
        if constexpr (requires {
                          { value.type() } -> std::convertible_to<kind>;
                      }) {
            this->leaf_kind = value.type();
        }
        if constexpr (requires {
                          { value.offset() } -> std::convertible_to<std::size_t>;
                      }) {
            this->leaf_offset = value.offset();
        }
    }

    void describe(error &failure) const noexcept {
        // Measured first, so that a path too long to keep loses its front - the end of it is
        // the part nearest the fault.
        path_sink measure {};
        this->write_path(measure);
        constexpr std::string_view elided = "...";
        const bool cut = measure.total > error::path_capacity;
        const std::size_t room = cut ? error::path_capacity - elided.size() : error::path_capacity;
        path_sink sink {
            .out = failure.path_chars.data(),
            .skip = measure.total > room ? measure.total - room : 0,
        };
        if (cut) sink.direct(elided);
        this->write_path(sink);
        failure.path_length = static_cast<std::uint8_t>(sink.written);
        failure.found_kind = this->leaf_kind;
    }

    struct path_sink {
        char *out = nullptr;
        std::size_t skip = 0;
        std::size_t written = 0;
        std::size_t total = 0;

        void direct(std::string_view part) noexcept {
            for (const char c : part) this->out[this->written++] = c;
        }

        void append(std::string_view part) noexcept {
            for (const char c : part) {
                ++this->total;
                if (this->out == nullptr) continue;
                if (this->skip > 0) {
                    --this->skip;
                    continue;
                }
                this->out[this->written++] = c;
            }
        }

        void append_number(std::size_t number) noexcept {
            std::array<char, 20> digits {};
            std::size_t used = 0;
            do {
                digits[digits.size() - ++used] = static_cast<char>('0' + number % 10);
                number /= 10;
            } while (number != 0);
            this->append(std::string_view { digits.data() + digits.size() - used, used });
        }
    };

    void write_path(path_sink &sink) const noexcept {
        for (std::size_t at = 0; at < this->count; ++at) {
            const auto &step = this->segments[at];
            if (!step.keyed) {
                sink.append("[");
                sink.append_number(step.index);
                sink.append("]");
            } else if (step.escaped) {
                sink.append("[\"");
                sink.append(step.key);
                sink.append("\"]");
            } else if (plain_key(step.key)) {
                if (at != 0) sink.append(".");
                sink.append(step.key);
            } else {
                sink.append("[\"");
                for (const char c : step.key) {
                    if (c == '"' || c == '\\') sink.append("\\");
                    sink.append(std::string_view { &c, 1 });
                }
                sink.append("\"]");
            }
        }
    }

    /** A key that reads unambiguously after a dot. */
    [[nodiscard]] static constexpr bool plain_key(std::string_view key) noexcept {
        if (key.empty()) return false;
        for (const char c : key) {
            const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!word) return false;
        }
        return true;
    }

    std::array<segment, static_cast<std::size_t>(max_depth)> segments {};
    std::size_t count = 0;
    kind leaf_kind = kind::invalid;
    std::size_t leaf_offset = 0;
    std::string_view absent {};
};

/** Why a document validate() accepted did not decode as T, read again from `root` to say so. */
template<typename T, typename Source>
[[nodiscard]] error explain_mismatch(const Source &root, decode_options options) {
    if (options.trace) {
        failure_locator locator;
        T scratch {};
        locator.locate(root, scratch);
        return locator.result();
    }
    if (const auto absent = first_missing_member<T>(root); !absent.empty()) {
        return error { errc::missing_key, 0, absent };
    }
    return error { errc::type_mismatch, 0 };
}

} // namespace serpent::detail
