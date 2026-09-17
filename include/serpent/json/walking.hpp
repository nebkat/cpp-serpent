#pragma once

// Reading without walking any byte twice, for a document traversed by hand.
//
// A forward iterator has to know where the current value ends before it can hand over the next
// one, and the only way to know is to walk it - so a byte is walked once by the loop that wants
// it and again by the loop stepping over it, once for every level of nesting above it. On a
// document six deep that is six times the work.
//
// The nested loops already walk those bytes. This is how they tell each other:
//
//     auto document = json::walking_reader::over(text);
//     for (auto row : document.array())        // outer
//         for (auto value : row.array())       // inner, and it leaves a note of how far it got
//             total += value.as_int<int>();    // so the outer step resumes rather than restarts
//
// Nothing to manage by hand, and nothing to get wrong: the note is a *memo*, never the truth.
// Every handle still knows its own position, so if the memo does not apply - a value read twice,
// two handles held at once, members taken out of order - the walk falls back to scanning exactly
// as the plain reader does. The worst case is today's behaviour, which is what makes this safe
// to reach for rather than a mode to be careful in.

#include <serpent/json/reader.hpp>

#include <string_view>

namespace serpent::json {

/**
 * @brief How far a traversal has got, and into which value.
 *
 * One of these per document, describing the innermost traversal currently in progress. Each
 * level takes it over as control comes back to it, so it is a single small struct rather than a
 * stack: finding the end of a value only ever needs the point reached inside it and how many
 * containers are still open there.
 */
struct walk_memo {
    const char *owner = nullptr;   ///< first character of the value being walked
    const char *reached = nullptr; ///< how far into it anything has scanned
    int open = 0;                  ///< containers still open at `reached`, counting from `owner`

    /** Whether this memo says anything about the value beginning at `first`. */
    [[nodiscard]] constexpr bool describes(const char *first) const noexcept {
        return this->owner == first && this->reached != nullptr;
    }

    constexpr void note(const char *first, const char *position, int still_open) noexcept {
        this->owner = first;
        this->reached = position;
        this->open = still_open;
    }
};

namespace scanner {

/** Consumes forward until `open` containers have been closed, leaving the cursor just past. */
inline void close_containers(cursor &scan, int open) noexcept {
    while (open > 0) {
        skip_whitespace(scan);
        if (!scan.available(1)) {
            scan.fail(errc::unexpected_end);
            return;
        }
        const char here = scan.peek();
        if (here == '"') {
            // A bracket inside a string is not a bracket, so strings are consumed as a whole.
            (void)scan_string(scan);
            continue;
        }
        if (here == '[' || here == '{') {
            ++open;
        } else if (here == ']' || here == '}') {
            --open;
        }
        scan.advance(1);
    }
}

} // namespace scanner

class walking_reader;

/**
 * @brief A handle that answers what a reader answers, over a document that keeps a memo.
 *
 * Everything about what a value *is* goes to an ordinary reader. Only stepping to the next
 * value is different, and only because the memo may already know where this one ended.
 */
class walking_reader {
    reader plain;
    walk_memo *memo = nullptr;

public:
    constexpr walking_reader() = default;
    constexpr walking_reader(reader plain, walk_memo *memo) noexcept : plain(plain), memo(memo) {}

    /** Wraps a document. The memo must outlive every handle taken from it. */
    [[nodiscard]] static walking_reader over(std::string_view text, walk_memo &memo) noexcept {
        return walking_reader { reader::over(text), &memo };
    }

    /** The plain handle, for anything this does not offer. */
    [[nodiscard]] reader scanning() const noexcept { return this->plain; }

    [[nodiscard]] kind type() const noexcept { return this->plain.type(); }
    [[nodiscard]] bool is_valid() const noexcept { return this->plain.is_valid(); }
    [[nodiscard]] explicit operator bool() const noexcept { return this->is_valid(); }
    [[nodiscard]] bool is_null() const noexcept { return this->plain.is_null(); }
    [[nodiscard]] bool is_boolean() const noexcept { return this->plain.is_boolean(); }
    [[nodiscard]] bool is_integer() const noexcept { return this->plain.is_integer(); }
    [[nodiscard]] bool is_real() const noexcept { return this->plain.is_real(); }
    [[nodiscard]] bool is_number() const noexcept { return this->plain.is_number(); }
    [[nodiscard]] bool is_string() const noexcept { return this->plain.is_string(); }
    [[nodiscard]] bool is_array() const noexcept { return this->plain.is_array(); }
    [[nodiscard]] bool is_object() const noexcept { return this->plain.is_object(); }

    [[nodiscard]] std::optional<bool> as_bool() const noexcept { return this->plain.as_bool(); }

    template<std::integral T>
    [[nodiscard]] std::optional<T> as_int() const noexcept {
        return this->plain.template as_int<T>();
    }

    template<std::floating_point T>
    [[nodiscard]] std::optional<T> as_float() const noexcept {
        return this->plain.template as_float<T>();
    }

    [[nodiscard]] std::optional<std::string> as_string() const { return this->plain.as_string(); }
    [[nodiscard]] std::size_t size() const noexcept { return this->plain.size(); }

    [[nodiscard]] walking_reader operator[](std::string_view name) const noexcept {
        return walking_reader { this->plain[name], this->memo };
    }

    [[nodiscard]] walking_reader operator[](std::size_t index) const noexcept {
        return walking_reader { this->plain[index], this->memo };
    }

    /**
     * Whatever this value holds, as one of your types.
     *
     * The same dispatch the plain reader makes, but reading through *this* handle: a container
     * filled from the document walks it, and that walk is the one that leaves the notes.
     * Delegating to the plain handle here would read correctly and quietly give up the walk,
     * which is the whole reason this type exists.
     */
    template<typename T>
    [[nodiscard]] std::optional<T> try_get() const {
        if constexpr (std::same_as<T, bool>) {
            return this->as_bool();
        } else if constexpr (serpent::detail::string_like<T> && std::constructible_from<T, std::string_view>) {
            return this->plain.template try_get<T>();
        } else if constexpr (std::floating_point<T>) {
            return this->template as_float<T>();
        } else if constexpr (std::integral<T>) {
            return this->template as_int<T>();
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

    [[nodiscard]] const char *data() const noexcept { return this->plain.data(); }
    [[nodiscard]] std::string_view buffer() const noexcept { return this->plain.buffer(); }
    [[nodiscard]] walk_memo *notes() const noexcept { return this->memo; }

    class iterator;
    class range;
    struct key_value;
    class member_iterator;
    class member_range;

    [[nodiscard]] inline range array() const noexcept;
    [[nodiscard]] inline member_range items() const noexcept;
};

/**
 * @brief Steps to the next element, resuming from the memo where it applies.
 *
 * The element just handed out may have been walked by a loop of its own. If it was, the memo
 * says where that loop got to and how deep it was, and finishing the element is whatever is
 * left rather than the whole of it. If it was not, this is an ordinary skip.
 */
class walking_reader::iterator {
    std::string_view source;
    const char *element = nullptr;
    const char *container = nullptr;
    walk_memo *memo = nullptr;
    bool done = true;

    void step() noexcept {
        scanner::cursor scan { this->source, this->element };
        int open = 0;

        if (this->memo != nullptr && this->memo->describes(this->element)) {
            // Somebody already walked into this element. Carry on from there.
            scan.position = this->memo->reached;
            open = this->memo->open;
        }

        if (open > 0) {
            scanner::close_containers(scan, open);
        } else if (scan.position == this->element) {
            scanner::skip_value(scan, 1);
        }

        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1)) {
            this->done = true;
            return;
        }
        if (scan.peek() == ',') {
            scan.advance(1);
            scanner::skip_whitespace(scan);
            if (!scan.available(1)) {
                this->done = true;
                return;
            }
            this->element = scan.position;
            // This level owns the memo again, positioned between elements of its own container.
            if (this->memo != nullptr) this->memo->note(this->container, scan.position, 1);
            return;
        }
        this->done = true;
        if (this->memo != nullptr && scan.peek() == ']') {
            scan.advance(1);
            this->memo->note(this->container, scan.position, 0);
        }
    }

public:
    using value_type = walking_reader;
    using reference = walking_reader;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    constexpr iterator() = default;
    iterator(std::string_view source, const char *element, const char *container, walk_memo *memo) noexcept
            : source(source)
            , element(element)
            , container(container)
            , memo(memo)
            , done(element == nullptr) {}

    [[nodiscard]] walking_reader operator*() const noexcept {
        return walking_reader { reader { this->source, this->element }, this->memo };
    }

    iterator &operator++() noexcept {
        if (!this->done) this->step();
        return *this;
    }

    iterator operator++(int) noexcept {
        auto copy = *this;
        ++*this;
        return copy;
    }

    [[nodiscard]] bool operator==(const iterator &other) const noexcept {
        if (this->done || other.done) return this->done == other.done;
        return this->element == other.element;
    }
};

class walking_reader::range {
    std::string_view source;
    const char *first = nullptr;
    const char *container = nullptr;
    walk_memo *memo = nullptr;

public:
    constexpr range() = default;
    range(std::string_view source, const char *first, const char *container, walk_memo *memo) noexcept
            : source(source)
            , first(first)
            , container(container)
            , memo(memo) {}

    [[nodiscard]] iterator begin() const noexcept {
        return iterator { this->source, this->first, this->container, this->memo };
    }

    [[nodiscard]] iterator end() const noexcept { return iterator {}; }
};

inline walking_reader::range walking_reader::array() const noexcept {
    if (!this->is_array()) return {};
    scanner::cursor scan { this->plain.buffer(), this->plain.data() + 1 };
    scanner::skip_whitespace(scan);
    if (!scan.available(1) || scan.peek() == ']') return {};

    // Taking the memo over: this value is what is being walked now, one container deep into it.
    if (this->memo != nullptr) this->memo->note(this->plain.data(), scan.position, 1);
    return range { this->plain.buffer(), scan.position, this->plain.data(), this->memo };
}

/** A key and its value, the shape every reader's items() yields. */
struct walking_reader::key_value {
    scanner::string_span key;
    walking_reader value;

    [[nodiscard]] bool key_is(std::string_view other) const noexcept { return scanner::equals(this->key, other); }

    [[nodiscard]] std::string key_string() const {
        std::string decoded;
        decoded.reserve(scanner::decoded_length(this->key));
        scanner::decode_string(this->key, [&](char value) { decoded.push_back(value); });
        return decoded;
    }
};

/** The same, for members: the key is scanned, then the value is stepped over as above. */
class walking_reader::member_iterator {
    std::string_view source;
    const char *value_at = nullptr;
    const char *container = nullptr;
    scanner::string_span key {};
    walk_memo *memo = nullptr;
    bool done = true;

    void take_member(scanner::cursor &scan) noexcept {
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() != '"') {
            this->done = true;
            return;
        }
        this->key = scanner::scan_string(scan);
        scanner::skip_whitespace(scan);
        if (!scan.available(1) || scan.peek() != ':') {
            this->done = true;
            return;
        }
        scan.advance(1);
        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1)) {
            this->done = true;
            return;
        }
        this->value_at = scan.position;
        this->done = false;
        if (this->memo != nullptr) this->memo->note(this->container, scan.position, 1);
    }

public:
    using value_type = key_value;
    using reference = key_value;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    constexpr member_iterator() = default;
    member_iterator(std::string_view source, const char *start, const char *container, walk_memo *memo) noexcept
            : source(source)
            , container(container)
            , memo(memo) {
        scanner::cursor scan { source, start };
        this->take_member(scan);
    }

    [[nodiscard]] key_value operator*() const noexcept {
        return key_value { this->key, walking_reader { reader { this->source, this->value_at }, this->memo } };
    }

    member_iterator &operator++() noexcept {
        if (this->done) return *this;

        scanner::cursor scan { this->source, this->value_at };
        int open = 0;
        if (this->memo != nullptr && this->memo->describes(this->value_at)) {
            scan.position = this->memo->reached;
            open = this->memo->open;
        }
        if (open > 0) {
            scanner::close_containers(scan, open);
        } else if (scan.position == this->value_at) {
            scanner::skip_value(scan, 1);
        }

        scanner::skip_whitespace(scan);
        if (!scan.ok() || !scan.available(1) || scan.peek() != ',') {
            this->done = true;
            if (this->memo != nullptr && scan.ok() && scan.available(1) && scan.peek() == '}') {
                scan.advance(1);
                this->memo->note(this->container, scan.position, 0);
            }
            return *this;
        }
        scan.advance(1);
        this->take_member(scan);
        return *this;
    }

    member_iterator operator++(int) noexcept {
        auto copy = *this;
        ++*this;
        return copy;
    }

    [[nodiscard]] bool operator==(const member_iterator &other) const noexcept {
        if (this->done || other.done) return this->done == other.done;
        return this->value_at == other.value_at;
    }
};

class walking_reader::member_range {
    std::string_view source;
    const char *start = nullptr;
    const char *container = nullptr;
    walk_memo *memo = nullptr;
    bool empty = true;

public:
    constexpr member_range() = default;
    member_range(std::string_view source, const char *start, const char *container, walk_memo *memo) noexcept
            : source(source)
            , start(start)
            , container(container)
            , memo(memo)
            , empty(false) {}

    [[nodiscard]] member_iterator begin() const noexcept {
        if (this->empty) return {};
        return member_iterator { this->source, this->start, this->container, this->memo };
    }

    [[nodiscard]] member_iterator end() const noexcept { return {}; }
};

inline walking_reader::member_range walking_reader::items() const noexcept {
    if (!this->is_object()) return {};
    scanner::cursor scan { this->plain.buffer(), this->plain.data() + 1 };
    scanner::skip_whitespace(scan);
    if (!scan.available(1) || scan.peek() == '}') return {};
    return member_range { this->plain.buffer(), scan.position, this->plain.data(), this->memo };
}

/** A document and the memo its handles share. Hold one of these; take handles from it. */
class walking_document {
    std::string text;
    walk_memo memo;

public:
    explicit walking_document(std::string text) noexcept : text(std::move(text)) {}

    [[nodiscard]] walking_reader root() noexcept { return walking_reader::over(this->text, this->memo); }
    [[nodiscard]] std::string_view buffer() const noexcept { return this->text; }
};

} // namespace serpent::json
