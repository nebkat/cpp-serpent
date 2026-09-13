#pragma once

// An owning document tree, for building a document whose shape is decided as it is written.
//
// Its own header, included by nothing else, because the library is built the other way round:
// bytes are read in place through a view and written straight from your types, with no tree in
// between. That is still true of everything else here - this is the escape hatch for the case
// the rest of the library cannot serve, where the fields are not known until run time and the
// document is assembled a piece at a time.
//
// Reading is not what this is for. A document you have the bytes of is already a tree, one that
// costs nothing: view::over(bytes) and reader::over(text) walk it in place. Build a value when
// you are the one producing the document.

#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/serializer.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace serpent {

/**
 * @brief A document held as a tree: one of the scalar kinds, an array of values, or an object.
 *
 * Every node owns what it holds, so a value can be returned, stored and added to long after the
 * code that started it has gone. That ownership is the whole point of it, and the reason it is
 * not what the rest of the library does.
 */
class value {
public:
    using array = std::vector<value>;
    using binary = std::vector<std::byte>;

    /**
     * The members of an object, in the order they were added.
     *
     * Insertion order rather than sorted, because a document built by hand is usually read by a
     * person, and the order the fields were written in is the order they were meant in. Neither
     * format ascribes meaning to key order, so nothing downstream depends on the choice.
     */
    class object {
        std::vector<std::pair<std::string, value>> entries;

    public:
        object() = default;
        object(std::initializer_list<std::pair<const std::string_view, value>> members) {
            this->entries.reserve(members.size());
            for (const auto &[name, held] : members) this->entries.emplace_back(std::string { name }, held);
        }

        [[nodiscard]] auto begin() const noexcept { return this->entries.begin(); }
        [[nodiscard]] auto end() const noexcept { return this->entries.end(); }
        [[nodiscard]] auto begin() noexcept { return this->entries.begin(); }
        [[nodiscard]] auto end() noexcept { return this->entries.end(); }
        [[nodiscard]] std::size_t size() const noexcept { return this->entries.size(); }
        [[nodiscard]] bool empty() const noexcept { return this->entries.empty(); }

        [[nodiscard]] const value *find(std::string_view name) const noexcept;
        [[nodiscard]] value *find(std::string_view name) noexcept;
        [[nodiscard]] bool contains(std::string_view name) const noexcept { return this->find(name) != nullptr; }

        /** The member, adding it as null if it was not there. */
        value &operator[](std::string_view name);

        /** Removes a member, saying whether there was one. */
        bool erase(std::string_view name);

        /**
         * The same members with the same values, whatever order they were added in.
         *
         * Spelled out rather than defaulted, both because neither format ascribes meaning to
         * key order and because a defaulted one would not be found at all: without it, two
         * objects compare by converting each to a value, which compares its object again.
         */
        friend bool operator==(const object &left, const object &right) noexcept;
    };

    using storage = std::variant<std::monostate,
            bool,
            std::int64_t,
            std::uint64_t,
            double,
            std::string,
            binary,
            array,
            object>;

private:
    storage held {};

public:
    value() = default;
    value(std::nullptr_t) noexcept {}
    value(bool truth) noexcept : held(truth) {}

    // Split by signedness so that a value above int64 range survives, and taken as the widest
    // of each: the writer narrows every integer to the smallest marker that holds it anyway, so
    // keeping the declared width here would buy nothing but alternatives to visit.
    template<std::integral T>
    requires (!std::same_as<T, bool>)
    value(T number) noexcept {
        if constexpr (std::is_signed_v<T>)
            this->held = static_cast<std::int64_t>(number);
        else
            this->held = static_cast<std::uint64_t>(number);
    }

    template<std::floating_point T>
    value(T number) noexcept : held(static_cast<double>(number)) {}

    // Spelled out rather than left to string_view, which would lose to the bool conversion.
    value(const char *text) : held(std::string { text }) {}
    value(std::string_view text) : held(std::string { text }) {}
    value(std::string text) noexcept : held(std::move(text)) {}
    value(std::span<const std::byte> bytes) : held(binary { bytes.begin(), bytes.end() }) {}
    value(binary bytes) noexcept : held(std::move(bytes)) {}
    value(array items) noexcept : held(std::move(items)) {}
    value(object members) noexcept : held(std::move(members)) {}

    /** An object written out at its call site: the braces a nested document is built with. */
    static value of(std::initializer_list<std::pair<const std::string_view, value>> members) {
        return value { object { members } };
    }

    /** An array written out at its call site. */
    static value of(std::initializer_list<value> items) { return value { array { items } }; }

    // ---------------- what it is ----------------

    [[nodiscard]] kind type() const noexcept {
        return std::visit(
                []<typename T>(const T &) {
                    if constexpr (std::same_as<T, std::monostate>) return kind::null;
                    else if constexpr (std::same_as<T, bool>) return kind::boolean;
                    else if constexpr (std::same_as<T, double>) return kind::real;
                    else if constexpr (std::same_as<T, std::string>) return kind::string;
                    else if constexpr (std::same_as<T, object>) return kind::object;
                    else if constexpr (std::same_as<T, array> || std::same_as<T, binary>) return kind::array;
                    else return kind::integer;
                },
                this->held);
    }

    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(this->held); }
    [[nodiscard]] bool is_boolean() const noexcept { return std::holds_alternative<bool>(this->held); }
    [[nodiscard]] bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] bool is_real() const noexcept { return std::holds_alternative<double>(this->held); }
    [[nodiscard]] bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(this->held); }
    [[nodiscard]] bool is_binary() const noexcept { return std::holds_alternative<binary>(this->held); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<array>(this->held); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<object>(this->held); }

    [[nodiscard]] const storage &contents() const noexcept { return this->held; }

    // ---------------- reading it back ----------------

    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        if (const auto *truth = std::get_if<bool>(&this->held)) return *truth;
        return std::nullopt;
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> as_int() const noexcept {
        if (const auto *whole = std::get_if<std::int64_t>(&this->held))
            return std::in_range<T>(*whole) ? std::optional<T> { static_cast<T>(*whole) } : std::nullopt;
        if (const auto *whole = std::get_if<std::uint64_t>(&this->held))
            return std::in_range<T>(*whole) ? std::optional<T> { static_cast<T>(*whole) } : std::nullopt;
        return std::nullopt;
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> as_float() const noexcept {
        if (const auto *number = std::get_if<double>(&this->held)) return static_cast<T>(*number);
        if (const auto whole = this->as_int<std::int64_t>()) return static_cast<T>(*whole);
        if (const auto whole = this->as_int<std::uint64_t>()) return static_cast<T>(*whole);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept {
        if (const auto *text = std::get_if<std::string>(&this->held)) return std::string_view { *text };
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> as_binary() const noexcept {
        if (const auto *bytes = std::get_if<binary>(&this->held)) return std::span<const std::byte> { *bytes };
        return std::nullopt;
    }

    /** The array or object contents, or nullptr when it is neither. */
    [[nodiscard]] const array *as_array() const noexcept { return std::get_if<array>(&this->held); }
    [[nodiscard]] array *as_array() noexcept { return std::get_if<array>(&this->held); }
    [[nodiscard]] const object *as_object() const noexcept { return std::get_if<object>(&this->held); }
    [[nodiscard]] object *as_object() noexcept { return std::get_if<object>(&this->held); }

    /** How many members or elements, counting a scalar as one and null as none. */
    [[nodiscard]] std::size_t size() const noexcept {
        if (const auto *items = this->as_array()) return items->size();
        if (const auto *members = this->as_object()) return members->size();
        return this->is_null() ? 0 : 1;
    }

    // ---------------- building it ----------------

    /**
     * The member under this key, which starts as null if it was not there.
     *
     * A null value becomes an object on the way, so a document can be built up from nothing,
     * one branch at a time. Anything else already holding a value is a programming error and
     * says so - it would otherwise silently discard what was there.
     */
    value &operator[](std::string_view name) {
        if (this->is_null()) this->held = object {};
        auto *members = this->as_object();
        if (members == nullptr) raise(errc::type_mismatch, 0, name);
        return (*members)[name];
    }

    /** The member, or a null value when there is none. Never adds. */
    [[nodiscard]] const value &operator[](std::string_view name) const noexcept {
        static const value nothing {};
        const auto *members = this->as_object();
        if (members == nullptr) return nothing;
        const auto *found = members->find(name);
        return found != nullptr ? *found : nothing;
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        const auto *members = this->as_object();
        return members != nullptr && members->contains(name);
    }

    /** The member, or an error naming the key. */
    [[nodiscard]] const value &at(std::string_view name) const {
        const auto *members = this->as_object();
        const auto *found = members != nullptr ? members->find(name) : nullptr;
        if (found == nullptr) raise(errc::missing_key, 0, name);
        return *found;
    }

    [[nodiscard]] const value &at(std::size_t index) const {
        const auto *items = this->as_array();
        if (items == nullptr || index >= items->size()) raise(errc::out_of_range, 0);
        return (*items)[index];
    }

    /** Appends, turning a null value into an array first, as operator[] does for objects. */
    value &push_back(value item) {
        if (this->is_null()) this->held = array {};
        auto *items = this->as_array();
        if (items == nullptr) raise(errc::type_mismatch, 0);
        return items->emplace_back(std::move(item));
    }

    friend bool operator==(const value &left, const value &right) = default;
};

inline const value *value::object::find(std::string_view name) const noexcept {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    return found != this->entries.end() ? &found->second : nullptr;
}

inline value *value::object::find(std::string_view name) noexcept {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    return found != this->entries.end() ? &found->second : nullptr;
}

inline value &value::object::operator[](std::string_view name) {
    if (auto *found = this->find(name)) return *found;
    return this->entries.emplace_back(std::string { name }, value {}).second;
}

inline bool operator==(const value::object &left, const value::object &right) noexcept {
    if (left.size() != right.size()) return false;
    return std::ranges::all_of(left, [&right](const auto &entry) {
        const auto *other = right.find(entry.first);
        return other != nullptr && *other == entry.second;
    });
}

inline bool value::object::erase(std::string_view name) {
    const auto found = std::ranges::find(this->entries, name, [](const auto &entry) {
        return std::string_view { entry.first };
    });
    if (found == this->entries.end()) return false;
    this->entries.erase(found);
    return true;
}

/**
 * Both directions, so a value goes wherever any other type goes: on its own, as a member of a
 * reflected struct, or as an element of a container.
 */
template<>
struct serializer<value, void> {
    template<typename Writer>
    static void write(Writer &out, const value &item) {
        std::visit(
                [&out]<typename T>(const T &held) {
                    if constexpr (std::same_as<T, std::monostate>) {
                        out.null();
                    } else if constexpr (std::same_as<T, value::binary>) {
                        out.bytes(held);
                    } else if constexpr (std::same_as<T, value::array>) {
                        out.range(held);
                    } else if constexpr (std::same_as<T, value::object>) {
                        const auto scope = out.object();
                        for (const auto &[name, member] : held) {
                            out.key(name);
                            out.value(member);
                        }
                    } else {
                        out.value(held);
                    }
                },
                item.contents());
    }

    template<typename Source>
    static bool read(Source source, value &item) {
        switch (source.type()) {
        case kind::invalid: return false;
        case kind::null: item = value {}; return true;
        case kind::boolean: item = value { *source.as_bool() }; return true;
        case kind::integer:
            if (const auto whole = source.template as_int<std::int64_t>()) item = value { *whole };
            else if (const auto unsigned_whole = source.template as_int<std::uint64_t>()) item = value { *unsigned_whole };
            else return false;
            return true;
        case kind::real: {
            const auto number = source.template as_float<double>();
            if (!number) return false;
            item = value { *number };
            return true;
        }
        case kind::string: {
            const auto text = source.as_string();
            if (!text) return false;
            item = value { *text };
            return true;
        }
        case kind::array: {
            // A format with a binary type of its own keeps it binary; one without carries it as
            // an array of numbers and reads back as exactly that, which is all it ever was.
            if constexpr (requires { source.as_binary(); }) {
                if (const auto bytes = source.as_binary()) {
                    item = value { *bytes };
                    return true;
                }
            }
            value::array items;
            if constexpr (requires { source.size_hint(); }) {
                if (const auto hint = source.size_hint()) items.reserve(*hint);
            }
            for (const auto element : source.array()) {
                if (!read(element, items.emplace_back())) return false;
            }
            item = value { std::move(items) };
            return true;
        }
        case kind::object: {
            value::object members;
            for (const auto entry : source.items()) {
                // key_string() rather than the key itself, which a text format leaves encoded.
                if (!read(entry.value, members[entry.key_string()])) return false;
            }
            item = value { std::move(members) };
            return true;
        }
        }
        return false;
    }
};

} // namespace serpent
