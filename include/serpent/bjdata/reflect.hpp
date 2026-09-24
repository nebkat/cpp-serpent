#pragma once

// Reading a reflected type from BJData without going through the member iterator.
//
// The generic path is written in terms of handles: an iterator that parses an entry into a key
// and a reader, a visitor that compares the key against a name it is handed at run time. That is
// the right shape for a reader that knows nothing about the type. For a type whose fields the
// compiler can enumerate, none of it is necessary - the keys are constants, their lengths are
// constants, and the destination of each is known - so this walks the bytes once and assigns
// straight into the fields.
//
// Binary only. JSON stays on the generic path, where the reader is worth reading.

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/direct.hpp>
#include <serpent/bjdata/reader.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/bjdata/soa_write.hpp>
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
                constexpr bool plain = !serpent::detail::member_is_projected<member>();
                if constexpr (plain && fixed_width<field_type>) {
                    if (this->at(key_then<name, detail::own_marker<field_type>()>, sizeof(field_type))) {
                        this->scan.advance(key.size() + 1);
                        this->value.[:member:] = detail::load<field_type>(this->scan.position);
                        this->scan.advance(sizeof(field_type));
                        found |= [] { return bit_of(member); }();
                        continue;
                    }
                } else if constexpr (plain && std::same_as<field_type, bool>) {
                    if (this->at(key, 1)) {
                        const auto kind = static_cast<marker>(this->scan.position[key.size()]);
                        if (kind == marker::boolean_true || kind == marker::boolean_false) {
                            this->scan.advance(key.size() + 1);
                            this->value.[:member:] = kind == marker::boolean_true;
                            found |= [] { return bit_of(member); }();
                            continue;
                        }
                    }
                } else if constexpr (plain && std::same_as<field_type, std::string>) {
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
            (std::integral<Field> && !std::same_as<Field, bool> && !std::same_as<Field, char>) || std::floating_point<Field>;

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

        if constexpr (!serpent::detail::member_is_projected<Member>() && direct::readable<field_type>) {
            if (direct::read(this->scan, kind, field)) return this->scan.ok();
        }
        return this->read_value_through_view<Member>(kind);
    }

    /** Anything else - and a value that was not what its member is - through a reader of it, then stepped over. */
    template<std::meta::info Member>
    [[nodiscard]] bool read_value_through_view(marker kind) {
        if (!is_value(kind)) {
            if (this->scan.ok()) this->scan.fail(errc::unexpected_marker, this->scan.position - 1);
            return false;
        }

        auto &field = this->value.[:Member:];
        const reader held { kind, this->buffer, this->scan.position };
        auto &&projection = serpent::detail::projected<Member>(field);
        if (!read_into(held, projection)) this->every_value_read = false;
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

template<typename T>
class table_filler;

/**
 * Reads one field of a table into a member, given where the field's bytes for this record are.
 *
 * What a member can take is decided by its type against the field's kind: numbers from a
 * scalar or a boolean byte, text from any of the three text forms, a nested described type from
 * a record, a fixed or growable sequence from an array, an optional from any of those or from
 * a field that is always null. False where the two cannot be reconciled or the value does not
 * fit, as anywhere else.
 */
template<typename T>
bool read_table_field(const soa::table &source, const soa::field &field, const std::byte *at, T &into) {
    using namespace soa;
    if constexpr (serpent::detail::optional_like<T>) {
        if (field.kind == field_kind::null) {
            into.reset();
            return true;
        }
        return read_table_field(source, field, at, into.emplace());
    } else if constexpr (std::same_as<T, bool>) {
        if (field.kind == field_kind::boolean) {
            const auto byte = to_marker(*at);
            if (byte != marker::boolean_true && byte != marker::boolean_false) return false;
            into = byte == marker::boolean_true;
            return true;
        }
        return false;
    } else if constexpr (std::same_as<T, char>) {
        // A character member is a C field, or a one-byte number's bits.
        if (field.kind != field_kind::scalar || field.width != 1) return false;
        into = static_cast<char>(*at);
        return true;
    } else if constexpr (std::integral<T> || std::floating_point<T>) {
        if (field.kind != field_kind::scalar) return false;
        if constexpr (std::integral<T>) {
            if (field.type == marker::character) {
                std::uint8_t byte = 0;
                if (!direct::load_stored<std::uint8_t>(at, field.width, byte)) return false;
                if (!std::in_range<T>(byte)) return false;
                into = static_cast<T>(byte);
                return true;
            }
            return direct::load_integer(field.type, at, field.width, into).has_value();
        } else {
            return direct::load_real(field.type, at, field.width, into).has_value();
        }
    } else if constexpr (mapped_enum<T>) {
        // Text forms come from a dictionary, fixed text or a character; number forms from a
        // scalar. A value that is no form takes the fallback, as anywhere else.
        constexpr auto forms = enum_wire_forms<T>();
        std::optional<std::string_view> text;
        std::optional<std::int64_t> number;
        switch (field.kind) {
        case field_kind::dictionary: {
            std::uint64_t index = 0;
            if (!direct::load_integer(field.type, at, field.width, index)) return false;
            text = source.dictionary_entry(field, index);
            break;
        }
        case field_kind::offsets: {
            std::uint64_t index = 0;
            if (!direct::load_integer(field.type, at, field.width, index)) return false;
            text = source.offset_entry(field, index);
            break;
        }
        case field_kind::text: text = table::fixed_text(field, at); break;
        case field_kind::scalar:
            if (field.type == marker::character) {
                text = std::string_view { reinterpret_cast<const char *>(at), 1 };
            } else {
                std::int64_t whole = 0;
                if (!direct::load_integer(field.type, at, field.width, whole)) return false;
                number = whole;
            }
            break;
        default: return false;
        }
        for (const auto &form : forms) {
            const bool matches = (text && form.wire.held == as::kind::text && form.wire.text() == *text)
                    || (number && form.wire.held == as::kind::integer && form.wire.whole == *number);
            if (matches) {
                into = form.value;
                return true;
            }
        }
        for (const auto &form : forms) {
            if (form.is_fallback) {
                into = form.value;
                return true;
            }
        }
        return false;
    } else if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> underlying {};
        if (field.kind != field_kind::scalar || !direct::load_integer(field.type, at, field.width, underlying)) return false;
        into = static_cast<T>(underlying);
        return true;
    } else if constexpr (serpent::detail::string_like<T> && requires(T &text, std::string_view view) { text.assign(view); }) {
        std::optional<std::string_view> text;
        switch (field.kind) {
        case field_kind::text: text = table::fixed_text(field, at); break;
        case field_kind::dictionary: {
            std::uint64_t index = 0;
            if (!direct::load_integer(field.type, at, field.width, index)) return false;
            text = source.dictionary_entry(field, index);
            break;
        }
        case field_kind::offsets: {
            std::uint64_t index = 0;
            if (!direct::load_integer(field.type, at, field.width, index)) return false;
            text = source.offset_entry(field, index);
            break;
        }
        case field_kind::scalar:
            if (field.type == marker::character) text = std::string_view { reinterpret_cast<const char *>(at), 1 };
            break;
        default: break;
        }
        if (!text) return false;
        into.assign(*text);
        return true;
    } else if constexpr (reflected_type<T>) {
        if (field.kind != field_kind::record) return false;
        return table_filler<T> { source, field }.fill(at, into);
    } else if constexpr (serpent::detail::fixed_sequence<T> || serpent::detail::back_insertable<T>) {
        if (field.kind != field_kind::array) return false;
        const auto &layout = source.fields();
        if constexpr (serpent::detail::back_insertable<T>) into.clear();
        auto slot = std::ranges::begin(into);
        for (std::uint32_t index = field.children; index < field.next; index = layout.fields[index].next) {
            const auto &element = layout.fields[index];
            if constexpr (serpent::detail::back_insertable<T>) {
                if (!read_table_field(source, element, at + element.offset, into.emplace_back())) return false;
            } else {
                if (slot == std::ranges::end(into)) return false;
                if (!read_table_field(source, element, at + element.offset, *slot)) return false;
                ++slot;
            }
        }
        if constexpr (serpent::detail::fixed_sequence<T>) return slot == std::ranges::end(into);
        return true;
    } else {
        return false;
    }
}

/**
 * Fills a described type from records of a table: the schema's fields are matched to the
 * type's members by name once, and then each record is a run of loads at fixed offsets.
 */
template<typename T>
class table_filler {
    static constexpr auto members = std::define_static_array(serpent::detail::members_including_bases<T>());
    static_assert(members.size() <= 64, "a type with more than 64 members needs a wider seen mask");

    const soa::table &source;
    const soa::field *record; ///< the nested record field these members are in, or null at the top
    std::array<const soa::field *, members.size()> located {};
    std::uint64_t seen = 0;

    static constexpr std::uint64_t required = [] {
        std::uint64_t mask = 0;
        std::size_t position = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member) && serpent::detail::member_is_required<T, member>())
                mask |= std::uint64_t { 1 } << position;
            ++position;
        }
        return mask;
    }();

public:
    table_filler(const soa::table &source, const soa::field *record = nullptr) noexcept : source(source), record(record) {
        const auto &layout = source.fields();
        const std::uint32_t first = record == nullptr ? 0 : record->children;
        const std::uint32_t last = record == nullptr ? layout.count() : record->next;
        std::size_t position = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                for (std::uint32_t index = first; index < last; index = layout.fields[index].next) {
                    if (layout.fields[index].name == name) {
                        this->located[position] = &layout.fields[index];
                        this->seen |= std::uint64_t { 1 } << position;
                        break;
                    }
                }
            }
            ++position;
        }
    }

    table_filler(const soa::table &source, const soa::field &record) noexcept : table_filler(source, &record) {}

    /** Whether every member the type insists on has a field. */
    [[nodiscard]] bool complete() const noexcept { return (this->seen & required) == required; }

    /** Fills `value` from the record whose bytes begin at `at`. */
    [[nodiscard]] bool fill(const std::byte *at, T &value) const {
        std::size_t position = 0;
        bool converted = true;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                if (const auto *field = this->located[position]; field != nullptr) {
                    if (!read_table_field(this->source, *field, at + field->offset, value.[:member:])) converted = false;
                }
            }
            ++position;
        }
        return converted;
    }

    /** Fills `value` from record `index` of a table whose records these members are. */
    [[nodiscard]] bool fill_record(std::uint64_t index, T &value) const {
        // At the top, a member's field is a top-level field, placed by the table; the record's
        // own bytes are where the first field is, less its offset.
        std::size_t position = 0;
        bool converted = true;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                if (const auto *field = this->located[position]; field != nullptr) {
                    if (!read_table_field(this->source, *field, this->source.at(*field, index), value.[:member:])) converted = false;
                }
            }
            ++position;
        }
        return converted;
    }
};

/** Fills a container of described types from a table. */
template<typename C, typename T = std::remove_cvref_t<typename C::value_type>>
bool read_table(std::span<const std::byte> buffer, const detail::container_prefix &prefix, bool column_major, C &out) {
    soa::table source;
    if (!source.place(buffer, prefix, column_major)) return false;
    const table_filler<T> filler { source };
    if (!filler.complete()) return false;
    out.clear();
    out.reserve(static_cast<std::size_t>(source.size()));
    for (std::uint64_t index = 0; index < source.size(); ++index) {
        if (!filler.fill_record(index, out.emplace_back())) return false;
    }
    return true;
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
bool read_reflected(const reader &source, T &value) {
    if (!source.is_object()) return false;
    if (const auto &place = source.place_in_table(); place.in_table()) {
        // A record of a table, or a nested record in one: its members are at fixed offsets.
        soa::table layout;
        if (!layout.place_at(source.buffer(), place.container)) return false;
        if (place.is_record()) {
            const detail::table_filler<T> filler { layout };
            return filler.complete() && filler.fill_record(place.record, value);
        }
        const auto &field = layout.fields().fields[place.field];
        if (field.kind != soa::field_kind::record) return false;
        const detail::table_filler<T> filler { layout, field };
        return filler.complete() && filler.fill(source.data(), value);
    }
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
std::optional<bool> read_sequence(const reader &source, C &out) {
    // A table - opening with `[`, or with `{` when laid out column by column, which type()
    // reports as an array either way - has its records placed by its schema rather than marked
    // one by one.
    if (!source.is_array()) return std::nullopt;

    const auto info = source.container_header();
    if (info.body == nullptr) return std::nullopt;
    if (info.structured()) return detail::read_table(source.buffer(), info, source.type_marker() == marker::object_begin, out);
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
        if (serpent::detail::member_is_projected<Member>()) return 0;

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

    /** Whether every member the type writes is in one run, so the whole object has a longest length. */
    static constexpr bool whole_object_bounded = runs::members.size() > 0 && runs::begins_run(0)
            && runs::run_end(0) == runs::members.size();

    /** The object with its braces as one piece, where it can be; false where it was not written. */
    [[nodiscard]] bool write_whole_object() {
        if constexpr (!whole_object_bounded) {
            return false;
        } else {
            constexpr std::size_t all = runs::members.size();
            return this->out.compose_object(2 + runs::widest_run(0, all), [this](char *const to) {
                to[0] = static_cast<char>(marker::object_begin);
                char *const end = this->write_run_into<0, all>(to + 1);
                *end = static_cast<char>(marker::object_end);
                return static_cast<std::size_t>(end + 1 - to);
            });
        }
    }

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
        this->out.value(serpent::detail::projected<Member>(field));
    }

    /** The members from `First` up to `Last` stored one after another, if room for them can be had. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] bool write_run() {
        return this->out.compose_members(runs::widest_run(First, Last), [this](char *const to) {
            return static_cast<std::size_t>(this->write_run_into<First, Last>(to) - to);
        });
    }

    /** The members from `First` up to `Last` into room already claimed, returning where they end. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] char *write_run_into(char *at) {
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                static constexpr auto &key = detail::encoded_key<name>;
                std::memcpy(at, key.data(), key.size());
                at = writer::write_marked(at + key.size(), writer::marked_form(this->value.[:member:]));
            }
        }
        return at;
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
    object_writer<Preference, T> writing { out, value };
    if (writing.write_whole_object()) return;
    const auto scope = out.object();
    writing.write_members();
}

#endif // SERPENT_BOUNDED_OBJECT_WRITE

#endif

} // namespace serpent::bjdata
