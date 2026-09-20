#pragma once

// Writing a range of described records as a draft 4 Structure-of-Arrays table.
//
// Whether a type can be a record of a table is decided from the type alone: every member has
// to be something a schema can say - a number, a boolean, a character, text, an enumeration,
// a nested record of the same kind, or a fixed run of such - so the schema is the same for
// every document the type produces. One thing is decided per table, from the data: how each
// text column is carried, as a dictionary when there are few distinct values and as a table of
// offsets otherwise, or as single characters when every value is one.
//
// The records are then one composed claim each, their members stored back to back with no
// keys and no markers: that is what a table saves, and why a table of numbers is close to a
// copy of them.
//
// Included by bjdata/reflect.hpp; not for including on its own.

#include <serpent/config.hpp>
#include <serpent/bjdata/marker.hpp>
#include <serpent/bjdata/soa.hpp>
#include <serpent/bjdata/writer.hpp>
#include <serpent/concepts.hpp>
#include <serpent/reflect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <string_view>
#include <vector>

#if SERPENT_HAS_REFLECTION

namespace serpent::bjdata::soa {

/** What a member is to a schema. */
enum class column : std::uint8_t {
    none,               ///< nothing a schema can say: the type is not a record
    scalar,             ///< a number at its declared width
    boolean,
    character,
    text,               ///< decided per table: characters, a dictionary or an offset table
    enumeration_text,   ///< a mapped enumeration whose forms are all text: a dictionary of them
    enumeration_number, ///< a mapped enumeration whose forms are all integers: the narrowest that holds them
    record,             ///< a nested record
    run,                ///< a fixed run of elements
};

template<typename T>
consteval column column_of();

template<typename T>
consteval bool mappable() {
    return column_of<T>() != column::none;
}

/** A described type every member of which a schema can say. */
template<typename T>
consteval bool is_table_record() {
    if constexpr (!reflected_type<T>) {
        return false;
    } else {
        bool every = true;
        template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<T>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                using field = std::remove_cvref_t<typename [:std::meta::type_of(member):]>;
                if constexpr (serpent::detail::annotation_of<tagged>(member).has_value() || !mappable<field>()) every = false;
            }
        }
        return every;
    }
}

template<typename T>
consteval column column_of() {
    using bare = std::remove_cvref_t<T>;
    if constexpr (std::same_as<bare, bool>) {
        return column::boolean;
    } else if constexpr (std::same_as<bare, char>) {
        return column::character;
    } else if constexpr (std::integral<bare> || std::floating_point<bare>) {
        return column::scalar;
    } else if constexpr (mapped_enum<bare>) {
        constexpr auto forms = enum_wire_forms<bare>();
        if (forms.empty()) return column::none;
        bool text = true;
        bool number = true;
        for (const auto &form : forms) {
            if (form.wire.held != as::kind::text) text = false;
            if (form.wire.held != as::kind::integer) number = false;
        }
        return text ? column::enumeration_text : number ? column::enumeration_number : column::none;
    } else if constexpr (std::is_enum_v<bare>) {
        return column::scalar;
    } else if constexpr (std::same_as<bare, std::string> || std::same_as<bare, std::string_view>) {
        return column::text;
    } else if constexpr (reflected_type<bare>) {
        return is_table_record<bare>() ? column::record : column::none;
    } else if constexpr (serpent::detail::fixed_sequence<bare> && !serpent::detail::byte_range<bare>) {
        using element = std::remove_cvref_t<std::ranges::range_value_t<bare>>;
        return std::tuple_size_v<bare> > 0 && mappable<element>() ? column::run : column::none;
    } else {
        return column::none;
    }
}

template<typename T>
concept table_record = is_table_record<T>();

/** The marker a scalar member is stored under: its own width, or an enumeration's underlying. */
template<typename T>
consteval marker scalar_marker() {
    if constexpr (std::is_enum_v<T>) return bjdata::detail::own_marker<std::underlying_type_t<T>>();
    else return bjdata::detail::own_marker<T>();
}

/** The narrowest integer marker whose range holds every form of a number-valued enumeration. */
template<typename E>
consteval marker enumeration_marker() {
    constexpr auto forms = enum_wire_forms<E>();
    std::int64_t least = forms[0].wire.whole;
    std::int64_t most = least;
    for (const auto &form : forms) {
        if (form.wire.whole < least) least = form.wire.whole;
        if (form.wire.whole > most) most = form.wire.whole;
    }
    if (least >= 0) {
        if (most <= 0xFF) return marker::uint8;
        if (most <= 0xFFFF) return marker::uint16;
        if (most <= 0xFFFFFFFFll) return marker::uint32;
        return marker::uint64;
    }
    if (least >= -128 && most <= 127) return marker::int8;
    if (least >= -32768 && most <= 32767) return marker::int16;
    if (least >= -2147483648ll && most <= 2147483647ll) return marker::int32;
    return marker::int64;
}

/** How many text columns a record has, nested ones included, in schema order. */
template<typename T>
consteval std::size_t text_columns() {
    using bare = std::remove_cvref_t<T>;
    constexpr column kind = column_of<bare>();
    if constexpr (kind == column::text) {
        return 1;
    } else if constexpr (kind == column::record) {
        std::size_t total = 0;
        template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
            if constexpr (!serpent::detail::has_annotation<skip>(member))
                total += text_columns<typename [:std::meta::type_of(member):]>();
        }
        return total;
    } else if constexpr (kind == column::run) {
        return std::tuple_size_v<bare> * text_columns<std::ranges::range_value_t<bare>>();
    } else {
        return 0;
    }
}

/** The narrowest unsigned marker that counts to `most`. */
[[nodiscard]] constexpr marker unsigned_marker_for(std::uint64_t most) noexcept {
    return most <= 0xFF ? marker::uint8 : most <= 0xFFFF ? marker::uint16 : most <= 0xFFFFFFFFull ? marker::uint32 : marker::uint64;
}

/**
 * How one text column of a table is carried, found from its values.
 *
 * Single characters go as `C`. Otherwise the distinct values are gathered, up to a bound: within
 * it they are a dictionary and each record an index into it; past it the column is an offset
 * table, one entry a record, and the gathering stops. The entries view the records' own text,
 * which outlives the write.
 */
class text_column {
public:
    static constexpr std::size_t dictionary_bound = 255;

private:
    static constexpr std::size_t slots = 512; ///< twice the bound, rounded to a power of two

    std::vector<std::string_view> entries;
    std::array<std::uint16_t, slots> table {}; ///< entry index + 1 by hash, open-addressed; 0 is empty
    std::uint64_t bytes = 0;                    ///< every value's length summed, for an offset table
    std::uint64_t records = 0;
    bool characters = true;
    bool overflowed = false;

    /**
     * The length and the first and last eight bytes, mixed: enough to spread names that
     * differ anywhere near either end, at a cost that does not grow with the text. A
     * collision costs the comparison that decides anyway.
     */
    SERPENT_ALWAYS_INLINE [[nodiscard]] static std::uint64_t load_up_to_eight(const char *from, std::size_t count) noexcept {
        std::uint64_t word = 0;
        if (count >= 8) {
            std::memcpy(&word, from, 8);
        } else {
            for (std::size_t index = 0; index < count; ++index)
                word |= static_cast<std::uint64_t>(static_cast<unsigned char>(from[index])) << (index * 8);
        }
        return word;
    }

    SERPENT_ALWAYS_INLINE [[nodiscard]] static std::size_t hash(std::string_view text) noexcept {
        const std::size_t take = std::min(text.size(), std::size_t { 8 });
        const std::uint64_t head = load_up_to_eight(text.data(), take);
        const std::uint64_t tail = load_up_to_eight(text.data() + text.size() - take, take);
        std::uint64_t mixed = (head ^ (tail * 0x9E3779B97F4A7C15ull)) + text.size();
        mixed ^= mixed >> 32;
        mixed *= 0xD6E8FEB86659FD93ull;
        mixed ^= mixed >> 32;
        return static_cast<std::size_t>(mixed) & (slots - 1);
    }

public:
    text_column() { this->entries.reserve(16); }

    /** Offers one record's value. */
    void note(std::string_view text) {
        ++this->records;
        this->bytes += text.size();
        if (text.size() != 1 || static_cast<unsigned char>(text[0]) > 127) this->characters = false;
        if (this->overflowed) return;
        std::size_t slot = hash(text);
        while (this->table[slot] != 0) {
            if (same(this->entries[this->table[slot] - 1], text)) return;
            slot = (slot + 1) & (slots - 1);
        }
        if (this->entries.size() == dictionary_bound) {
            this->overflowed = true;
            return;
        }
        this->entries.push_back(text);
        this->table[slot] = static_cast<std::uint16_t>(this->entries.size());
    }

    [[nodiscard]] field_kind kind() const noexcept {
        if (this->characters && this->records > 0) return field_kind::scalar;
        return this->overflowed ? field_kind::offsets : field_kind::dictionary;
    }

    /** The marker of what each record stores: `C`, a dictionary index, or an offset index. */
    [[nodiscard]] marker index_marker() const noexcept {
        switch (this->kind()) {
        case field_kind::scalar: return marker::character;
        case field_kind::dictionary: return unsigned_marker_for(this->entries.size());
        default: return unsigned_marker_for(this->bytes);
        }
    }

    [[nodiscard]] std::size_t width() const noexcept { return payload_width(this->index_marker()); }
    [[nodiscard]] const std::vector<std::string_view> &dictionary() const noexcept { return this->entries; }
    [[nodiscard]] std::uint64_t text_bytes() const noexcept { return this->bytes; }

    /** A value's index in the dictionary; only asked once the column is known to be one. */
    [[nodiscard]] std::uint64_t index_of(std::string_view text) const noexcept {
        std::size_t slot = hash(text);
        while (this->table[slot] != 0) {
            if (same(this->entries[this->table[slot] - 1], text)) return this->table[slot] - 1;
            slot = (slot + 1) & (slots - 1);
        }
        return 0;
    }

    /** Equality with the length settled first and short text compared in place, not by a call. */
    SERPENT_ALWAYS_INLINE [[nodiscard]] static bool same(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        if (left.size() <= 16) {
            const std::size_t take = std::min(left.size(), std::size_t { 8 });
            return load_up_to_eight(left.data(), take) == load_up_to_eight(right.data(), take)
                    && load_up_to_eight(left.data() + left.size() - take, take) == load_up_to_eight(right.data() + right.size() - take, take);
        }
        return std::memcmp(left.data(), right.data(), left.size()) == 0;
    }
};

/** The text columns of a record type, in schema order, each decided from the records. */
template<typename T>
class text_plan {
    std::array<text_column, text_columns<T>()> columns {};

    template<typename Field>
    void note(const Field &value, std::size_t &next) {
        using bare = std::remove_cvref_t<Field>;
        constexpr column kind = column_of<bare>();
        if constexpr (kind == column::text) {
            this->columns[next++].note(std::string_view { value });
        } else if constexpr (kind == column::record) {
            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member)) this->note(value.[:member:], next);
            }
        } else if constexpr (kind == column::run) {
            for (const auto &element : value) this->note(element, next);
        }
    }

public:
    template<std::ranges::input_range R>
    explicit text_plan(const R &records) {
        for (const auto &record : records) {
            std::size_t next = 0;
            this->note(record, next);
        }
    }

    [[nodiscard]] const text_column &operator[](std::size_t index) const noexcept { return this->columns[index]; }
    [[nodiscard]] std::size_t size() const noexcept { return this->columns.size(); }
};

/** Writes a table of the records in `items` through `out`, which has the container marker still to write. */
template<prefer Preference, std::ranges::forward_range R>
class table_writer {
    using record_type = std::remove_cvref_t<std::ranges::range_value_t<R>>;
    static_assert(table_record<record_type>);

    basic_writer<Preference> &out;
    const R &items;
    const std::uint64_t count;
    const text_plan<record_type> plan;

    // ---------------- the schema ----------------

    void write_name(std::string_view name) noexcept {
        this->out.put_length(name.size());
        this->out.put_text(name);
    }

    template<typename Field>
    void write_type(std::size_t &next_text) noexcept {
        using bare = std::remove_cvref_t<Field>;
        constexpr column kind = column_of<bare>();
        if constexpr (kind == column::scalar) {
            this->out.put_marker(scalar_marker<bare>());
        } else if constexpr (kind == column::boolean) {
            this->out.put_marker(marker::boolean_true);
        } else if constexpr (kind == column::character) {
            this->out.put_marker(marker::character);
        } else if constexpr (kind == column::enumeration_number) {
            this->out.put_marker(enumeration_marker<bare>());
        } else if constexpr (kind == column::enumeration_text) {
            constexpr auto forms = enum_wire_forms<bare>();
            this->out.put_marker(marker::array_begin);
            this->out.put_marker(marker::strong_type);
            this->out.put_marker(marker::string);
            this->out.put_marker(marker::count);
            this->out.put_length(forms.size());
            for (const auto &form : forms) this->write_name(form.wire.text());
        } else if constexpr (kind == column::text) {
            const auto &column = this->plan[next_text++];
            switch (column.kind()) {
            case field_kind::scalar: this->out.put_marker(marker::character); break;
            case field_kind::dictionary:
                this->out.put_marker(marker::array_begin);
                this->out.put_marker(marker::strong_type);
                this->out.put_marker(marker::string);
                this->out.put_marker(marker::count);
                this->out.put_length(column.dictionary().size());
                for (const auto entry : column.dictionary()) this->write_name(entry);
                break;
            default:
                this->out.put_marker(marker::array_begin);
                this->out.put_marker(marker::strong_type);
                this->out.put_marker(column.index_marker());
                this->out.put_marker(marker::array_end);
                break;
            }
        } else if constexpr (kind == column::record) {
            this->out.put_marker(marker::object_begin);
            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                    static constexpr std::string_view name = serpent::detail::field_key<bare, member>();
                    this->write_name(name);
                    this->write_type<typename [:std::meta::type_of(member):]>(next_text);
                }
            }
            this->out.put_marker(marker::object_end);
        } else if constexpr (kind == column::run) {
            this->out.put_marker(marker::array_begin);
            for (std::size_t index = 0; index < std::tuple_size_v<bare>; ++index)
                this->write_type<std::ranges::range_value_t<bare>>(next_text);
            this->out.put_marker(marker::array_end);
        }
    }

    // ---------------- a record's bytes ----------------

    template<typename Field>
    [[nodiscard]] std::size_t width_of(std::size_t &next_text) const noexcept {
        using bare = std::remove_cvref_t<Field>;
        constexpr column kind = column_of<bare>();
        if constexpr (kind == column::scalar) {
            return payload_width(scalar_marker<bare>());
        } else if constexpr (kind == column::boolean || kind == column::character) {
            return 1;
        } else if constexpr (kind == column::enumeration_number) {
            return payload_width(enumeration_marker<bare>());
        } else if constexpr (kind == column::enumeration_text) {
            return payload_width(unsigned_marker_for(enum_wire_forms<bare>().size()));
        } else if constexpr (kind == column::text) {
            return this->plan[next_text++].width();
        } else if constexpr (kind == column::record) {
            std::size_t total = 0;
            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member))
                    total += this->width_of<typename [:std::meta::type_of(member):]>(next_text);
            }
            return total;
        } else {
            std::size_t total = 0;
            for (std::size_t index = 0; index < std::tuple_size_v<bare>; ++index)
                total += this->width_of<std::ranges::range_value_t<bare>>(next_text);
            return total;
        }
    }

    template<typename Stored>
    [[nodiscard]] static char *store(char *to, Stored value) noexcept {
        const nonstd::unaligned_little<Stored> storage { value };
        std::memcpy(to, storage.data(), nonstd::unaligned_little<Stored>::storage_bytes);
        return to + nonstd::unaligned_little<Stored>::storage_bytes;
    }

    [[nodiscard]] static char *store_index(char *to, marker width, std::uint64_t index) noexcept {
        switch (width) {
        case marker::uint8: return store(to, static_cast<std::uint8_t>(index));
        case marker::uint16: return store(to, static_cast<std::uint16_t>(index));
        case marker::uint32: return store(to, static_cast<std::uint32_t>(index));
        default: return store(to, index);
        }
    }

    template<typename Field>
    [[nodiscard]] char *store_field(char *to, const Field &value, std::size_t &next_text, std::uint64_t record) noexcept {
        using bare = std::remove_cvref_t<Field>;
        constexpr column kind = column_of<bare>();
        if constexpr (kind == column::scalar) {
            if constexpr (std::is_enum_v<bare>) return store(to, std::to_underlying(value));
            else return store(to, value);
        } else if constexpr (kind == column::boolean) {
            *to = static_cast<char>(value ? marker::boolean_true : marker::boolean_false);
            return to + 1;
        } else if constexpr (kind == column::character) {
            *to = value;
            return to + 1;
        } else if constexpr (kind == column::enumeration_number || kind == column::enumeration_text) {
            constexpr auto forms = enum_wire_forms<bare>();
            std::size_t found = forms.size();
            for (std::size_t index = 0; index < forms.size(); ++index)
                if (forms[index].value == value) found = index;
            if (found == forms.size()) {
                for (std::size_t index = 0; index < forms.size(); ++index)
                    if (forms[index].is_fallback) found = index;
            }
            if (found == forms.size()) {
                this->out.fail(errc::type_mismatch);
                found = 0;
            }
            if constexpr (kind == column::enumeration_number) {
                return store_index_signed(to, enumeration_marker<bare>(), forms[found].wire.whole);
            } else {
                return store_index(to, unsigned_marker_for(forms.size()), found);
            }
        } else if constexpr (kind == column::text) {
            const auto &column = this->plan[next_text++];
            const std::string_view text { value };
            switch (column.kind()) {
            case field_kind::scalar:
                *to = text[0];
                return to + 1;
            case field_kind::dictionary: return store_index(to, column.index_marker(), column.index_of(text));
            default: return store_index(to, column.index_marker(), record);
            }
        } else if constexpr (kind == column::record) {
            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member))
                    to = this->store_field(to, value.[:member:], next_text, record);
            }
            return to;
        } else {
            for (const auto &element : value) to = this->store_field(to, element, next_text, record);
            return to;
        }
    }

    [[nodiscard]] static char *store_index_signed(char *to, marker width, std::int64_t value) noexcept {
        switch (width) {
        case marker::uint8: return store(to, static_cast<std::uint8_t>(value));
        case marker::uint16: return store(to, static_cast<std::uint16_t>(value));
        case marker::uint32: return store(to, static_cast<std::uint32_t>(value));
        case marker::uint64: return store(to, static_cast<std::uint64_t>(value));
        case marker::int8: return store(to, static_cast<std::int8_t>(value));
        case marker::int16: return store(to, static_cast<std::int16_t>(value));
        case marker::int32: return store(to, static_cast<std::int32_t>(value));
        default: return store(to, value);
        }
    }

    // ---------------- the tables after the payload ----------------

    template<typename Field>
    void gather_offsets(const Field &value, std::size_t &next_text, std::size_t wanted, std::uint64_t &offset, marker width, bool text_pass) noexcept {
        using bare = std::remove_cvref_t<Field>;
        constexpr column kind = column_of<bare>();
        if constexpr (kind == column::text) {
            if (next_text++ == wanted) {
                const std::string_view text { value };
                if (text_pass) {
                    this->out.put_text(text);
                } else {
                    offset += text.size();
                    this->out.put(std::span<const std::byte> { reinterpret_cast<const std::byte *>(offset_bytes(width, offset).data()), payload_width(width) });
                }
            }
        } else if constexpr (kind == column::record) {
            template for (constexpr auto member : std::define_static_array(serpent::detail::members_including_bases<bare>())) {
                if constexpr (!serpent::detail::has_annotation<skip>(member))
                    this->gather_offsets(value.[:member:], next_text, wanted, offset, width, text_pass);
            }
        } else if constexpr (kind == column::run) {
            for (const auto &element : value) this->gather_offsets(element, next_text, wanted, offset, width, text_pass);
        }
    }

    [[nodiscard]] static std::array<char, 8> offset_bytes(marker width, std::uint64_t offset) noexcept {
        std::array<char, 8> bytes {};
        std::ignore = store_index(bytes.data(), width, offset);
        return bytes;
    }

public:
    table_writer(basic_writer<Preference> &out, const R &items) noexcept
    : out(out)
    , items(items)
    , count(static_cast<std::uint64_t>(std::ranges::distance(items)))
    , plan(items) {}

    void write() noexcept {
        // The header: `[$`, the schema, `#`, the count.
        this->out.put_marker(marker::array_begin);
        this->out.put_marker(marker::strong_type);
        std::size_t next_text = 0;
        this->write_type<record_type>(next_text);
        this->out.put_marker(marker::count);
        this->out.put_length(this->count);

        // The records, one composed claim each.
        next_text = 0;
        const std::size_t record_bytes = this->width_of<record_type>(next_text);
        std::uint64_t record = 0;
        for (const auto &item : this->items) {
            if (!this->out.compose_record(record_bytes, [&](char *const to) {
                    std::size_t text = 0;
                    return static_cast<std::size_t>(this->store_field(to, item, text, record) - to);
                }))
                return;
            ++record;
        }

        // For each offset column in schema order: count + 1 offsets, then the text.
        for (std::size_t column = 0; column < this->plan.size(); ++column) {
            if (this->plan[column].kind() != field_kind::offsets) continue;
            const auto width = this->plan[column].index_marker();
            std::uint64_t offset = 0;
            this->out.put(std::span<const std::byte> { reinterpret_cast<const std::byte *>(offset_bytes(width, 0).data()), payload_width(width) });
            for (const auto &item : this->items) {
                std::size_t text = 0;
                this->gather_offsets(item, text, column, offset, width, false);
            }
            for (const auto &item : this->items) {
                std::size_t text = 0;
                this->gather_offsets(item, text, column, offset, width, true);
            }
        }
    }
};

/**
 * Writes a range of records as a table. Found by lookup from the writer's range(), which
 * offers any forward range of a table record with at least two elements; fewer are written as
 * the array of objects they would have been, as dart-bjdata writes them.
 */
template<prefer Preference, std::ranges::forward_range R>
    requires (is_table_record<std::remove_cvref_t<std::ranges::range_value_t<R>>>())
void write_table(basic_writer<Preference> &out, const R &items) noexcept {
    table_writer<Preference, R> { out, items }.write();
}

} // namespace serpent::bjdata::soa

#endif // SERPENT_HAS_REFLECTION
