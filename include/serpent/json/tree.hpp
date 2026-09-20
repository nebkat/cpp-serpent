#pragma once

// Building a value tree from JSON text in one pass.
//
// The generic walk that serializer<value> makes over any source asks a reader handle what each
// value is and then what it holds, and a handle answers each question with a scan of the value:
// a number is scanned to classify it and scanned again to convert it, a string to find its end
// and again to copy it, and every one arrives in a temporary that is then moved into the tree.
// This is the walk a reader generated for a type makes instead - one cursor, each byte looked
// at once, each value converted straight into the place the tree keeps it - for the type that
// holds anything.
//
// Found by serializer<value> through lookup on the source, as read_reflected is, so a source
// without it takes the generic walk. value.hpp includes this at its end; it is not for
// including on its own.

#include <serpent/error.hpp>
#include <serpent/json/direct.hpp>
#include <serpent/json/reader.hpp>
#include <serpent/json/scan.hpp>
#include <serpent/limits.hpp>
#include <serpent/value.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace serpent::json {

// Not in a detail namespace of its own: a serpent::json::detail would hide serpent::detail from
// every unqualified use of it in this namespace, depending on which header came first.
template<bool Terminated>
class tree_builder {
    scanner::basic_cursor<Terminated> &scan;

    // The members and elements of a container are gathered here and moved into one sized
    // exactly to them: a vector grown into place would be reallocated and its contents moved
    // several times over, and would keep the last growth's slack. One per depth, since a
    // container is gathered while the one above it still is; each keeps its capacity from one
    // container to the next. One for every depth there can be, never grown: a container holds
    // a reference to its own while those below it are gathered.
    std::array<std::vector<serpent::value::object::entry>, max_depth + 1> members_at {};
    std::array<serpent::value::array, max_depth + 1> elements_at {};

public:
    explicit tree_builder(scanner::basic_cursor<Terminated> &scan) noexcept : scan(scan) {}

    /** The value at the cursor, whatever it is, into `into`. False leaves the cursor failed. */
    bool build(serpent::value &into, int depth) {
        if (depth > max_depth) {
            this->scan.fail(errc::depth_exceeded);
            return false;
        }
        scanner::skip_whitespace(this->scan);
        if (!this->scan.need(1)) return false;

        switch (this->scan.peek()) {
        case '{':
            this->scan.advance(1);
            return this->members(into.emplace<serpent::value::object>(), depth);
        case '[':
            this->scan.advance(1);
            return this->elements(into.emplace<serpent::value::array>(), depth);
        case '"': return direct::read(this->scan, into.emplace<std::string>());
        case 't':
            scanner::scan_literal(this->scan, "true");
            into.emplace<bool>(true);
            return this->scan.ok();
        case 'f':
            scanner::scan_literal(this->scan, "false");
            into.emplace<bool>(false);
            return this->scan.ok();
        case 'n':
            scanner::scan_literal(this->scan, "null");
            into.emplace<std::monostate>();
            return this->scan.ok();
        default: return this->number(into);
        }
    }

private:
    /**
     * An integer stays an integer, as the widest of its signedness; anything else is a double.
     * Which it is shows at the first character past the digits, and looking there first saves
     * a real the integer conversion that would otherwise be tried and refused.
     */
    bool number(serpent::value &into) {
        if (!direct::at_number(this->scan)) {
            this->scan.fail(errc::unexpected_character);
            return false;
        }
        const bool negative = this->scan.peek() == '-';
        const char *past_digits = this->scan.position + (negative ? 1 : 0);
        while (past_digits != this->scan.limit && scanner::is_digit(*past_digits)) ++past_digits;
        const bool is_real = past_digits != this->scan.limit
                && (*past_digits == '.' || *past_digits == 'e' || *past_digits == 'E');

        if (!is_real) {
            if (std::int64_t whole; direct::read(this->scan, whole)) {
                into.emplace<std::int64_t>(whole);
                return true;
            }
            if (!this->scan.ok()) return false;
            if (std::uint64_t wide; !negative && direct::read(this->scan, wide)) {
                into.emplace<std::uint64_t>(wide);
                return true;
            }
            if (!this->scan.ok()) return false;
        }
        if (double real; direct::read(this->scan, real)) {
            into.emplace<double>(real);
            return true;
        }
        if (this->scan.ok()) this->scan.fail(errc::invalid_number);
        return false;
    }

    /** After the closing bracket of a value: the separator, or the end of the container. */
    bool closed(char closing) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        const char separator = this->scan.take();
        if (separator == closing) return true;
        if (separator == ',') return false;
        this->scan.fail(errc::unexpected_character, this->scan.position - 1);
        return false;
    }

    bool elements(serpent::value::array &items, int depth) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        if (this->scan.peek() == ']') {
            this->scan.advance(1);
            return true;
        }
        auto &gathered = this->elements_at[static_cast<std::size_t>(depth)];
        gathered.clear();
        do {
            if (!this->build(gathered.emplace_back(), depth + 1)) return false;
        } while (!this->closed(']') && this->scan.ok());
        if (!this->scan.ok()) return false;
        items.reserve(gathered.size());
        for (auto &element : gathered) items.push_back(std::move(element));
        gathered.clear();
        return true;
    }

    bool members(serpent::value::object &members, int depth) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        if (this->scan.peek() == '}') {
            this->scan.advance(1);
            return true;
        }
        auto &gathered = this->members_at[static_cast<std::size_t>(depth)];
        gathered.clear();
        do {
            scanner::skip_whitespace(this->scan);
            auto &[name, held] = gathered.emplace_back();
            if (!direct::read(this->scan, name)) {
                if (this->scan.ok()) this->scan.fail(errc::unexpected_character);
                return false;
            }
            scanner::skip_whitespace(this->scan);
            if (!this->scan.need(1)) return false;
            if (this->scan.take() != ':') {
                this->scan.fail(errc::unexpected_character, this->scan.position - 1);
                return false;
            }
            if (!this->build(held, depth + 1)) return false;
        } while (!this->closed('}') && this->scan.ok());
        if (!this->scan.ok()) return false;
        members.take(gathered);
        members.coalesce_duplicates();
        return true;
    }
};

/**
 * Fills a value tree from the JSON value a reader stands on, reading the document once.
 *
 * Returns false for a document that is malformed at or below that value. Whoever is stepping
 * through the container the value sits in can step to where it ended.
 */
template<bool Terminated>
bool read_tree(const basic_reader<Terminated> &source, serpent::value &into) {
    scanner::basic_cursor<Terminated> scan { source.document(), source.data() };
    tree_builder<Terminated> builder { scan };
    if (!builder.build(into, 0)) return false;
    source.note_end(scan.position);
    return true;
}

} // namespace serpent::json
