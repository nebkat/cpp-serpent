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

#include <serpent/config.hpp>
#include <serpent/constant_text.hpp>
#include <serpent/json/direct.hpp>
#include <serpent/json/reader.hpp>
#include <serpent/json/writer.hpp>
#include <serpent/member_runs.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>

#include <concepts>
#include <cstdint>
#include <cstring>
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

    std::uint64_t seen = 0;   ///< one bit for each member read
    bool converted = true;    ///< whether every member read so far converted
    bool any_entry = false;   ///< whether an entry has been passed, so that a comma precedes the next

public:
    /** `scan` stands just past the object's opening brace, and is left just past its closing one. */
    object_filler(T &value, scanner::cursor &scan, std::string_view document) noexcept
    : value(value)
    , scan(scan)
    , document(document) {}

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
            const reader held { this->document, this->scan.position };
            if constexpr (tag.has_value()) {
                // A tagged variant is read through the tag that names its alternatives; reading it
                // plainly would go back to trying each alternative in turn.
                using declared = [:std::meta::type_of(Member):];
                auto wrapper = make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field);
                if (!read_into(held, wrapper)) this->converted = false;
            } else {
                if (!read_into(held, field)) this->converted = false;
            }
            step_over_value(this->scan, held);
        }
    }
};

/** Reads the object whose opening brace the cursor has just passed, leaving it past the closing one. */
template<typename T>
[[nodiscard]] bool read_object(T &value, scanner::cursor &scan, std::string_view document) {
    object_filler<T> filler { value, scan, document };
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
    const bool read = read_object(value, scan, source.document());
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
            if (!read_object(out.emplace_back(), scan, source.document())) return false;
            scanner::skip_whitespace(scan);
        } while (scanner::accept(scan, ','));

        if (!scanner::accept(scan, ']')) return false;
    }

    source.note_end(scan.position);
    return true;
}

#if SERPENT_BOUNDED_OBJECT_WRITE

/**
 * The most text one member can take - comma, key and value - or zero if it has no limit. For a
 * string it is everything but the string itself, which is `text`: added when the run is written.
 */
template<typename T, std::meta::info Member>
struct widest_member {
    using field = std::remove_cvref_t<typename [:std::meta::type_of(Member):]>;
    static constexpr bool text = std::same_as<field, std::string> || std::same_as<field, std::string_view>;

    static constexpr std::size_t value = [] () -> std::size_t {
        if (serpent::detail::has_annotation<skip>(Member)) return 0;
        if (serpent::detail::annotation_of<tagged>(Member).has_value()) return 0;

        constexpr std::string_view name = serpent::detail::field_key<T, Member>();
        if (!scanner::is_plain_text(name)) return 0;
        constexpr std::size_t comma_quotes_and_colon = 4;
        if (text) return comma_quotes_and_colon + name.size() + 2;
        if (widest_text<field> == 0) return 0;
        return comma_quotes_and_colon + name.size() + widest_text<field>;
    }();
};

template<typename T>
class object_writer {
    using runs = serpent::detail::member_runs<T, widest_member>;

    writer &out;
    const T &value;

    /** Whether every member the type writes is in one run, so the whole object has a longest length. */
    static constexpr bool whole_object_bounded = runs::members.size() > 0 && runs::begins_run(0)
            && runs::run_end(0) == runs::members.size();

public:
    object_writer(writer &out, const T &value) noexcept : out(out), value(value) {}

    /** The object with its braces as one piece, where it can be; false where it was not written. */
    [[nodiscard]] bool write_whole_object() {
        if constexpr (!whole_object_bounded) {
            return false;
        } else {
            constexpr std::size_t all = runs::members.size();
            if (!this->text_is_plain<0, all>()) return false;
            return this->out.compose_object(2 + runs::widest_run(0, all) + this->text_bytes<0, all>(), [this](char *const to) {
                to[0] = '{';
                char *const end = this->write_run_into<0, all>(to + 1, false);
                *end = '}';
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
        if constexpr (constexpr auto tag = serpent::detail::annotation_of<tagged>(Member); tag.has_value()) {
            using declared = [:std::meta::type_of(Member):];
            static_assert(serpent::detail::variant_like<declared>, "serpent::tagged belongs on a variant field");
            this->out.value(make_tagged<serpent::detail::resolved_tag<declared, *tag>()>(field));
        } else {
            this->out.value(field);
        }
    }

    /**
     * The members from `First` up to `Last` as one piece of text, if room for it can be had.
     *
     * Only the first member written into an object goes without a comma, and whether anything
     * has been written yet is the one thing here not known until it runs.
     */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] bool write_run() {
        if (!this->text_is_plain<First, Last>()) return false;
        return this->out.compose_members(runs::widest_run(First, Last) + this->text_bytes<First, Last>(), [this](char *const to) {
            return static_cast<std::size_t>(this->write_run_into<First, Last>(to, this->out.has_members()) - to);
        });
    }

    /** The length of the strings among the members from `First` up to `Last`, which the bound cannot know. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] std::size_t text_bytes() const noexcept {
        std::size_t total = 0;
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last) && runs::text[runs::position_of(member)])
                total += std::string_view { this->value.[:member:] }.size();
        }
        return total;
    }

    /** Whether every string among those members can stand between quotes as it is. One that cannot is
     *  written the usual way, escapes and all, and takes its run with it. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] bool text_is_plain() const noexcept {
        bool plain = true;
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last) && runs::text[runs::position_of(member)]) {
                const std::string_view text { this->value.[:member:] };
                plain = plain && scanner::end_of_plain_text(text.data(), text.data() + text.size()) == text.data() + text.size();
            }
        }
        return plain;
    }

    /** The members from `First` up to `Last` stored at `to`, the first with a comma before it if `after_another`. */
    template<std::size_t First, std::size_t Last>
    [[nodiscard]] char *write_run_into(char *to, bool after_another) {
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last)) {
                static constexpr std::string_view name = serpent::detail::field_key<T, member>();
                to = after_another ? copy_constant(to, written_next_key<name>) : copy_constant(to, written_key<name>);
                if constexpr (runs::text[runs::position_of(member)]) {
                    const std::string_view text { this->value.[:member:] };
                    *to++ = '"';
                    std::memcpy(to, text.data(), text.size());
                    to += text.size();
                    *to++ = '"';
                } else {
                    to = write_text(to, this->value.[:member:]);
                }
                after_another = true;
            }
        }
        return to;
    }

    /** The same members when that room could not be had: each the usual way. */
    template<std::size_t First, std::size_t Last>
    void write_run_one_at_a_time() {
        template for (constexpr auto member : runs::members) {
            if constexpr (runs::within(member, First, Last)) this->write_member<member>();
        }
    }

    template<std::size_t Width>
    [[nodiscard]] static char *copy_constant(char *to, const std::array<char, Width> &text) noexcept {
        std::memcpy(to, text.data(), Width);
        return to + Width;
    }
};

/**
 * Writes a described type as a JSON object.
 *
 * Found by ordinary lookup from serializer<T>::write, as read_reflected is, so that a writer with
 * no such function - the binary one - does not take this path.
 */
template<reflected_type T>
    requires (!serpent::detail::is_discriminated<T>())
void write_reflected(writer &out, const T &value) {
    static_assert(serpent::detail::keys_are_distinct<T>(), serpent::detail::duplicate_key_message<T>());
    static_assert(serpent::detail::annotations_make_sense<T>(), serpent::detail::annotation_complaint<T>());
    object_writer<T> composer { out, value };
    if (composer.write_whole_object()) return;
    const auto scope = out.object();
    composer.write_members();
}

#endif // SERPENT_BOUNDED_OBJECT_WRITE

#endif

} // namespace serpent::json
