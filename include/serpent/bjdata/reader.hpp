#pragma once

#include <serpent/bjdata/detail.hpp>
#include <serpent/bjdata/direct.hpp>
#include <serpent/concepts.hpp>
#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/fwd.hpp>
#include <serpent/bjdata/marker.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <algorithm>
#include <memory>
#include <concepts>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace serpent::bjdata {

namespace detail {

/** nonstd::unaligned_little_span<const U>, the type a typed array is viewed in place as. */
template<typename T>
concept unaligned_span = requires {
    typename T::value_type;
    requires std::same_as<T, nonstd::unaligned_little_span<const std::remove_const_t<typename T::value_type>>>;
};

} // namespace detail

class array_iterator;
class member_iterator;
template<typename Iterator>
class element_range;
using array_range = element_range<array_iterator>;
using member_range = element_range<member_iterator>;

/**
 * @brief A handle to one BJData value inside a buffer, holding no storage of its own.
 *
 * Every accessor is total: a malformed document, a missing key or a value of the wrong type
 * yields an invalid reader or an empty optional rather than throwing. The checked accessors
 * (at, get) wrap those and raise instead, so both styles run over the same bytes and the same
 * single parsing implementation.
 *
 * What the binary reader can do that the JSON one cannot is lend: as<std::string_view>() and
 * as<nonstd::unaligned_little_span<const T>>() hand back the document's own bytes.
 */
class reader {
    marker element = marker::invalid;
    std::span<const std::byte> source {}; ///< the whole document, so bounds and offsets are absolute
    const std::byte *payload = nullptr; ///< first byte after this value's marker
    soa::place table {};                ///< set when this value is a record of a table, or a field of one

    // One past the end of this value, once a walk of it has been completed - for whoever steps
    // over it next, which is the iterator that owns this handle. A forward iterator has to walk
    // a value to find its sibling, so a container read element by element would otherwise be
    // scanned once to read each element and again to step over it, and once more for each
    // level above. Only a completed walk is recorded: a counted container's remaining count is
    // not recoverable from a position alone, so knowing where a walk paused would not do.
    //
    // Which is why a reader can be moved but not copied: a copy would have a note of its own,
    // and a loop written `for (auto child : ...)` would quietly walk every byte twice.
    mutable const std::byte *reached = nullptr;

public:
    constexpr reader() = default;
    constexpr reader(marker element, std::span<const std::byte> source, const std::byte *payload) noexcept
    : element(element)
    , source(source)
    , payload(payload) {}
    reader(const reader &) = delete;
    reader &operator=(const reader &) = delete;
    reader(reader &&) = default;
    reader &operator=(reader &&) = default;

    /** Records where this value ends, so that stepping to the next need not scan it again. */
    void note_end(const std::byte *end) const noexcept { this->reached = end; }

    /** Another handle on the same value, with nothing known about it yet. */
    [[nodiscard]] reader clone() const noexcept {
        reader copy { this->element, this->source, this->payload };
        copy.table = this->table;
        return copy;
    }

    /** Where this value stands in a table, if it is a record or a field of one. */
    [[nodiscard]] constexpr const soa::place &place_in_table() const noexcept { return this->table; }

    /** Wraps a buffer, reading its leading marker. Performs no deep parsing. */
    [[nodiscard]] static reader over(std::span<const std::byte> buffer) noexcept {
        if (buffer.empty()) return {};
        const auto kind = to_marker(buffer.front());
        if (!is_value(kind)) return {};
        return reader { kind, buffer, buffer.data() + 1 };
    }

    [[nodiscard]] constexpr marker type_marker() const noexcept { return this->element; }
    [[nodiscard]] constexpr const std::byte *data() const noexcept { return this->payload; }
    /** The whole document these bytes came from. */
    [[nodiscard]] constexpr std::span<const std::byte> buffer() const noexcept { return this->source; }
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return this->payload == nullptr ? 0 : static_cast<std::size_t>(this->payload - this->source.data());
    }

    [[nodiscard]] constexpr kind type() const noexcept {
        switch (this->element) {
        case marker::null: return kind::null;
        case marker::boolean_true:
        case marker::boolean_false: return kind::boolean;
        case marker::uint8:
        case marker::int8:
        case marker::uint16:
        case marker::int16:
        case marker::uint32:
        case marker::int32:
        case marker::uint64:
        case marker::int64:
        case marker::byte: return kind::integer;
        case marker::float16:
        case marker::float32:
        case marker::float64: return kind::real;
        case marker::character:
        case marker::string:
        case marker::high_precision: return kind::string;
        case marker::array_begin: return kind::array;
        case marker::object_begin:
            // A table laid out column by column opens with `{` but is a sequence of records.
            if (!this->table.in_table() && this->available() >= 2 && to_marker(this->payload[0]) == marker::strong_type
                    && to_marker(this->payload[1]) == marker::object_begin)
                return kind::array;
            return kind::object;
        default: return kind::invalid;
        }
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return this->type() != kind::invalid; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return this->is_valid(); }
    [[nodiscard]] constexpr bool is_null() const noexcept { return this->type() == kind::null; }
    [[nodiscard]] constexpr bool is_boolean() const noexcept { return this->type() == kind::boolean; }
    [[nodiscard]] constexpr bool is_integer() const noexcept { return this->type() == kind::integer; }
    [[nodiscard]] constexpr bool is_real() const noexcept { return this->type() == kind::real; }
    [[nodiscard]] constexpr bool is_number() const noexcept { return this->is_integer() || this->is_real(); }
    [[nodiscard]] constexpr bool is_string() const noexcept { return this->type() == kind::string; }
    [[nodiscard]] constexpr bool is_array() const noexcept { return this->type() == kind::array; }
    [[nodiscard]] constexpr bool is_object() const noexcept { return this->type() == kind::object; }
    [[nodiscard]] bool is_binary() const noexcept { return this->read_binary().has_value(); }


    // ---------------- containers ----------------

    /**
     * This value's payload, everything after its marker. Empty optional if it does not parse.
     *
     * With type_marker() this is enough to splice the value into another document with no
     * re-encoding: write the marker, then copy these bytes. The marker is not included
     * because an element of a typed container has none of its own in the stream.
     */
    [[nodiscard]] std::optional<std::span<const std::byte>> payload_bytes() const noexcept {
        if (!this->is_valid()) return std::nullopt;
        auto scanner = this->scan();
        detail::skip_value(scanner, this->element, 0);
        if (!scanner.ok()) return std::nullopt;
        return std::span<const std::byte> { this->payload, static_cast<std::size_t>(scanner.position - this->payload) };
    }

    [[nodiscard]] detail::header container_header() const noexcept {
        if (this->element != marker::array_begin && this->element != marker::object_begin) return {};
        // A record of a table, or a run of elements in one, has no header: its shape is the schema's.
        if (this->table.in_table()) return {};
        auto scanner = this->scan();
        const auto info = detail::parse_header(scanner, this->element == marker::object_begin);
        if (!scanner.ok()) return {};
        return info;
    }

    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * How many elements are coming, when the document says so without being walked.
     *
     * Nothing for an unbounded container: size() would have to count them, and a caller asking
     * for a hint wants to avoid exactly that.
     */
    [[nodiscard]] std::optional<std::size_t> size_hint() const noexcept;

    [[nodiscard]] array_range array() const & noexcept;
    [[nodiscard]] array_range array() && noexcept;
    [[nodiscard]] member_range items() const & noexcept;
    [[nodiscard]] member_range items() && noexcept;

    [[nodiscard]] reader operator[](std::size_t index) const noexcept;
    [[nodiscard]] reader operator[](std::string_view key) const noexcept;
    [[nodiscard]] reader find(std::string_view key) const noexcept { return (*this)[key]; }

    // ---------------- checked ----------------

    [[nodiscard]] reader at(std::size_t index) const {
        auto result = (*this)[index];
        if (!result.is_valid()) raise(errc::out_of_range, this->offset());
        return result;
    }

    [[nodiscard]] reader at(std::string_view key) const {
        auto result = (*this)[key];
        if (!result.is_valid()) raise(errc::missing_key, this->offset(), key);
        return result;
    }

    /**
     * The value as a T, or nothing if it is not one: a boolean, an integer that fits, a real
     * (from a real or an integer), text as std::string_view (S and H yield their bytes, C one
     * character; never a copy) or as anything made from one, a [$B or [$U array as
     * std::span<const std::byte>, a typed array as nonstd::unaligned_little_span<const U> when
     * its marker is exactly the one U packs as, a container or optional filled from the shape of
     * the document, or a described or converted type.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> as() const noexcept {
        if constexpr (std::same_as<T, bool>)
            return this->read_bool();
        else if constexpr (std::same_as<T, char>) {
            // A character is the byte under a C marker, not a one-byte number.
            const auto text = this->read_text();
            if (!text || text->size() != 1 || this->element != marker::character) return std::nullopt;
            return text->front();
        } else if constexpr (std::same_as<T, std::string_view>)
            return this->read_text();
        else if constexpr (std::same_as<T, std::span<const std::byte>>)
            return this->read_binary();
        else if constexpr (detail::unaligned_span<T>)
            return this->read_span<std::remove_const_t<typename T::value_type>>();
        else if constexpr (serpent::detail::string_like<T> && std::constructible_from<T, std::string_view>) {
            const auto text = this->read_text();
            if (!text) return std::nullopt;
            return T { *text };
        } else if constexpr (std::floating_point<T>)
            return this->read_real<T>();
        else if constexpr (std::integral<T>)
            return this->read_integer<T>();
        else if constexpr (serpent::detail::structurally_readable<T>) {
            // Containers and optionals are filled from the shape of the document, so they
            // need no customization and must not go looking for one.
            T value {};
            if (!read_into(*this, value)) return std::nullopt;
            return value;
        } else {
            T value {};
            if (!serializer<T>::read(*this, value)) return std::nullopt;
            return value;
        }
    }

    /** as<T>(), or errc::type_mismatch thrown. */
    template<typename T>
    [[nodiscard]] T get() const {
        auto result = this->as<T>();
        if (!result) raise(errc::type_mismatch, this->offset());
        return *std::move(result);
    }

private:
    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->source.data() + this->source.size();
    }

    [[nodiscard]] constexpr std::optional<bool> read_bool() const noexcept {
        return direct::boolean_under(this->element);
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> read_integer() const noexcept {
        T value {};
        if (!direct::load_integer(this->element, this->payload, this->available(), value)) return std::nullopt;
        return value;
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> read_real() const noexcept {
        T value {};
        if (!direct::load_real(this->element, this->payload, this->available(), value)) return std::nullopt;
        return value;
    }

    /** S and H yield their raw bytes; C yields a one-character reader. Never copies. */
    [[nodiscard]] std::optional<std::string_view> read_text() const noexcept {
        // Text in a table - fixed, or from an offset table - has no length of its own; the
        // handle was told it. A dictionary entry has one and reads as any string does.
        if (this->table.in_table() && this->table.extent != soa::place::prefixed) {
            if (this->element != marker::string && this->element != marker::high_precision) return std::nullopt;
            return std::string_view { reinterpret_cast<const char *>(this->payload), this->table.extent };
        }
        if (this->element == marker::character) {
            if (this->available() < 1) return std::nullopt;
            return std::string_view { reinterpret_cast<const char *>(this->payload), 1 };
        }
        if (this->element != marker::string && this->element != marker::high_precision) return std::nullopt;

        auto scanner = this->scan();
        const auto text = detail::read_key(scanner);
        if (!scanner.ok()) return std::nullopt;
        return text;
    }

    /** A [$B# or [$U# array, viewed as its raw bytes. */
    [[nodiscard]] std::optional<std::span<const std::byte>> read_binary() const noexcept {
        const auto info = this->container_header();
        if (this->element != marker::array_begin || !info.typed()) return std::nullopt;
        if (info.element != marker::byte && info.element != marker::uint8) return std::nullopt;
        if (info.body == nullptr || info.count > static_cast<std::uint64_t>(this->limit() - info.body))
            return std::nullopt;
        return std::span<const std::byte> { info.body, static_cast<std::size_t>(info.count) };
    }

    /**
     * A typed array whose element marker is exactly the one T packs as, viewed in place.
     * Zero copy demands an exact layout match, so a widening read goes through the ordinary
     * element iterator instead.
     */
    template<typename T>
    [[nodiscard]] std::optional<nonstd::unaligned_little_span<const T>> read_span() const noexcept {
        static_assert(strong_type_for<T>() != marker::invalid, "T does not correspond to a BJData strong type");

        const auto info = this->container_header();
        if (this->element != marker::array_begin || !info.typed()) return std::nullopt;
        if (info.element != strong_type_for<T>()) return std::nullopt;

        const auto width = payload_width(info.element);
        if (info.body == nullptr || info.count > static_cast<std::uint64_t>(this->limit() - info.body) / width)
            return std::nullopt;
        return nonstd::unaligned_little_span<const T> { info.body, static_cast<std::size_t>(info.count) };
    }


    [[nodiscard]] constexpr std::size_t available() const noexcept {
        return this->payload != nullptr && this->payload < this->limit()
                ? static_cast<std::size_t>(this->limit() - this->payload)
                : 0;
    }

    [[nodiscard]] constexpr detail::cursor scan() const noexcept {
        return detail::cursor { this->source, this->payload };
    }

    friend class array_iterator;
    friend class member_iterator;
    friend reader move_to(reader &, marker, const std::byte *) noexcept;
    friend void place_field(reader &, const soa::table &, const soa::field &, const std::byte *) noexcept;
    friend void place_record(reader &, const std::byte *, std::uint64_t) noexcept;
};

/** A key and its value, for structured bindings over items(). */
struct key_value {
    std::string_view key;
    reader value;

    /** Present so this and json_key_value offer the same interface to generic code. */
    [[nodiscard]] constexpr bool key_is(std::string_view other) const noexcept { return this->key == other; }
    [[nodiscard]] std::string key_string() const { return std::string { this->key }; }
};

/** Points a handle at another value of the same document, with nothing known about it yet. */
inline reader move_to(reader &handle, marker element, const std::byte *payload) noexcept {
    handle.element = element;
    handle.payload = payload;
    handle.reached = nullptr;
    handle.table = {};
    return {};
}

/** Points a handle at record `index` of the table whose container marker is at `container`. */
inline void place_record(reader &handle, const std::byte *container, std::uint64_t index) noexcept {
    handle.element = marker::object_begin;
    handle.payload = container;
    handle.reached = nullptr;
    handle.table = { .container = container, .field = soa::place::whole_record, .record = static_cast<std::uint32_t>(index) };
}

/**
 * Points a handle at one field of a record, whose bytes begin at `at`. A scalar, a boolean and
 * a null are given the marker their bytes would have carried, so that every question a handle
 * answers about a value is answered the same way; a dictionary entry is a string with its
 * length prefix; fixed and offset text are strings told their length; a nested record and a
 * fixed run of elements keep their place in the table for whoever walks them.
 */
inline void place_field(reader &handle, const soa::table &source, const soa::field &field, const std::byte *at) noexcept {
    using soa::field_kind;
    handle.reached = nullptr;
    handle.payload = at;
    handle.table = { .container = handle.table.container, .field = static_cast<std::uint32_t>(&field - source.fields().begin()),
        .record = handle.table.record };
    switch (field.kind) {
    case field_kind::scalar: handle.element = field.type; break;
    case field_kind::boolean: handle.element = to_marker(*at); break;
    case field_kind::null: handle.element = marker::null; break;
    case field_kind::text:
        handle.element = field.type;
        handle.table.extent = static_cast<std::uint32_t>(soa::table::fixed_text(field, at).size());
        break;
    case field_kind::dictionary: {
        std::uint64_t index = 0;
        handle.element = marker::invalid;
        if (!direct::load_integer(field.type, at, field.width, index)) break;
        if (const auto entry = source.dictionary_entry(field, index)) {
            handle.element = marker::string;
            handle.payload = reinterpret_cast<const std::byte *>(entry->data());
            handle.table.extent = static_cast<std::uint32_t>(entry->size());
        }
        break;
    }
    case field_kind::offsets: {
        std::uint64_t index = 0;
        handle.element = marker::invalid;
        if (!direct::load_integer(field.type, at, field.width, index)) break;
        if (const auto entry = source.offset_entry(field, index)) {
            handle.element = marker::string;
            handle.payload = reinterpret_cast<const std::byte *>(entry->data());
            handle.table.extent = static_cast<std::uint32_t>(entry->size());
        }
        break;
    }
    case field_kind::record: handle.element = marker::object_begin; break;
    case field_kind::array: handle.element = marker::array_begin; break;
    }
}

/**
 * Iterator over the elements of an array.
 *
 * All the traversal state lives here - a cursor, the count still owed, the strong type when
 * there is one, and the element it stands on. It hands out a reference to that element, so
 * that a walk of it leaves its note where the step to the next one reads it; and tells the
 * array being walked, through the reference it was given to it, when the walk is complete. An
 * input iterator, since what it owns cannot be copied.
 */
class array_iterator {
public:
    using value_type = reader;
    using reference = const reader &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    const std::byte *cursor = nullptr;
    marker element = marker::invalid;
    std::uint64_t remaining = 0;
    bool counted = false;
    bool exhausted = true;
    reader current;

    // Over a table: its records, one handle each; or, from a handle on a fixed run of elements
    // in a record, those elements. The placed table is owned here, since a handle carries only
    // its place and the schema has to be read to find anything.
    std::unique_ptr<soa::table> layout;
    std::uint64_t record = 0;      ///< the record handed out next
    std::uint32_t next_field = 0;  ///< the element handed out next, walking a run
    std::uint32_t last_field = 0;

    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->container->source.data() + this->container->source.size();
    }

    void normalise_in_table() noexcept {
        if (this->container->table.in_table()) {
            // Elements of a run inside a record, each at its offset from the run's bytes.
            this->exhausted = this->next_field >= this->last_field;
            if (this->exhausted) return;
            const auto &element = this->layout->fields().fields[this->next_field];
            this->current.table = this->container->table;
            place_field(this->current, *this->layout, element, this->container->payload + element.offset);
            return;
        }
        this->exhausted = this->record >= this->layout->size();
        if (this->exhausted) {
            this->container->note_end(this->layout->end());
            return;
        }
        place_record(this->current, this->container->payload - 1, this->record);
    }

    /** Settles whether the cursor stands on an element, and if so points `current` at it. */
    void normalise() noexcept {
        if (this->layout != nullptr) {
            this->normalise_in_table();
            return;
        }
        if (this->cursor == nullptr) {
            this->exhausted = true;
            return;
        }
        if (this->element == marker::invalid) {
            while (this->cursor < this->limit() && to_marker(*this->cursor) == marker::noop)
                ++this->cursor;
        }
        if (this->counted) {
            this->exhausted = this->remaining == 0 || this->cursor >= this->limit();
        } else {
            this->exhausted = this->cursor >= this->limit() || to_marker(*this->cursor) == marker::array_end;
        }
        if (this->exhausted) {
            this->publish();
            return;
        }
        if (this->element != marker::invalid) {
            move_to(this->current, this->element, this->cursor);
            return;
        }
        const auto kind = to_marker(*this->cursor);
        if (!is_value(kind)) {
            this->exhausted = true;
            return;
        }
        move_to(this->current, kind, this->cursor + 1);
    }

    /** Says where the array ended, once it has been walked all the way. */
    void publish() const noexcept {
        if (this->counted) {
            if (this->remaining == 0) this->container->note_end(this->cursor);
        } else if (this->cursor != nullptr && this->cursor < this->limit()
                && to_marker(*this->cursor) == marker::array_end) {
            this->container->note_end(this->cursor + 1);
        }
    }

public:
    array_iterator() = default;

    array_iterator(const reader &container, const detail::header &info) noexcept
    : container(&container)
    , cursor(info.body)
    , element(info.element)
    , remaining(info.count)
    , counted(!info.unbounded)
    , current(marker::invalid, container.source, nullptr) {
        if (container.table.in_table() && container.element == marker::array_begin) {
            // A fixed run of elements in a record.
            this->layout = std::make_unique<soa::table>();
            if (this->layout->place_at(container.source, container.table.container)) {
                const auto &run = this->layout->fields().fields[container.table.field];
                this->next_field = run.children;
                this->last_field = run.next;
            } else {
                this->layout.reset();
            }
            this->exhausted = this->layout == nullptr;
            if (!this->exhausted) this->normalise();
            return;
        }
        if (info.structured()) {
            this->layout = std::make_unique<soa::table>();
            if (!this->layout->place(container.source, info, container.element == marker::object_begin)) this->layout.reset();
            this->exhausted = this->layout == nullptr;
            if (!this->exhausted) this->normalise();
            return;
        }
        this->exhausted = info.body == nullptr;
        this->normalise();
    }

    [[nodiscard]] const reader &operator*() const noexcept { return this->current; }

    array_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        if (this->layout != nullptr) {
            if (this->container->table.in_table()) this->next_field = this->layout->fields().fields[this->next_field].next;
            else ++this->record;
            this->normalise();
            return *this;
        }

        if (this->element != marker::invalid) {
            const auto width = payload_width(this->element);
            if (static_cast<std::uint64_t>(this->limit() - this->cursor) < width) {
                this->exhausted = true;
                return *this;
            }
            this->cursor += width;
        } else if (this->current.reached != nullptr) {
            // Something already walked this element to its end, so step to where it finished
            // rather than scanning the same bytes a second time to find the same place.
            this->cursor = this->current.reached;
        } else {
            detail::cursor scanner { this->container->source, this->current.payload };
            detail::skip_value(scanner, this->current.element, 1);
            if (!scanner.ok()) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = scanner.position;
        }

        if (this->counted && this->remaining > 0) --this->remaining;
        this->normalise();
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/** Iterator over the key/value pairs of an object; the same arrangement as array_iterator. */
class member_iterator {
public:
    using value_type = key_value;
    using reference = const key_value &;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

private:
    const reader *container = nullptr;
    const std::byte *cursor = nullptr;
    marker element = marker::invalid;
    std::uint64_t remaining = 0;
    bool counted = false;
    bool exhausted = true;
    key_value current;

    // Over a record of a table: its fields in schema order, the whole record's from where the
    // table places each, a nested record's at their offsets from its bytes.
    std::unique_ptr<soa::table> layout;
    std::uint32_t next_field = 0;
    std::uint32_t last_field = 0;

    [[nodiscard]] constexpr const std::byte *limit() const noexcept {
        return this->container->source.data() + this->container->source.size();
    }

    void normalise_in_table() noexcept {
        this->exhausted = this->next_field >= this->last_field;
        if (this->exhausted) return;
        const auto &field = this->layout->fields().fields[this->next_field];
        this->current.key = field.name;
        this->current.value.table = this->container->table;
        const std::byte *at = this->container->table.is_record()
                ? this->layout->at(field, this->container->table.record)
                : this->container->payload + field.offset;
        place_field(this->current.value, *this->layout, field, at);
    }

    /**
     * Says where this object ended, once it has been walked all the way.
     *
     * Only on exhaustion: an unbounded object ends at a terminator the cursor stops on rather
     * than steps over, so the value ends one byte further on than the walk reached.
     */
    void publish() const noexcept {
        if (this->counted) {
            if (this->remaining == 0) this->container->note_end(this->cursor);
        } else if (this->cursor != nullptr && this->cursor < this->limit()
                && to_marker(*this->cursor) == marker::object_end) {
            this->container->note_end(this->cursor + 1);
        }
    }

    /** Parses the entry at the cursor into `current`; false if it is malformed. */
    [[nodiscard]] bool parse_here() noexcept {
        detail::cursor scanner { this->container->source, this->cursor };
        const auto key = detail::read_key(scanner);
        if (!scanner.ok()) return false;

        if (this->element != marker::invalid) {
            this->current.key = key;
            move_to(this->current.value, this->element, scanner.position);
            return true;
        }
        if (!scanner.need(1)) return false;
        const auto kind = to_marker(scanner.peek());
        if (!is_value(kind)) return false;
        this->current.key = key;
        move_to(this->current.value, kind, scanner.position + 1);
        return true;
    }

    void normalise() noexcept {
        if (this->layout != nullptr) {
            this->normalise_in_table();
            return;
        }
        if (this->cursor == nullptr) {
            this->exhausted = true;
            return;
        }
        // Unlike a typed array, an object skips noops whether or not it has a strong type.
        while (this->cursor < this->limit() && to_marker(*this->cursor) == marker::noop)
            ++this->cursor;

        if (this->counted) {
            this->exhausted = this->remaining == 0 || this->cursor >= this->limit();
        } else {
            this->exhausted = this->cursor >= this->limit() || to_marker(*this->cursor) == marker::object_end;
        }
        // The key and the value marker are read here rather than in operator*, so that
        // advancing can resume from the value instead of parsing the key a second time.
        if (this->exhausted) this->publish();
        else if (!this->parse_here()) this->exhausted = true;
    }

public:
    member_iterator() = default;

    member_iterator(const reader &container, const detail::header &info) noexcept
    : container(&container)
    , cursor(info.body)
    , element(info.element)
    , remaining(info.count)
    , counted(!info.unbounded)
    , current { {}, reader { marker::invalid, container.source, nullptr } } {
        if (container.table.in_table() && container.element == marker::object_begin) {
            this->layout = std::make_unique<soa::table>();
            if (this->layout->place_at(container.source, container.table.container)) {
                if (container.table.is_record()) {
                    this->next_field = 0;
                    this->last_field = this->layout->fields().count();
                } else {
                    const auto &record = this->layout->fields().fields[container.table.field];
                    this->next_field = record.children;
                    this->last_field = record.next;
                }
            } else {
                this->layout.reset();
            }
            this->exhausted = this->layout == nullptr;
            if (!this->exhausted) this->normalise();
            return;
        }
        this->exhausted = info.body == nullptr;
        this->normalise();
    }

    [[nodiscard]] const key_value &operator*() const noexcept { return this->current; }
    [[nodiscard]] const key_value *operator->() const noexcept { return &this->current; }

    member_iterator &operator++() noexcept {
        if (this->exhausted) return *this;

        if (this->layout != nullptr) {
            this->next_field = this->layout->fields().fields[this->next_field].next;
            this->normalise();
            return *this;
        }

        // The key and the value's marker were consumed by normalise(), so resume at the value.
        if (this->element != marker::invalid) {
            const auto width = payload_width(this->element);
            if (static_cast<std::uint64_t>(this->limit() - this->current.value.payload) < width) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = this->current.value.payload + width;
        } else if (this->current.value.reached != nullptr) {
            this->cursor = this->current.value.reached;
        } else {
            detail::cursor scanner { this->container->source, this->current.value.payload };
            detail::skip_value(scanner, this->current.value.element, 1);
            if (!scanner.ok()) {
                this->exhausted = true;
                return *this;
            }
            this->cursor = scanner.position;
        }

        if (this->counted && this->remaining > 0) --this->remaining;
        this->normalise();
        return *this;
    }

    void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return this->exhausted; }
};

/**
 * The elements of an array, or the members of an object, as a range.
 *
 * Made from a reader that will outlive the loop, it refers to it, and the walk leaves its note
 * there for whatever steps over that reader next. Made from a temporary - `document["a"].array()`
 * - it keeps the reader itself, and the note has nowhere further to go.
 */
template<typename Iterator>
class element_range {
    const reader *container = nullptr;
    std::optional<reader> owned;

    [[nodiscard]] const reader &of() const noexcept { return this->owned ? *this->owned : *this->container; }

public:
    explicit element_range(const reader &container) noexcept : container(&container) {}
    explicit element_range(reader &&container) noexcept : owned(std::move(container)) {}

    [[nodiscard]] Iterator begin() const noexcept { return Iterator { this->of(), this->of().container_header() }; }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};

inline array_range reader::array() const & noexcept { return array_range { *this }; }
inline array_range reader::array() && noexcept { return array_range { std::move(*this) }; }
inline member_range reader::items() const & noexcept { return member_range { *this }; }
inline member_range reader::items() && noexcept { return member_range { std::move(*this) }; }

inline std::optional<std::size_t> reader::size_hint() const noexcept {
    const auto info = this->container_header();
    if (info.body == nullptr || info.unbounded) return std::nullopt;
    return static_cast<std::size_t>(info.count);
}

inline std::size_t reader::size() const noexcept {
    if (this->table.in_table()) {
        std::size_t total = 0;
        if (this->element == marker::object_begin) {
            for ([[maybe_unused]] const auto &entry : this->items()) ++total;
        } else if (this->element == marker::array_begin) {
            for ([[maybe_unused]] const auto &entry : this->array()) ++total;
        }
        return total;
    }
    const auto info = this->container_header();
    if (info.body == nullptr) return 0;
    if (!info.unbounded) return static_cast<std::size_t>(info.count);

    std::size_t total = 0;
    if (this->element == marker::object_begin) {
        for ([[maybe_unused]] const auto &entry : this->items())
            ++total;
    } else {
        for ([[maybe_unused]] const auto &entry : this->array())
            ++total;
    }
    return total;
}

inline reader reader::operator[](std::size_t index) const noexcept {
    // A column-major table opens with `{` and is indexed by record like any other.
    const bool table_here = this->table.in_table() || this->container_header().structured();
    if (this->element != marker::array_begin && !(this->element == marker::object_begin && table_here)) return {};
    if (table_here) {
        std::size_t position = 0;
        for (const auto &value : this->array()) {
            if (position++ == index) return value.clone();
        }
        return {};
    }

    const auto info = this->container_header();
    if (info.body == nullptr) return {};

    // A typed array has a fixed stride, so indexing is arithmetic rather than a walk.
    if (info.typed() && !info.unbounded) {
        if (index >= info.count) return {};
        const auto width = payload_width(info.element);
        // Check before forming the pointer, so an out-of-range index never computes one.
        if (static_cast<std::uint64_t>(this->limit() - info.body) < (index + 1) * width) return {};
        return reader { info.element, this->source, info.body + index * width };
    }

    std::size_t position = 0;
    for (const auto &value : this->array()) {
        if (position++ == index) return value.clone();
    }
    return {};
}

inline reader reader::operator[](std::string_view key) const noexcept {
    if (this->element != marker::object_begin) return {};
    for (const auto &entry : this->items()) {
        if (entry.key == key) return entry.value.clone();
    }
    return {};
}

/**
 * Walks the whole document once, checking that every value is well formed, in bounds, and
 * that the root consumes the entire buffer. Traversal afterwards cannot fail.
 */
[[nodiscard]] inline std::expected<void, error> validate(std::span<const std::byte> buffer) noexcept {
    detail::cursor source { buffer.data(), buffer.data(), buffer.data() + buffer.size() };

    if (!source.need(1)) return std::unexpected { source.to_error() };

    const auto root = to_marker(source.peek());
    if (root == marker::extension) return std::unexpected { error { errc::extension_unsupported, 0 } };
    if (!is_value(root)) return std::unexpected { error { errc::unexpected_marker, 0 } };
    source.advance(1);

    detail::skip_value(source, root, 0);
    if (!source.ok()) return std::unexpected { source.to_error() };

    if (source.position != source.limit) {
        return std::unexpected { error { errc::trailing_data, source.offset_of(source.position) } };
    }
    return {};
}

/**
 * Fills a contiguous container of numbers from a typed array of the same numbers, in one copy.
 *
 * Found by ordinary lookup from the general container read, which otherwise reads an element at
 * a time through the iterator. Answers nothing for any other shape - an untyped array, a typed
 * one of another width - and leaves those to it.
 */
template<std::ranges::contiguous_range C, typename T = std::ranges::range_value_t<C>>
    requires (strong_type_for<T>() != marker::invalid) && std::is_arithmetic_v<T> && requires(C &out) { out.resize(std::size_t {}); }
std::optional<bool> read_sequence(const reader &source, C &out) {
    const auto values = source.template as<nonstd::unaligned_little_span<const T>>();
    if (!values) return std::nullopt;

    out.resize(values->size());
    std::ranges::copy(*values, std::ranges::begin(out));
    return true;
}

} // namespace serpent::bjdata
