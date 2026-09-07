#pragma once

#include <serpent/fwd.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace serpent {

template<typename Writer>
class write_visitor;

template<typename Source>
class read_visitor;

/** For a static_assert that only fires when its branch is actually taken. */
template<typename>
inline constexpr bool always_false = false;

/** Flips constness by direction, so one json_convert can serve both. */
template<typename Visitor, typename T>
using conversion_object_t = std::conditional_t<std::remove_cvref_t<Visitor>::is_reading, T &, const T &>;

/** Names each field on the way out: key, then value. Generic over the writer. */
template<typename Writer>
class write_visitor {
    Writer *out = nullptr;

public:
    static constexpr bool is_reading = false;

    explicit write_visitor(Writer &out) noexcept : out(&out) {}

    template<typename T>
    void member(std::string_view name, const T &value) noexcept {
        this->out->key(name);
        this->out->value(value);
    }

    [[nodiscard]] Writer &target() const noexcept { return *this->out; }
};

namespace detail {

/** Stands in for a visitor when asking whether a type has a json_convert. */
struct convert_probe {
    static constexpr bool is_reading = false;

    template<typename T>
    void member(std::string_view, const T &);
};

} // namespace detail

/**
 * Names each field on the way in, keeping a cursor into the object.
 *
 * A field is looked for at the cursor first and only scanned for when that misses, so a
 * document written in declaration order costs one pass rather than a scan per field. A field
 * whose key is absent keeps whatever value it already held.
 */
template<typename Source>
class read_visitor {
    using iterator = decltype(std::declval<const Source &>().items().begin());

    Source source {};
    iterator cursor {};
    bool complete = true;

public:
    static constexpr bool is_reading = true;

    explicit read_visitor(Source source) noexcept : source(source), cursor(source.items().begin()) {}

    template<typename T>
    void member(std::string_view name, T &value) {
        if (this->cursor != iterator {}) {
            const auto entry = *this->cursor;
            if (entry.key_is(name)) {
                if (!read_into(entry.value, value)) this->complete = false;
                ++this->cursor;
                return;
            }
        }
        const auto found = this->source[name];
        if (!found.is_valid()) return; // absent: keep the existing value
        if (!read_into(found, value)) this->complete = false;
    }

    [[nodiscard]] bool ok() const noexcept { return this->complete; }
};

} // namespace serpent
