#pragma once

// Reading a described type from JSON.
//
// The reader handle and its iterators are written for a caller who knows nothing about a document
// in advance: they classify every byte to find out what stands there. A type whose members the
// compiler can enumerate knows a great deal in advance - what each key is, what each value should
// be, and exactly how this library would have written the pair - so the reader generated for it
// expects those bytes rather than classifying them.
//
// It reads an object in two steps. First the members that stand exactly where and how this library
// would have written them, which for a document this library wrote is all of them: one comparison
// and one conversion apiece. Then whatever is left, written any other way - space around the
// punctuation, another order, keys this type does not name - which is scanned entry by entry and
// matched by name. Either way the document is read once, and a document in an unexpected order
// costs a little more rather than the square of its size.
//
// serializer<T>::read finds read_reflected by ordinary lookup on the source, so the visitor stays
// the way in for a hand-written conversion, whose body is the user's.

#include <serpent/constant_text.hpp>
#include <serpent/json/direct.hpp>
#include <serpent/json/reader.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace serpent::json {

#if SERPENT_HAS_REFLECTION

/** `"name":` - a member's key exactly as this library writes it. */
template<const std::string_view &Name>
inline constexpr auto written_key = serpent::detail::joined<Name.size() + 3>({ "\"", Name, "\":" });

/** `,"name":` - the same, behind the comma that separates it from the member before. */
template<const std::string_view &Name>
inline constexpr auto written_next_key = serpent::detail::joined<Name.size() + 4>({ ",\"", Name, "\":" });

/**
 * Fills one object of a described type from the text of a JSON object.
 *
 * Holds what reading an object needs to carry from one member to the next: where the cursor is,
 * which members have been seen, and whether everything read so far converted.
 */
template<typename T>
class object_filler {
    static constexpr auto members = std::define_static_array(serpent::detail::members_including_bases<T>());
    static_assert(members.size() <= 64, "a type with more than 64 members needs a wider seen mask");

    /** One bit for each member the document has to carry. */
    static constexpr std::uint64_t required = [] {
        std::uint64_t mask = 0;
        std::size_t position = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)
                    && serpent::detail::member_is_required<T, member>()) {
                mask |= std::uint64_t { 1 } << position;
            }
            ++position;
        }
        return mask;
    }();

    T &value;
    scanner::cursor &scan; ///< borrowed, so that one cursor can be carried from object to object
    std::string_view document;
    walk_memo *notes;

    std::uint64_t seen = 0;   ///< one bit for each member read
    bool converted = true;    ///< whether every member read so far converted
    bool any_entry = false;   ///< whether an entry has been passed, so that a comma precedes the next

public:
    /** `scan` stands just past the object's opening brace, and is left just past its closing one. */
    object_filler(T &value, scanner::cursor &scan, std::string_view document, walk_memo *notes) noexcept
    : value(value)
    , scan(scan)
    , document(document)
    , notes(notes) {}

    /**
     * Reads the members that stand exactly as this library would have written them.
     *
     * Each member, in declaration order, is offered the text at the cursor. A member that is not
     * there - left out, or written some other way - simply does not match, and the next is
     * offered the same text; nothing is consumed that is not recognised. So a document this
     * library wrote is read entirely here, and any other loses nothing by having been tried.
     */
    void read_members_as_written() {
        std::size_t position = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                const bool recognised = this->any_entry ? scanner::accept(this->scan, written_next_key<name>)
                                                        : scanner::accept(this->scan, written_key<name>);
                if (recognised) {
                    this->any_entry = true;
                    this->read_value<member>(position);
                }
            }
            ++position;
        }
    }

    /**
     * Reads every entry from the cursor to the closing brace, however it is written.
     *
     * After read_members_as_written() has taken a document this library wrote, the closing brace
     * is all that is left. For any other document this is where the work is done: each key is
     * scanned, matched by name against the members, and its value read or stepped over.
     */
    void read_remaining_entries() {
        std::string decoded_key;
        while (this->at_next_entry()) {
            const auto key = scanner::scan_string(this->scan);
            if (!this->scan.ok()) return;

            // An escaped key is rare and cannot be compared in place, so it is decoded only then.
            std::string_view name = key.contents;
            if (key.escaped) [[unlikely]] {
                decoded_key.clear();
                scanner::decode_string(key, [&](char one) { decoded_key.push_back(one); });
                name = decoded_key;
            }

            scanner::skip_whitespace(this->scan);
            if (!scanner::accept(this->scan, ':')) {
                this->fail_here();
                return;
            }
            if (!this->read_member_named(name)) {
                scanner::skip_whitespace(this->scan);
                scanner::skip_value(this->scan, 1);
            }
        }
    }

    /**
     * Whether the object was read: well formed to its closing brace, every member converted,
     * and every member the type insists on present.
     */
    [[nodiscard]] bool succeeded() const noexcept {
        return this->scan.ok() && this->converted && (this->seen & required) == required;
    }

private:
    /**
     * Moves to the key of the next entry. False at the closing brace, which is stepped over, and
     * on anything that is neither - which fails the cursor.
     */
    [[nodiscard]] bool at_next_entry() noexcept {
        scanner::skip_whitespace(this->scan);
        if (scanner::accept(this->scan, '}')) return false;

        if (this->any_entry) {
            if (!scanner::accept(this->scan, ',')) {
                this->fail_here();
                return false;
            }
            scanner::skip_whitespace(this->scan);
        }
        this->any_entry = true;

        if (this->scan.available(1) && this->scan.peek() == '"') return true;
        this->fail_here();
        return false;
    }

    /** Fails the cursor for what stands at it: the end of the text, or a character that cannot be there. */
    void fail_here() noexcept {
        if (this->scan.need(1)) this->scan.fail(errc::unexpected_character);
    }

    /** Reads the value at the cursor into the member called `name`, if there is one. */
    [[nodiscard]] bool read_member_named(std::string_view name) {
        bool found = false;
        std::size_t position = 0;
        template for (constexpr auto member : members) {
            if constexpr (!serpent::detail::has_annotation<skip>(member)) {
                // Compared against a name whose length the compiler knows, so nearly every member
                // is rejected by its length alone and the rest by a comparison of fixed width.
                static constexpr std::string_view candidate = serpent::detail::field_key<T, member>();
                if (!found && name == candidate) {
                    found = true;
                    this->read_value<member>(position);
                }
            }
            ++position;
        }
        return found;
    }

    /** Reads the value at the cursor into one member, leaving the cursor just past it. */
    template<std::meta::info Member>
    void read_value(std::size_t position) {
        this->seen |= std::uint64_t { 1 } << position;
        scanner::skip_whitespace(this->scan);

        auto &field = this->value.[:Member:];
        using field_type = std::remove_cvref_t<decltype(field)>;
        constexpr auto tag = serpent::detail::annotation_of<tagged>(Member);

        if constexpr (direct::readable<field_type> && !tag.has_value()) {
            // The kinds of member a schema is mostly made of, read where they stand.
            if (direct::read(this->scan, field)) return;
            this->converted = false;
            scanner::skip_value(this->scan, 1);
        } else {
            // Anything else is read through a handle, by whatever reads that type anywhere else -
            // and says, as it is read, how far into the text it got.
            const reader held { this->document, this->scan.position, this->notes };
            if constexpr (tag.has_value()) {
                // A tagged variant is read through the tag that names its alternatives; reading it
                // plainly would go back to trying each alternative in turn.
                using declared = [:std::meta::type_of(Member):];
                auto wrapper = make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field);
                if (!read_into(held, wrapper)) this->converted = false;
            } else {
                if (!read_into(held, field)) this->converted = false;
            }
            step_over_value(this->scan, this->document, this->notes);
        }
    }
};

/** Reads the object whose opening brace the cursor has just passed, leaving it past the closing one. */
template<typename T>
[[nodiscard]] bool read_object(T &value, scanner::cursor &scan, std::string_view document, walk_memo *notes) {
    object_filler<T> filler { value, scan, document, notes };
    filler.read_members_as_written();
    filler.read_remaining_entries();
    return filler.succeeded();
}

/**
 * Fills a described type from a JSON object, reading the document once.
 *
 * Returns false for anything that is not an object, for a member that will not convert, and for a
 * member the type insists on that the document did not carry. A key the type does not name is
 * ignored, as it is everywhere else.
 */
template<typename T>
    requires reflected_type<std::remove_cvref_t<T>>
bool read_reflected(const reader &source, T &value) {
    if (!source.is_object()) return false;

    scanner::cursor scan { source.document(), source.data() + 1 };
    const bool read = read_object(value, scan, source.document(), source.notes());
    // Whoever is stepping through the container this object sits in can step to here.
    if (scan.ok()) source.note_end(scan.position);
    return read;
}

/**
 * Fills a container of described objects from a JSON array, reading the document once.
 *
 * Without this each element is reached through the array iterator: a handle is made for it, the
 * handle is asked what it holds, and the iterator then finds its way past the element to the next.
 * One cursor carried from each object to the one after it needs none of that.
 *
 * Returns nothing for a value that is not an array, which leaves the general path to refuse it.
 */
template<typename C, typename T = std::remove_cvref_t<typename C::value_type>>
    requires reflected_type<T> && requires(C &out) {
        out.clear();
        { out.emplace_back() } -> std::same_as<T &>;
    }
std::optional<bool> read_sequence(const reader &source, C &out) {
    if (!source.is_array()) return std::nullopt;

    scanner::cursor scan { source.document(), source.data() + 1 };
    out.clear();

    scanner::skip_whitespace(scan);
    if (!scanner::accept(scan, ']')) {
        do {
            scanner::skip_whitespace(scan);
            if (!scanner::accept(scan, '{')) return false;
            if (!read_object(out.emplace_back(), scan, source.document(), source.notes())) return false;
            scanner::skip_whitespace(scan);
        } while (scanner::accept(scan, ','));

        if (!scanner::accept(scan, ']')) return false;
    }

    source.note_end(scan.position);
    return true;
}

#endif

} // namespace serpent::json
