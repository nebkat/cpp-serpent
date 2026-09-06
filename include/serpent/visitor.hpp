#pragma once

// The two direction-carrying visitors, split out so reflect.hpp can build on them without
// depending on the serializer dispatch that in turn depends on it.

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

/**
 * Flips constness by direction, so one json_convert can serve both.
 * Lifted from isobus's conversion_object_t<Direction, T>.
 */
template<typename Visitor, typename T>
using conversion_object_t = std::conditional_t<std::remove_cvref_t<Visitor>::is_reading, T &, const T &>;

template<typename Source, typename T>
bool read_into(Source source, T &value);

/**
 * Names each field on the way out: key, then value.
 *
 * Generic over the writer, so one json_convert serves every output format. That is the
 * whole reason a type written once can be emitted as BJData and as JSON.
 */
template<typename Writer>
class write_visitor {
    Writer *out = nullptr;

public:
    static constexpr bool is_reading = false;

    explicit write_visitor(Writer &out) noexcept: out(&out) {}

    template<typename T>
    void member(std::string_view name, const T &value) noexcept {
        this->out->key(name);
        this->out->value(value);
    }

    [[nodiscard]] Writer &target() const noexcept { return *this->out; }
};

namespace detail {

/**
 * Stands in for a visitor when asking whether a type has a json_convert.
 *
 * Using a real visitor would tie the question to one particular writer, which is exactly
 * what the customization is supposed to be free of.
 */
struct convert_probe {
    static constexpr bool is_reading = false;

    template<typename T>
    void member(std::string_view, const T &);
};

}// namespace detail

/**
 * Names each field on the way in, keeping a cursor into the object.
 *
 * A field is looked for at the cursor first and only scanned for when that misses, so a
 * document whose keys are in declaration order - which is what this writer, dart-bjdata and
 * nlohmann all produce - costs one pass rather than one scan per field. A field whose key is
 * absent is left at whatever value it already held, which gives the firmware's
 * `j.value(key, default)` semantics for free.
 */
template<typename Source>
class read_visitor {
    using iterator = decltype(std::declval<const Source &>().items().begin());

    Source source {};
    iterator cursor {};
    bool complete = true;

public:
    static constexpr bool is_reading = true;

    explicit read_visitor(Source source) noexcept: source(source), cursor(source.items().begin()) {}

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
        if (!found.is_valid()) return;                 // absent: keep the existing value
        if (!read_into(found, value)) this->complete = false;
    }

    [[nodiscard]] bool ok() const noexcept { return this->complete; }
};

}// namespace serpent
