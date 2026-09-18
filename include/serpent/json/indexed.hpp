#pragma once

// Reading a document whose structure has been recorded once, for a document walked more than
// once or walked deeply.
//
// Its own header, included by nothing else, and additive: it does not change how a reader works,
// it wraps one. A reader finds its next sibling by scanning the current value to its end, so a
// byte is scanned once for every level of nesting above it - invisible on a small document, and
// the whole cost on a deep one. Measured against a scalar build of a vectorised parser, that one
// decision is about six of the seven times it is faster; the vector instructions are the rest.
//
// What this does not change is what a value *is*. Every question about content is handed to an
// ordinary reader, so the two cannot form separate opinions about how a number is spelled, which
// is how this library's bugs have historically happened. Only navigation is different, and
// navigation is all an index knows about.

#include <serpent/json/reader.hpp>

#include <charconv>
#include <cstring>
#include <cstdint>
#include <string_view>
#include <vector>

namespace serpent::json {

class indexed_reader;

/**
 * @brief Where every value in a document is, and which values are inside which.
 *
 * Recorded in one pass and then immutable. The layout is pre-order - a value is followed by its
 * whole subtree - so a child is the next entry and a sibling is wherever this value's subtree
 * ends. Both are a load rather than a scan, which is the point.
 *
 * It costs one entry per value, so it is for a host holding a file rather than a device holding
 * a message: a megabyte of JSON is comfortably more index than document.
 */
class structural_index {
public:
    /**
     * Twelve bytes: where the value is, where its subtree ends, and where its key is.
     *
     * The key's length is not kept. A stored key runs to the next quote, so comparing one
     * against a name the caller already has the length of is that many bytes and then a check
     * that a quote follows - which is the same work a length would have saved and no storage.
     * The high bit of the key offset says the key carries an escape and cannot be compared
     * where it lies, which caps a document at two gigabytes.
     *
     * How many children a container has is not kept either: counting them is a walk along the
     * siblings, each step a load, and only size() and size_hint() ever ask.
     */
    struct node {
        std::uint32_t first = 0; ///< offset of the value's first character
        std::uint32_t end = 0;   ///< one past this value's subtree, in entries
        std::uint32_t key = 0;   ///< offset of its key, high bit set when the key has an escape

        static constexpr std::uint32_t escaped_bit = std::uint32_t { 1 } << 31;

        [[nodiscard]] constexpr bool has_key() const noexcept { return this->key != 0; }
        [[nodiscard]] constexpr bool key_escaped() const noexcept { return (this->key & escaped_bit) != 0; }
        [[nodiscard]] constexpr std::uint32_t key_offset() const noexcept { return this->key & ~escaped_bit; }
    };

private:
    std::string_view source;
    std::vector<node> nodes;
    bool complete = false;

    [[nodiscard]] std::uint32_t offset_of(const char *position) const noexcept {
        return static_cast<std::uint32_t>(position - this->source.data());
    }

    /**
     * Records one value and everything inside it, leaving the cursor just past it.
     *
     * The one pass the whole design rests on: a container's children are recorded as they are
     * scanned, so no byte is visited twice and nothing is scanned to find where it ended.
     */
    void record(scanner::cursor &scan, scanner::string_span key, int depth) {
        if (depth > serpent::max_depth) {
            scan.fail(errc::depth_exceeded);
            return;
        }
        scanner::skip_whitespace(scan);
        if (!scan.available(1)) {
            scan.fail(errc::unexpected_end);
            return;
        }

        const auto self = static_cast<std::uint32_t>(this->nodes.size());
        const std::uint32_t key_field = key.contents.empty()
                ? 0
                : this->offset_of(key.contents.data()) | (key.escaped ? node::escaped_bit : 0);
        this->nodes.push_back(node { this->offset_of(scan.position), 0, key_field });
        const char opening = scan.peek();

        if (opening == '[' || opening == '{') {
            const bool object = opening == '{';
            scan.advance(1);
            scanner::skip_whitespace(scan);
            const char closing = object ? '}' : ']';

            if (scan.available(1) && scan.peek() == closing) {
                scan.advance(1);
            } else {
                while (scan.ok()) {
                    scanner::string_span member_key;
                    if (object) {
                        scanner::skip_whitespace(scan);
                        if (!scan.available(1) || scan.peek() != '"') {
                            scan.fail(errc::unexpected_character);
                            break;
                        }
                        member_key = scanner::scan_string(scan);
                        scanner::skip_whitespace(scan);
                        if (!scan.available(1) || scan.peek() != ':') {
                            scan.fail(errc::unexpected_character);
                            break;
                        }
                        scan.advance(1);
                    }

                    this->record(scan, member_key, depth + 1);
                    if (!scan.ok()) break;

                    scanner::skip_whitespace(scan);
                    if (!scan.available(1)) {
                        scan.fail(errc::unexpected_end);
                        break;
                    }
                    if (scan.peek() == ',') {
                        scan.advance(1);
                        continue;
                    }
                    if (scan.peek() == closing) {
                        scan.advance(1);
                        break;
                    }
                    scan.fail(errc::unexpected_character);
                    break;
                }
            }
        } else {
            // A scalar has nothing inside it, so stepping over it is the whole of the work.
            scanner::skip_value(scan, depth);
        }

        this->nodes[self].end = static_cast<std::uint32_t>(this->nodes.size());
    }

public:
    structural_index() = default;

    /**
     * Records the structure of a document. The text must outlive the index.
     *
     * A document that does not parse yields an index that says so rather than a partial one;
     * ask ok() before using it, or validate() first and know already.
     */
    [[nodiscard]] static structural_index over(std::string_view text) {
        structural_index built;
        built.source = text;
        // Roughly one value per twenty bytes of a real document, which over-reserves for prose
        // and under-reserves for arrays of numbers. Either way it beats growing from nothing.
        built.nodes.reserve(text.size() / 20 + 8);

        scanner::cursor scan { text, text.data() };
        built.record(scan, {}, 0);
        scanner::skip_whitespace(scan);
        built.complete = scan.ok() && !scan.available(1);
        if (!built.complete) built.nodes.clear();
        // Immutable from here, so the reservation that was a guess becomes dead weight.
        built.nodes.shrink_to_fit();
        return built;
    }

    [[nodiscard]] bool ok() const noexcept { return this->complete; }
    [[nodiscard]] std::string_view buffer() const noexcept { return this->source; }
    [[nodiscard]] std::size_t size() const noexcept { return this->nodes.size(); }
    [[nodiscard]] const node &at(std::uint32_t position) const noexcept { return this->nodes[position]; }

    /** How many bytes it occupies, which is the question worth asking before building one. */
    [[nodiscard]] std::size_t footprint() const noexcept { return this->nodes.capacity() * sizeof(node); }

    [[nodiscard]] inline indexed_reader root() const noexcept;
};

/**
 * @brief A handle to one value of an indexed document.
 *
 * Answers what a reader answers, because for everything but navigation it hands the position to
 * a reader and asks. Only finding a sibling or a member is different.
 */
/**
 * The span of a recorded key.
 *
 * The length was not stored, so the closing quote gives it - one scan of a key, and only when a
 * caller asks for the key itself rather than to compare it, which the lookup below does without
 * building a span at all.
 */
[[nodiscard]] inline scanner::string_span span_of_key(
        const structural_index &index, const structural_index::node &entry) noexcept {
    if (!entry.has_key()) return {};
    const auto text = index.buffer();
    const char *const first = text.data() + entry.key_offset();
    scanner::cursor scan { text, first - 1 };   // the opening quote
    const auto span = scanner::scan_string(scan);
    return scanner::string_span { span.contents, entry.key_escaped() };
}

class indexed_reader {
    const structural_index *index = nullptr;
    std::uint32_t position = 0;
    bool valid = false;

    [[nodiscard]] reader at_position() const noexcept {
        if (!this->valid) return {};
        const auto &here = this->index->at(this->position);
        return reader { this->index->buffer(), this->index->buffer().data() + here.first };
    }

    /** The key of this value, if it is a member: found by its recorded start and its own quote. */
    [[nodiscard]] scanner::string_span key_span() const noexcept {
        return span_of_key(*this->index, this->index->at(this->position));
    }

public:
    constexpr indexed_reader() = default;
    constexpr indexed_reader(const structural_index &index, std::uint32_t position) noexcept
            : index(&index)
            , position(position)
            , valid(true) {}

    /** The plain reader for this value, for anything this handle does not offer. */
    [[nodiscard]] reader scanning() const noexcept { return this->at_position(); }

    // ---------------- what it is: the reader answers, every time ----------------

    [[nodiscard]] kind type() const noexcept { return this->at_position().type(); }
    [[nodiscard]] bool is_valid() const noexcept { return this->valid && this->at_position().is_valid(); }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }
    [[nodiscard]] bool is_null() const noexcept { return this->at_position().is_null(); }
    [[nodiscard]] bool is_boolean() const noexcept { return this->at_position().is_boolean(); }
    [[nodiscard]] bool is_integer() const noexcept { return this->at_position().is_integer(); }
    [[nodiscard]] bool is_real() const noexcept { return this->at_position().is_real(); }
    [[nodiscard]] bool is_number() const noexcept { return this->at_position().is_number(); }
    [[nodiscard]] bool is_string() const noexcept { return this->at_position().is_string(); }

    // Known from the entry, so these cost a load rather than a look at the text.
    [[nodiscard]] bool is_array() const noexcept {
        return this->valid && this->index->buffer()[this->index->at(this->position).first] == '[';
    }
    [[nodiscard]] bool is_object() const noexcept {
        return this->valid && this->index->buffer()[this->index->at(this->position).first] == '{';
    }

    /**
     * Converted straight from the text, with no grammar check.
     *
     * The one place this handle does more than delegate, and it is not a second opinion about
     * what a number is: an index only exists for a document that parsed, and building it scanned
     * every number against the same grammar. A scanning reader has to check because it cannot
     * know that; this one knows. So the digits are walked once, by the conversion, rather than
     * once to validate and again to convert.
     */
    template<std::floating_point T>
    [[nodiscard]] std::optional<T> read_real() const noexcept {
        if (!this->valid) return std::nullopt;
        const auto text = this->index->buffer();
        const char *const begin = text.data() + this->index->at(this->position).first;
        double value = 0;
        const auto parsed = std::from_chars(begin, text.data() + text.size(), value);
        if (parsed.ec != std::errc {}) return std::nullopt;
        return static_cast<T>(value);
    }

    template<std::integral T>
    [[nodiscard]] std::optional<T> read_integer() const noexcept {
        if (!this->valid) return std::nullopt;
        const auto text = this->index->buffer();
        const char *const begin = text.data() + this->index->at(this->position).first;
        const char *const limit = text.data() + text.size();

        const auto reject_real = [limit](const char *end) {
            // from_chars stops at the point or exponent that makes this a real, so what follows
            // is what says whether it was one.
            return end != limit && (*end == '.' || *end == 'e' || *end == 'E');
        };

        if (*begin == '-') {
            std::int64_t value = 0;
            const auto parsed = std::from_chars(begin, limit, value);
            if (parsed.ec != std::errc {} || reject_real(parsed.ptr) || !std::in_range<T>(value))
                return std::nullopt;
            return static_cast<T>(value);
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(begin, limit, value);
        if (parsed.ec != std::errc {} || reject_real(parsed.ptr) || !std::in_range<T>(value)) return std::nullopt;
        return static_cast<T>(value);
    }

    /** How many values are directly inside this one: a walk along the siblings, each step a load. */
    [[nodiscard]] std::size_t size() const noexcept {
        if (!this->valid) return 0;
        const auto &here = this->index->at(this->position);
        if (here.end <= this->position + 1) return 0;
        std::size_t count = 0;
        for (auto child = this->position + 1; child < here.end; child = this->index->at(child).end) ++count;
        return count;
    }

    [[nodiscard]] std::optional<std::size_t> size_hint() const noexcept {
        if (!this->valid) return std::nullopt;
        return this->size();
    }

    // ---------------- navigation: the only part an index knows about ----------------

    /**
     * @brief Iterates the values directly inside this one.
     *
     * The first is the next entry, and each subsequent one begins where the previous one's
     * subtree ended - so a step is a load, whatever is nested in between.
     */
    class iterator {
        const structural_index *index = nullptr;
        std::uint32_t position = 0;

    public:
        using value_type = indexed_reader;
        using reference = indexed_reader;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        constexpr iterator() = default;
        constexpr iterator(const structural_index *index, std::uint32_t position) noexcept
                : index(index)
                , position(position) {}

        [[nodiscard]] indexed_reader operator*() const noexcept {
            return indexed_reader { *this->index, this->position };
        }

        iterator &operator++() noexcept {
            this->position = this->index->at(this->position).end;
            return *this;
        }

        iterator operator++(int) noexcept {
            auto copy = *this;
            ++*this;
            return copy;
        }

        [[nodiscard]] bool operator==(const iterator &other) const noexcept { return this->position == other.position; }
    };

    class range {
        const structural_index *index = nullptr;
        std::uint32_t first = 0;
        std::uint32_t last = 0;

    public:
        constexpr range() = default;
        constexpr range(const structural_index *index, std::uint32_t first, std::uint32_t last) noexcept
                : index(index)
                , first(first)
                , last(last) {}

        [[nodiscard]] iterator begin() const noexcept { return iterator { this->index, this->first }; }
        [[nodiscard]] iterator end() const noexcept { return iterator { this->index, this->last }; }
    };

    [[nodiscard]] range array() const noexcept {
        if (!this->valid || !this->is_array()) return {};
        const auto &here = this->index->at(this->position);
        return range { this->index, this->position + 1, here.end };
    }

    struct key_value;
    class member_iterator;
    class member_range;

    [[nodiscard]] inline member_range items() const noexcept;

    /** The element at this position. Walks the entries, which for an array is one per element. */
    [[nodiscard]] indexed_reader operator[](std::size_t element) const noexcept {
        if (!this->valid || !this->is_array()) return {};
        const auto &here = this->index->at(this->position);
        auto found = this->position + 1;
        for (std::size_t step = 0; step < element; ++step) {
            if (found >= here.end) return {};
            found = this->index->at(found).end;
        }
        return found < here.end ? indexed_reader { *this->index, found } : indexed_reader {};
    }

    /**
     * The member under this key.
     *
     * Still a comparison per member, but against a key the index already located - no value is
     * scanned to get past it, which is what the walk used to cost. A hash per key would make it
     * a lookup, and is a separate decision with its own memory.
     */
    [[nodiscard]] indexed_reader operator[](std::string_view name) const noexcept {
        if (!this->valid || !this->is_object()) return {};
        const auto &here = this->index->at(this->position);
        const auto text = this->index->buffer();
        for (auto member = this->position + 1; member != here.end; member = this->index->at(member).end) {
            const auto &entry = this->index->at(member);
            if (!entry.has_key()) continue;
            const char *const key = text.data() + entry.key_offset();

            if (!entry.key_escaped()) [[likely]] {
                // The quote that ends the stored key is what a length would have told us.
                const auto room = static_cast<std::size_t>(text.data() + text.size() - key);
                if (room > name.size() && key[name.size()] == '"'
                        && std::memcmp(key, name.data(), name.size()) == 0) {
                    return indexed_reader { *this->index, member };
                }
            } else if (scanner::equals(span_of_key(*this->index, entry), name)) {
                return indexed_reader { *this->index, member };
            }
        }
        return {};
    }

    /**
     * Whatever this value holds, as one of your types.
     *
     * Read through *this* handle, not the plain one underneath: a container filled from the
     * document has to walk it by the index, or the reading gives up the index and is correct but
     * pointless.
     */
    template<typename T>
        requires (!std::same_as<T, std::string_view> && !std::same_as<T, std::span<const std::byte>>)
    [[nodiscard]] std::optional<T> as() const {
        if constexpr (std::same_as<T, bool>) {
            return this->at_position().template as<bool>();
        } else if constexpr (serpent::detail::string_like<T> && std::constructible_from<T, std::string>) {
            return this->at_position().template as<T>();
        } else if constexpr (std::floating_point<T>) {
            return this->template read_real<T>();
        } else if constexpr (std::integral<T>) {
            return this->template read_integer<T>();
        } else if constexpr (serpent::detail::structurally_readable<T>) {
            T item {};
            if (!read_into(*this, item)) return std::nullopt;
            return item;
        } else {
            T item {};
            if (!serializer<T>::read(*this, item)) return std::nullopt;
            return item;
        }
    }

    /** JSON text is neither a string_view nor bytes to be lent: a string has to be decoded, and there is no binary. */
    template<typename T>
        requires std::same_as<T, std::string_view> || std::same_as<T, std::span<const std::byte>>
    [[nodiscard]] std::optional<T> as() const = delete("a JSON string has to be decoded, so it cannot be borrowed "
                                                     "as a string_view; read it as a std::string");
};

/**
 * A key and its value, the shape every reader's items() yields.
 *
 * The key is the span the index recorded, undecoded until asked, exactly as the scanning reader
 * hands it over.
 */
struct indexed_reader::key_value {
    scanner::string_span key;
    indexed_reader value;

    [[nodiscard]] bool key_is(std::string_view other) const noexcept { return scanner::equals(this->key, other); }

    [[nodiscard]] std::string key_string() const {
        std::string decoded;
        decoded.reserve(scanner::decoded_length(this->key));
        scanner::decode_string(this->key, [&](char value) { decoded.push_back(value); });
        return decoded;
    }
};

class indexed_reader::member_iterator {
    const structural_index *index = nullptr;
    std::uint32_t position = 0;

public:
    using value_type = key_value;
    using reference = key_value;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    constexpr member_iterator() = default;
    constexpr member_iterator(const structural_index *index, std::uint32_t position) noexcept
            : index(index)
            , position(position) {}

    [[nodiscard]] key_value operator*() const noexcept {
        return key_value { span_of_key(*this->index, this->index->at(this->position)),
            indexed_reader { *this->index, this->position } };
    }

    member_iterator &operator++() noexcept {
        this->position = this->index->at(this->position).end;
        return *this;
    }

    member_iterator operator++(int) noexcept {
        auto copy = *this;
        ++*this;
        return copy;
    }

    [[nodiscard]] bool operator==(const member_iterator &other) const noexcept {
        return this->position == other.position;
    }
};

class indexed_reader::member_range {
    const structural_index *index = nullptr;
    std::uint32_t first = 0;
    std::uint32_t last = 0;

public:
    constexpr member_range() = default;
    constexpr member_range(const structural_index *index, std::uint32_t first, std::uint32_t last) noexcept
            : index(index)
            , first(first)
            , last(last) {}

    [[nodiscard]] member_iterator begin() const noexcept { return member_iterator { this->index, this->first }; }
    [[nodiscard]] member_iterator end() const noexcept { return member_iterator { this->index, this->last }; }
};

inline indexed_reader::member_range indexed_reader::items() const noexcept {
    if (!this->valid || !this->is_object()) return {};
    return member_range { this->index, this->position + 1, this->index->at(this->position).end };
}

inline indexed_reader structural_index::root() const noexcept {
    if (this->nodes.empty()) return {};
    return indexed_reader { *this, 0 };
}

} // namespace serpent::json
