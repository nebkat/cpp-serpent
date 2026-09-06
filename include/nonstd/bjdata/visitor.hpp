#pragma once

// The two direction-carrying visitors, split out so reflect.hpp can build on them without
// depending on the serializer dispatch that in turn depends on it.

#include <nonstd/bjdata/view.hpp>
#include <nonstd/bjdata/writer.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nonstd::bjdata {

class write_visitor;
class read_visitor;

/**
 * Flips constness by direction, so one bjdata_convert can serve both.
 * Lifted from isobus's conversion_object_t<Direction, T>.
 */
template<typename Visitor, typename T>
using conversion_object_t = std::conditional_t<std::remove_cvref_t<Visitor>::is_reading, T &, const T &>;

template<typename T>
bool read_into(view source, T &value);

/** Names each field on the way out: key, then value. */
class write_visitor {
    writer *out = nullptr;

public:
    static constexpr bool is_reading = false;

    explicit write_visitor(writer &out) noexcept: out(&out) {}

    template<typename T>
    void member(std::string_view name, const T &value) noexcept {
        this->out->key(name);
        this->out->value(value);
    }

    [[nodiscard]] writer &target() const noexcept { return *this->out; }
};

/**
 * Names each field on the way in, keeping a cursor into the object.
 *
 * A field is looked for at the cursor first and only scanned for when that misses, so a
 * document whose keys are in declaration order - which is what this writer, dart-bjdata and
 * nlohmann all produce - costs one pass rather than one scan per field. A field whose key is
 * absent is left at whatever value it already held, which gives the firmware's
 * `j.value(key, default)` semantics for free.
 */
class read_visitor {
    view source {};
    member_iterator cursor {};
    bool complete = true;

public:
    static constexpr bool is_reading = true;

    explicit read_visitor(view source) noexcept: source(source), cursor(source.items().begin()) {}

    template<typename T>
    void member(std::string_view name, T &value) {
        if (this->cursor != member_iterator {}) {
            const auto entry = *this->cursor;
            if (entry.key == name) {
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

}// namespace nonstd::bjdata
