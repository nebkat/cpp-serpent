#pragma once

// Reading a reflected type from BJData without going through the member iterator.
//
// The generic path is written in terms of handles: an iterator that parses an entry into a key
// and a view, a visitor that compares the key against a name it is handed at run time. That is
// the right shape for a reader that knows nothing about the type. For a type whose fields the
// compiler can enumerate, none of it is necessary - the keys are constants, their lengths are
// constants, and the destination of each is known - so this walks the bytes once and assigns
// straight into the fields.
//
// Binary only. JSON stays on the generic path, where the reader is worth reading.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/direct.hpp>
#include <serpent/bjdata/view.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/config.hpp>
#include <serpent/member_runs.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace serpent::bjdata {

#if SERPENT_HAS_REFLECTION

namespace detail {

/** Compares against a name whose length is known, which is what makes it worth inlining. */
template<std::string_view const &Name>
[[nodiscard]] inline bool key_matches(std::string_view key) noexcept {
    return key.size() == Name.size() && std::memcmp(key.data(), Name.data(), Name.size()) == 0;
}

} // namespace detail

/**
 * Fills one object of a described type, consuming it.
 *
 * In two steps, because nearly every document read here was written by this library, and one
 * that was holds no surprises: its members are the type's, in the order the type declares them,
 * each key spelled the one way this library spells it. So the first step expects exactly that -
 * a key is one comparison against a constant, and the value after it is read as the type the
 * member is, straight off the cursor. Whatever that leaves - a document from another writer,
 * members in another order, keys this type does not name, an object that is counted or typed -
 * the second step reads entry by entry, matching each key by name.
 *
 * The cursor is borrowed and left after the object, which is what lets a sequence of objects be
 * walked once rather than read and then stepped over.
 */
template<typename T>
class object_filler {
    static constexpr auto members = std::define_static_array(serpent::detail::members_including_bases<T>());
    static_assert(members.size() <= 64, "a type with more than 64 members needs a wider seen mask");

    static consteval std::uint64_t bit_of(std::meta::info member) {
        return std::uint64_t { 1 } << (std::ranges::find(members, member) - members.begin());
    }

    /** One bit for each member the type insists on, to be compared once with those seen. */
    static constexpr std::uint64_t required = [] {
        std::uint64_t mask = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)
                    && serpent::detail::member_is_required<T, member>())
                mask |= bit_of(member);
        }
        return mask;
    }();

    detail::cursor &scan;
    std::span<const std::byte> buffer;
    T &value;

    detail::container_prefix prefix;
    std::uint64_t entries_left = 0;
    std::uint64_t seen = 0;
    bool every_value_read = true;

public:
    /** The cursor stands just after the opening brace. */
    object_filler(detail::cursor &scan, std::span<const std::byte> buffer, T &value) noexcept
    : scan(scan)
    , buffer(buffer)
    , value(value)
    , prefix(detail::parse_object_prefix(scan))
    , entries_left(prefix.count) {}

    void read_members_as_written() {
        // A counted or typed object is laid out differently, and this library writes neither.
        if (!this->scan.ok() || !this->prefix.unbounded || this->prefix.typed()) return;

        std::uint64_t found = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                static constexpr auto &key = detail::encoded_key<name>;
                using field_type = std::remove_cvref_t<typename [:std::meta::type_of(member):]>;

                // A member that is not here next - left out, or written later - is not an error:
                // the next member is looked for in the same place, and read_remaining_entries
                // finds by name whatever is never matched this way.
                // Written for speed the marker is the type's own, so key and marker are one
                // constant and the payload one load of a known width; a boolean is all marker,
                // and a short string's marker and length marker are constant too. Written for
                // size the marker is whatever held the value, and is looked at below.
                if constexpr (fixed_width<field_type>) {
                    if (this->at(key_then<name, detail::own_marker<field_type>()>, sizeof(field_type))) {
                        this->scan.advance(key.size() + 1);
                        this->value.[:member:] = detail::load<field_type>(this->scan.position);
                        this->scan.advance(sizeof(field_type));
                        found |= [] { return bit_of(member); }();
                        continue;
                    }
                } else if constexpr (std::same_as<field_type, bool>) {
                    if (this->at(key_then<name, marker::boolean_true>, 0)) {
                        this->scan.advance(key.size() + 1);
                        this->value.[:member:] = true;
                        found |= [] { return bit_of(member); }();
                        continue;
                    }
                    if (this->at(key_then<name, marker::boolean_false>, 0)) {
                        this->scan.advance(key.size() + 1);
                        this->value.[:member:] = false;
                        found |= [] { return bit_of(member); }();
                        continue;
                    }
                } else if constexpr (std::same_as<field_type, std::string>) {
                    if (this->at(key_then<name, marker::string, marker::uint8>, 1)) {
                        const auto length = static_cast<std::size_t>(this->scan.position[key.size() + 2]);
                        if (this->scan.remaining() >= key.size() + 3 + length) {
                            this->scan.advance(key.size() + 3);
                            this->value.[:member:].assign(reinterpret_cast<const char *>(this->scan.position), length);
                            this->scan.advance(length);
                            found |= [] { return bit_of(member); }();
                            continue;
                        }
                    }
                }
                if (this->at(key)) {
                    this->scan.advance(key.size());
                    if (!this->template read_value<member>(this->marker_after_key())) break;
                }
            }
        }
        this->seen |= found;
    }

    void read_remaining_entries() {
        while (this->at_next_entry()) {
            const auto key = detail::read_key(this->scan);
            if (!this->scan.ok()) return;
            if (!this->read_member_named(key, this->value_marker())) return;
            if (!this->prefix.unbounded) --this->entries_left;
        }
    }

    /** Whether the object was well formed, every value was one its member could hold, and
     *  every member the type insists on was there. */
    [[nodiscard]] bool succeeded() const noexcept {
        return this->scan.ok() && this->every_value_read && (this->seen & required) == required;
    }

private:
    /** Whether these bytes are next, with at least `then` more after them. */
    template<std::size_t Width>
    SERPENT_ALWAYS_INLINE [[nodiscard]] bool at(const std::array<std::byte, Width> &bytes, std::size_t then = 1) const noexcept {
        return this->scan.remaining() >= Width + then && std::memcmp(this->scan.position, bytes.data(), Width) == 0;
    }

    /** A number that is not a boolean: under the marker of its own type its payload is itself. */
    template<typename Field>
    static constexpr bool fixed_width =
            (std::integral<Field> && !std::same_as<Field, bool>) || std::floating_point<Field>;

    /** A key as this library writes it, and the markers that follow it. */
    template<const std::string_view &Name, marker... Markers>
    static constexpr auto key_then = [] {
        constexpr auto &key = detail::encoded_key<Name>;
        std::array<std::byte, key.size() + sizeof...(Markers)> bytes {};
        std::ranges::copy(key, bytes.begin());
        std::size_t index = key.size();
        ((bytes[index++] = static_cast<std::byte>(Markers)), ...);
        return bytes;
    }();

    /** The marker at() has already found room for. Whether it opens a value is read_value's to say. */
    SERPENT_ALWAYS_INLINE [[nodiscard]] marker marker_after_key() noexcept {
        if (this->prefix.typed()) return this->prefix.element;
        const auto kind = static_cast<marker>(this->scan.peek());
        this->scan.advance(1);
        return kind;
    }

    /** Steps to the next entry's key, or past the end of the object and answers false. */
    [[nodiscard]] bool at_next_entry() noexcept {
        if (!this->scan.ok() || this->prefix.body == nullptr) return false;

        while (this->scan.available(1) && this->scan.peek_marker() == marker::noop) this->scan.advance(1);

        if (!this->prefix.unbounded) return this->entries_left != 0 && this->scan.need(1);
        if (!this->scan.need(1)) return false;
        if (this->scan.peek_marker() != marker::object_end) return true;
        this->scan.advance(1);
        return false;
    }

    /** The marker that introduces the next value, which a strongly typed object leaves out. */
    [[nodiscard]] marker value_marker() noexcept {
        if (this->prefix.typed()) return this->prefix.element;
        if (!this->scan.need(1)) return marker::invalid;
        const auto kind = static_cast<marker>(this->scan.peek());
        this->scan.advance(1);
        return kind;
    }

    [[nodiscard]] bool read_member_named(std::string_view key, marker kind) {
        bool known = false;
        bool went_on = true;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                if (!known && detail::key_matches<name>(key)) {
                    known = true;
                    went_on = this->template read_value<member>(kind);
                }
            }
        }
        if (known) return went_on;

        // A key this type does not name: its value is stepped over, whatever it is.
        if (!is_value(kind)) {
            if (this->scan.ok()) this->scan.fail(errc::unexpected_marker, this->scan.position - 1);
            return false;
        }
        detail::skip_value(this->scan, kind, 1);
        return this->scan.ok();
    }

    /**
     * Reads the value under `kind` into a member, leaving the cursor after the value whether or
     * not it was one the member could hold. False only where the document cannot be read on.
     *
     * A member that is a number, a boolean or a string is read as that, straight off the cursor,
     * and a marker it cannot be read from is only then asked whether it opens a value at all -
     * so the usual case looks at the marker once.
     */
    template<std::meta::info Member>
    SERPENT_ALWAYS_INLINE [[nodiscard]] bool read_value(marker kind) {
        this->seen |= [] { return bit_of(Member); }();

        auto &field = this->value.[:Member:];
        using field_type = std::remove_cvref_t<decltype(field)>;
        constexpr auto tag = serpent::detail::annotation_of<tagged>(Member);

        if constexpr (!tag.has_value() && direct::readable<field_type>) {
            if (direct::read(this->scan, kind, field)) return this->scan.ok();
        }
        return this->read_value_through_view<Member>(kind);
    }

    /** Anything else - and a value that was not what its member is - through a view of it, then stepped over. */
    template<std::meta::info Member>
    [[nodiscard]] bool read_value_through_view(marker kind) {
        if (!is_value(kind)) {
            if (this->scan.ok()) this->scan.fail(errc::unexpected_marker, this->scan.position - 1);
            return false;
        }

        auto &field = this->value.[:Member:];
        const view held { kind, this->buffer, this->scan.position };
        if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member); tag.has_value()) {
            using declared = [:std::meta::type_of(Member):];
            auto wrapper = make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field);
            if (!read_into(held, wrapper)) this->every_value_read = false;
        } else {
            if (!read_into(held, field)) this->every_value_read = false;
        }
        detail::skip_value(this->scan, kind, 1);
        return this->scan.ok();
    }
};

namespace detail {

/** Reads one object at a cursor that stands just after its opening brace, and consumes it. */
template<reflected_type T>
bool read_object_body(detail::cursor &scan, const std::span<const std::byte> buffer, T &value) {
    object_filler<T> filler { scan, buffer, value };
    filler.read_members_as_written();
    filler.read_remaining_entries();
    return filler.succeeded();
}

} // namespace detail

/**
 * Fills a reflected type from an object.
 *
 * Found by argument-dependent lookup from serializer<T>::read, so a source that has no such
 * function - json::reader - simply does not take this path.
 */
template<typename T>
    requires reflected_type<T>
bool read_reflected(const view &source, T &value) {
    if (!source.is_object()) return false;
    detail::cursor scanner { source.buffer(), source.data() };
    return detail::read_object_body(scanner, source.buffer(), value);
}

/**
 * Fills a container of reflected objects, walking the document once.
 *
 * Without this the elements are read through the array iterator, which knows nothing of what
 * the reader above consumed and skips each element again to find the next - so every record is
 * parsed twice. Returns nothing for a shape it does not handle, leaving the generic path to it.
 */
template<typename C, typename T = std::remove_cvref_t<typename C::value_type>>
    requires reflected_type<T> && requires(C &out) {
        out.clear();
        out.emplace_back();
    }
std::optional<bool> read_sequence(const view &source, C &out) {
    if (!source.is_array()) return std::nullopt;

    const auto info = source.container_header();
    if (info.body == nullptr) return std::nullopt;
    // A typed array of anything but objects is not a sequence of these.
    if (info.typed() && info.element != marker::object_begin) return std::nullopt;

    detail::cursor scanner { source.buffer(), info.body };
    std::uint64_t remaining = info.count;
    const bool counted = !info.unbounded;

    out.clear();
    if (counted) out.reserve(static_cast<std::size_t>(info.count));

    while (scanner.ok()) {
        while (scanner.position < scanner.limit && to_marker(*scanner.position) == marker::noop)
            ++scanner.position;

        if (counted) {
            if (remaining == 0) break;
            if (scanner.position >= scanner.limit) return false;
        } else if (scanner.position >= scanner.limit) {
            return false;
        } else if (to_marker(*scanner.position) == marker::array_end) {
            ++scanner.position;
            break;
        }

        if (!info.typed()) {
            if (to_marker(*scanner.position) != marker::object_begin) return false;
            ++scanner.position;
        }

        auto &slot = out.emplace_back();
        if (!detail::read_object_body(scanner, source.buffer(), slot)) return false;
        if (counted && remaining > 0) --remaining;
    }
    return true;
}

#if SERPENT_BOUNDED_OBJECT_WRITE

/** The most bytes one member can take - key, marker and payload - or zero if it has no limit. */
template<typename T, std::meta::info Member>
struct widest_member {
    static constexpr std::size_t value = [] () -> std::size_t {
        if (serpent::detail::has_annotation<skip>(Member)) return 0;
        if (serpent::detail::annotation_of<tagged>(Member).has_value()) return 0;

        using field = std::remove_cvref_t<typename [:std::meta::type_of(Member):]>;
        if (!std::integral<field> && !std::floating_point<field>) return 0;

        static constexpr std::string_view name = serpent::detail::field_key<T, Member>();
        return detail::encoded_key<name>.size() + writer::widest_marked;
    }();
};

/**
 * Writes one object of a described type.
 *
 * A boolean or a number is a marker and at most eight bytes, and its key is a constant, so a run
 * of such members has a longest possible length: room for the run is claimed once and keys and
 * values are stored into it one after another, where each key and each value would otherwise
 * ask for room of its own. Any other member - a string, a container, another described type -
 * is written the usual way, and the run ends before it and another may begin after. Which
 * members are in which run is settled when this is compiled - see member_runs.hpp.
 */
template<prefer Preference, typename T>
class object_writer {
    using runs = serpent::detail::member_runs<T, widest_member>;
    using writer = basic_writer<Preference>;

    writer &out;
    const T &value;

public:
    object_writer(writer &out, const T &value) noexcept : out(out), value(value) {}

    void write_members() {
        template for (constexpr auto member : runs::members) {
            constexpr std::size_t position = runs::position_of(member);

            if constexpr (serpent::detail::has_annotation<skip>(member)) {
                // Not written at all.
            } else if constexpr (runs::widest[position] == 0) {
                this->write_member<member>();
            } else if constexpr (runs::begins_run(position)) {
                // Writes every member of the run, so the rest of it have nothing left to do here.
                if (!this->write_run<position, runs::run_end(position)>())
                    this->write_run_one_at_a_time<position, runs::run_end(position)>();
            }
        }
    }

private:
    /** One member, the usual way: its key, then whatever writes its value. */
    template<std::meta::info Member>
    void write_member() {
        static constexpr std::string_view name = serpent::detail::field_key<T, Member>();
        const auto &field = this->value.[:Member:];

        this->out.template key_literal<name>();
        if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member); tag.has_value()) {
            using declared = [:std::meta::type_of(Member):];
            static_assert(serpent::detail::variant_like<declared>, "serpent::tagged belongs on a variant field");
            this->out.value(make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field));
        } else {
            this->out.value(field);
        }
    }

    /** The members from `First` up to `Last` stored one after another, if room for them can be had. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] bool write_run() {
        return this->out.compose_members(runs::widest_run(First, Last), [this](char *const to) {
            char *at = to;
            template for (constexpr auto member : runs::members) {
                if constexpr (runs::within(member, First, Last)) {
                    static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                    static constexpr auto &key = detail::encoded_key<name>;
                    std::memcpy(at, key.data(), key.size());
                    at = writer::write_marked(at + key.size(), writer::marked_form(this->value.[:member:]));
                }
            }
            return static_cast<std::size_t>(at - to);
        });
    }

    /** The same members when that room could not be had: each the usual way. */
    template<std::size_t First, std::size_t Last>
    void write_run_one_at_a_time() {
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last)) this->write_member<member>();
        }
    }
};

/**
 * Writes a described type as a BJData object.
 *
 * Found by ordinary lookup from serializer<T>::write, as read_reflected is, so that a writer with
 * no such function does not take this path.
 */
template<prefer Preference, reflected_type T>
    requires (!serpent::detail::is_discriminated<T>())
void write_reflected(basic_writer<Preference> &out, const T &value) {
    static_assert(serpent::detail::keys_are_distinct<T>(), serpent::detail::duplicate_key_message<T>());
    static_assert(serpent::detail::annotations_make_sense<T>(), serpent::detail::annotation_complaint<T>());
    const auto scope = out.object();
    object_writer<Preference, T> { out, value }.write_members();
}

#endif // SERPENT_BOUNDED_OBJECT_WRITE

#endif

} // namespace serpent::bjdata
