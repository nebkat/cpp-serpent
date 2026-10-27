#pragma once

// Building a value tree from JSON text in one pass.
//
// The generic walk that serializer<value> makes over any source asks a reader handle what each
// value is and then what it holds, and a handle answers each question with a scan of the value:
// a number is scanned to classify it and scanned again to convert it, a string to find its end
// and again to copy it, and every one arrives in a temporary that is then moved into the tree.
// This is the walk a reader generated for a type makes instead - one cursor, each byte looked
// at once, each value converted straight into the place the tree keeps it - for the type that
// holds anything. The containers still open are kept in a stack of its own, on the heap, so the
// stack it takes is the same however deep the document goes.
//
// Found by serializer<value> through lookup on the source, as read_reflected is, so a source
// without it takes the generic walk. value.hpp includes this at its end; it is not for
// including on its own.

#include <serpent/error.hpp>
#include <serpent/json/direct.hpp>
#include <serpent/json/reader.hpp>
#include <serpent/json/scan.hpp>
#include <serpent/limits.hpp>
#include <serpent/tree_stack.hpp>
#include <serpent/value.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace serpent::json {

// Not in a detail namespace of its own: a serpent::json::detail would hide serpent::detail from
// every unqualified use of it in this namespace, depending on which header came first.
template<bool Terminated>
class tree_builder : serpent::detail::tree_stack<serpent::value> {
    scanner::basic_cursor<Terminated> &scan;
    // One buffer for every string in the document: a value holds its text itself, so the decode
    // needs somewhere to land and nothing needs to keep it afterwards.
    std::string scratch;

public:
    explicit tree_builder(scanner::basic_cursor<Terminated> &scan) noexcept : scan(scan) {}

    /** The value at the cursor, whatever it is, into `into`. False leaves the cursor failed. */
    bool build(serpent::value &into) {
        serpent::value held;
        bool complete = false;
        if (!this->begin(0, held, complete)) return false;
        while (!complete || !this->frames.empty()) {
            const bool filled = this->frames.back().object ? this->members(held, complete) : this->elements(held, complete);
            if (!filled) return false;
        }
        into = std::move(held);
        return true;
    }

private:
    // The container on top filled until it closes, answering it complete in `held`, or until a
    // child container opens, answering that incomplete. `held` is a child completed below it
    // first, if `complete` says so. One loop for each kind, so each knows its own bracket.

    bool elements(serpent::value &held, bool &complete) {
        const int depth = static_cast<int>(this->frames.size());
        while (true) {
            if (complete) {
                this->gathered_elements.push_back(std::move(held));
                if (this->closed(']')) {
                    held = this->close();
                    return true;
                }
                if (!this->scan.ok()) return false;
            }
            if (!this->begin(depth, held, complete)) return false;
            if (!complete) return true;
        }
    }

    bool members(serpent::value &held, bool &complete) {
        const int depth = static_cast<int>(this->frames.size());
        while (true) {
            if (complete) {
                this->gathered_members.back().second = std::move(held);
                if (this->closed('}')) {
                    held = this->close();
                    return true;
                }
                if (!this->scan.ok()) return false;
            }
            if (!this->key() || !this->begin(depth, held, complete)) return false;
            if (!complete) return true;
        }
    }

    /**
     * Starts the value at the cursor: complete at once unless it is a container with something
     * in it, which is opened for the loop to fill.
     */
    bool begin(int depth, serpent::value &held, bool &complete) {
        if (depth > max_depth) {
            this->scan.fail(errc::depth_exceeded);
            return false;
        }
        scanner::skip_whitespace(this->scan);
        if (!this->scan.need(1)) return false;

        complete = true;
        switch (this->scan.peek()) {
        case '{':
            this->scan.advance(1);
            return this->container(true, held, complete);
        case '[':
            this->scan.advance(1);
            return this->container(false, held, complete);
        case '"': {
            // Taken as the scan found it: a string with no escape in it needs no decoding, so
            // it goes straight into the node from the text rather than through the scratch.
            const auto span = scanner::scan_string(this->scan);
            if (!this->scan.ok()) return false;
            held = this->scanned_text(span);
            return true;
        }
        case 't':
            scanner::scan_literal(this->scan, "true");
            held = true;
            return this->scan.ok();
        case 'f':
            scanner::scan_literal(this->scan, "false");
            held = false;
            return this->scan.ok();
        case 'n':
            scanner::scan_literal(this->scan, "null");
            held = serpent::value {};
            return this->scan.ok();
        default: return this->number(held);
        }
    }

    /** A container whose opening bracket has just been read: empty, or opened. */
    bool container(bool object, serpent::value &held, bool &complete) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        if (this->scan.peek() == (object ? '}' : ']')) {
            this->scan.advance(1);
            held = object ? serpent::value::object() : serpent::value { static_cast<serpent::value::array_type *>(nullptr) };
            return true;
        }
        // Gathered first, so a failure part-way leaves nothing half-built in the tree, and each
        // run is sized once to what it holds rather than grown.
        this->open(object, 0, true);
        complete = false;
        return true;
    }

    /** A member's name and the colon after it. */
    bool key() {
        scanner::skip_whitespace(this->scan);
        // A name is a value like any other now, so it decodes into the scratch buffer and lands
        // inline where it is short - which nearly every name is.
        if (!this->scan.available(1) || this->scan.peek() != '"') {
            if (this->scan.ok()) this->scan.fail(errc::unexpected_character);
            return false;
        }
        const auto span = scanner::scan_string(this->scan);
        if (!this->scan.ok()) return false;
        this->name(this->scanned_text(span));
        scanner::skip_whitespace(this->scan);
        if (!this->scan.need(1)) return false;
        if (this->scan.take() != ':') {
            this->scan.fail(errc::unexpected_character, this->scan.position - 1);
            return false;
        }
        return true;
    }

    /**
     * A string as the scan left it: taken whole where nothing between the quotes needs decoding,
     * which is most strings, and decoded through the one scratch buffer where something does.
     */
    [[nodiscard]] serpent::value scanned_text(const scanner::string_span &span) {
        serpent::value held;
        if (!span.escaped) {
            held.assign(span.contents);
            return held;
        }
        this->scratch.clear();
        scanner::decode_string(span, this->scratch);
        held.assign(this->scratch);
        return held;
    }

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
                into = whole;
                return true;
            }
            if (!this->scan.ok()) return false;
            if (std::uint64_t wide; !negative && direct::read(this->scan, wide)) {
                into = wide;
                return true;
            }
            if (!this->scan.ok()) return false;
        }
        if (double real; direct::read(this->scan, real)) {
            into = real;
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
    if (!builder.build(into)) return false;
    source.note_end(scan.position);
    return true;
}

} // namespace serpent::json
