#pragma once

#include <serpent/concepts.hpp>
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

    /**
     * The same, for a key the compiler already knows.
     *
     * Every format frames a key the same way every time - a length and the bytes, or quotes and
     * a colon - so for a constant name that framing is a constant too, and goes out in one
     * piece instead of being assembled a byte at a time.
     */
    template<const std::string_view &Name, typename T>
    void member(const T &value) noexcept {
        this->out->template key_literal<Name>();
        this->out->value(value);
    }

    /** The same on the way out: whether a member may be absent only matters coming in. */
    template<typename T>
    bool member_if_present(std::string_view name, const T &value) noexcept {
        this->member(name, value);
        return true;
    }

    template<const std::string_view &Name, typename T>
    bool member_if_present(const T &value) noexcept {
        this->template member<Name>(value);
        return true;
    }

    /** Nothing can be missing on the way out; the member is whatever the object holds. */
    void missing(std::string_view) noexcept {}

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
    std::size_t found = 0;
    std::string_view absent {};

public:
    static constexpr bool is_reading = true;

    explicit read_visitor(Source source) noexcept : source(source), cursor(source.items().begin()) {}

    /**
     * A member the document has to carry.
     *
     * An optional is exempt, because absence is what it represents; say member_if_present for
     * anything else the document is allowed to leave out. Absence is recorded, not thrown:
     * the read is finished either way and ok() reports on it at the end.
     */
    template<typename T>
    void member(std::string_view name, T &value) {
        if (!this->take(name, value) && !detail::optional_like<std::remove_cvref_t<T>>) this->missing(name);
    }

    /** The same, for a key the compiler already knows: its length is a constant to compare. */
    template<const std::string_view &Name, typename T>
    void member(T &value) {
        this->member(Name, value);
    }

    /**
     * A member the document may leave out, saying whether it did.
     *
     * The spelling for a member whose existing value is a default worth keeping. member() is
     * the other answer, and is the one you get by not thinking about it, which is the way
     * round it should be.
     */
    template<typename T>
    bool member_if_present(std::string_view name, T &value) {
        return this->take(name, value);
    }

    template<const std::string_view &Name, typename T>
    bool member_if_present(T &value) {
        return this->take(Name, value);
    }

    /** Records that a member the type insists on was not in the document. */
    void missing(std::string_view name) noexcept {
        this->complete = false;
        if (this->absent.empty()) this->absent = name;
    }

    /** The first member that was required and not there, if any. */
    [[nodiscard]] std::string_view missing_member() const noexcept { return this->absent; }

    [[nodiscard]] bool ok() const noexcept { return this->complete; }

    /** How many of the type's members the document actually named. */
    [[nodiscard]] std::size_t matched() const noexcept { return this->found; }

private:
    template<typename T>
    bool take(std::string_view name, T &value) {
        if (this->cursor != iterator {}) {
            const auto entry = *this->cursor;
            if (entry.key_is(name)) {
                if (!read_into(entry.value, value)) this->complete = false;
                ++this->found;
                ++this->cursor;
                return true;
            }
        }
        const auto elsewhere = this->source[name];
        if (!elsewhere.is_valid()) return false; // absent: the caller decides whether that is allowed
        ++this->found;
        if (!read_into(elsewhere, value)) this->complete = false;
        return true;
    }
};

} // namespace serpent
