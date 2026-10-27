// The JSON tree builder against the recursive one it replaces: every document - each fixture,
// every prefix of one, each with any byte changed, and generated ones of every shape - builds
// into the same tree both ways or fails both ways, read within its bounds and read as terminated.
// The recursive builder is kept here as it was, a call deeper for every level, so it can be the
// reference for as long as the builder exists.

#include "check.hpp"

#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace serpent::json::recursive {

template<bool Terminated>
class recursive_builder {
    scanner::basic_cursor<Terminated> &scan;
    // One buffer for every string in the document: a value holds its text itself, so the decode
    // needs somewhere to land and nothing needs to keep it afterwards.
    std::string scratch;

    // The members and elements of a container are gathered here and moved into one sized
    // exactly to them: a vector grown into place would be reallocated and its contents moved
    // several times over, and would keep the last growth's slack. Used as a stack: a container
    // gathers after whatever the one above it has gathered so far, and cuts back to that when
    // it closes, so the two keep their capacity from one container to the next. A child is
    // built into a local and pushed afterwards, since a push below may move the vector.
    std::vector<serpent::member> gathered_members;
    std::vector<serpent::value> gathered_elements;

public:
    explicit recursive_builder(scanner::basic_cursor<Terminated> &scan) noexcept : scan(scan) {}

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
            return this->members(into, depth);
        case '[':
            this->scan.advance(1);
            return this->elements(into, depth);
        case '"': {
            // Taken as the scan found it: a string with no escape in it needs no decoding, so
            // it goes straight into the node from the text rather than through the scratch.
            const auto span = scanner::scan_string(this->scan);
            if (!this->scan.ok()) return false;
            into = this->scanned_text(span);
            return true;
        }
        case 't':
            scanner::scan_literal(this->scan, "true");
            into = true;
            return this->scan.ok();
        case 'f':
            scanner::scan_literal(this->scan, "false");
            into = false;
            return this->scan.ok();
        case 'n':
            scanner::scan_literal(this->scan, "null");
            into = serpent::value {};
            return this->scan.ok();
        default: return this->number(into);
        }
    }

private:
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

    bool elements(serpent::value &into, int depth) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        if (this->scan.peek() == ']') {
            this->scan.advance(1);
            into = serpent::value { static_cast<serpent::value::array_type *>(nullptr) };
            return true;
        }
        // Gathered first, so a failure part-way leaves nothing half-built in the tree, and the
        // run is grown once rather than per element of every nesting above it.
        const std::size_t start = this->gathered_elements.size();
        do {
            serpent::value held;
            if (!this->build(held, depth + 1)) return false;
            this->gathered_elements.push_back(std::move(held));
        } while (!this->closed(']') && this->scan.ok());
        if (!this->scan.ok()) return false;

        // Sized once from what was gathered, so the run is allocated exactly and never grown.
        const auto gathered = std::span { this->gathered_elements }.subspan(start);
        auto *block = serpent::value::array_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        serpent::value::array_type::place_all(block, gathered);
        this->gathered_elements.resize(start);
        into = serpent::value { block };
        return true;
    }

    bool members(serpent::value &into, int depth) {
        scanner::skip_whitespace(this->scan);
        if (!this->scan.available(1)) {
            this->scan.fail(errc::unterminated_container);
            return false;
        }
        if (this->scan.peek() == '}') {
            this->scan.advance(1);
            into = serpent::value::object();
            return true;
        }
        const std::size_t start = this->gathered_members.size();
        do {
            scanner::skip_whitespace(this->scan);
            serpent::value name;
            serpent::value held;
            // A name is a value like any other now, so it decodes into the scratch buffer and
            // lands inline where it is short - which nearly every name is.
            if (!this->scan.available(1) || this->scan.peek() != '"') {
                if (this->scan.ok()) this->scan.fail(errc::unexpected_character);
                return false;
            }
            const auto span = scanner::scan_string(this->scan);
            if (!this->scan.ok()) return false;
            name = this->scanned_text(span);
            scanner::skip_whitespace(this->scan);
            if (!this->scan.need(1)) return false;
            if (this->scan.take() != ':') {
                this->scan.fail(errc::unexpected_character, this->scan.position - 1);
                return false;
            }
            if (!this->build(held, depth + 1)) return false;
            this->gathered_members.emplace_back(std::move(name), std::move(held));
        } while (!this->closed('}') && this->scan.ok());
        if (!this->scan.ok()) return false;
        const auto gathered = std::span { this->gathered_members }.subspan(start);
        auto *block = serpent::value::object_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        serpent::value::object_type::place_all(block, gathered);
        this->gathered_members.resize(start);
        serpent::detail::coalesce_run(block);
        into = serpent::value { block };
        return true;
    }
};

} // namespace serpent::json::recursive

using namespace serpent;

namespace {

/** Decoded the old way: built recursively, then held to ending where the text does, as decode() holds it. */
template<bool Terminated>
std::optional<value> decoded_recursively(std::string_view text) {
    const auto root = json::basic_reader<Terminated>::over(text);
    if (!root.data()) return std::nullopt;
    json::scanner::basic_cursor<Terminated> scan { root.document(), root.data() };
    json::recursive::recursive_builder<Terminated> builder { scan };
    value tree;
    if (!builder.build(tree, 0)) return std::nullopt;
    root.note_end(scan.position);
    if (!json::scanner::ends_with_text(root, text)) return std::nullopt;
    return tree;
}

/** The same tree: the same shapes in the same order, a NaN the same as itself. */
bool same(const value &left, const value &right) {
    if (left.type() != right.type() || left.is_binary() != right.is_binary()) return false;
    switch (left.type()) {
    case kind::null: return true;
    case kind::boolean: return left.as<bool>() == right.as<bool>();
    case kind::integer: return left.as<std::int64_t>() == right.as<std::int64_t>() && left.as<std::uint64_t>() == right.as<std::uint64_t>();
    case kind::real: return std::bit_cast<std::uint64_t>(*left.as<double>()) == std::bit_cast<std::uint64_t>(*right.as<double>());
    case kind::string: return left.as<std::string_view>() == right.as<std::string_view>();
    case kind::array: {
        const auto ours = left.as_array();
        const auto theirs = right.as_array();
        if (ours.size() != theirs.size()) return false;
        for (std::size_t index = 0; index < ours.size(); ++index)
            if (!same(ours[index], theirs[index])) return false;
        return true;
    }
    case kind::object: {
        const auto ours = left.as_object();
        const auto theirs = right.as_object();
        if (ours.size() != theirs.size()) return false;
        for (std::size_t index = 0; index < ours.size(); ++index)
            if (!same(ours[index].first, theirs[index].first) || !same(ours[index].second, theirs[index].second)) return false;
        return true;
    }
    default: return false;
    }
}

int compared = 0;
int built = 0;

void agree(const std::string &text, const std::string &what) {
    ++compared;
    const auto reference = decoded_recursively<false>(text);
    const auto bounded = json::decode<value>(std::string_view { text });
    const auto terminated = json::decode<value>(text);
    check(decoded_recursively<true>(text).has_value() == reference.has_value(), what + ": the reference reads it the same both ways");
    if (reference.has_value() != bounded.has_value() || reference.has_value() != terminated.has_value()) {
        check(false, what + (reference ? ": the recursive builder reads it, but it does not build" : ": builds, but the recursive builder refuses it"));
        return;
    }
    if (!reference) return;
    ++built;
    check(same(*reference, *bounded) && same(*reference, *terminated), what + ": builds a different tree");
}

/** Each document whole, cut short at every length, and with every byte in turn changed - every one of a small document, and spread over a large one. */
void mutated(const std::string &document, const std::string &name) {
    const std::size_t stride = document.size() <= 256 ? 1 : document.size() / 16 + 1;
    agree(document, name);
    for (std::size_t length = 0; length < document.size(); length += stride) agree(document.substr(0, length), name + " cut to " + std::to_string(length));

    // Characters that change what a document means: the structure, what starts a value, what
    // continues a number, a string's escapes, and whitespace.
    static constexpr char replacements[] = { '{', '}', '[', ']', ',', ':', '"', '\\', '-', '+', '0', '1', '9', '.', 'e', 'E',
        't', 'n', 'u', ' ', '\n', 'x', '\0' };
    std::string changed = document;
    for (std::size_t at = 0; at < document.size(); at += stride) {
        for (const char replacement : replacements) {
            if (replacement == document[at]) continue;
            changed[at] = replacement;
            agree(changed, name + " byte " + std::to_string(at) + " = " + std::to_string(int(replacement)));
        }
        changed[at] = document[at];
    }
}

void fixtures() {
    for (const auto &entry : std::filesystem::recursive_directory_iterator { SERPENT_FIXTURE_DIR }) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in { entry.path(), std::ios::binary };
        mutated(std::string { std::istreambuf_iterator<char> { in }, {} }, entry.path().filename().string());
    }
}

/** Documents of every shape JSON has, made at random from a seed. */
class generator {
    std::mt19937_64 random;
    std::string out;

    int below(int bound) { return static_cast<int>(this->random() % static_cast<std::uint64_t>(bound)); }

    void space() {
        static constexpr const char *spaces[] = { "", "", "", " ", "\n", "\t ", "\r\n  " };
        this->out += spaces[this->below(7)];
    }

    void text() {
        static constexpr const char *pieces[] = { "a", "b", "id", "name", "\\n", "\\\"", "\\\\", "\\u00e9", "\\ud83d\\ude00", "é", "", "x y" };
        this->out += '"';
        const int count = this->below(4);
        for (int index = 0; index < count; ++index) this->out += pieces[this->below(12)];
        this->out += '"';
    }

    void key() {
        // Few names, so that objects have duplicates.
        static constexpr const char *names[] = { "\"\"", "\"a\"", "\"b\"", "\"id\"", "\"\\u0061\"" };
        this->out += names[this->below(5)];
    }

    void number() {
        static constexpr const char *numbers[] = { "0", "-0", "1", "-1", "42", "9223372036854775807", "-9223372036854775808",
            "9223372036854775808", "18446744073709551615", "18446744073709551616", "-9223372036854775809", "123456789012345678901234567890",
            "0.5", "-1.25", "1e5", "1E-5", "2.5e+300", "1e400", "-1e-400", "3.141592653589793", "0.1" };
        this->out += numbers[this->below(21)];
    }

    void scalar() {
        switch (this->below(6)) {
        case 0: this->out += "null"; break;
        case 1: this->out += this->below(2) == 0 ? "true" : "false"; break;
        case 2:
        case 3: this->number(); break;
        default: this->text(); break;
        }
    }

    void container(int depth) {
        const bool object = this->below(2) == 0;
        const int count = this->below(depth > 3 ? 3 : 6);
        this->out += object ? '{' : '[';
        this->space();
        for (int index = 0; index < count; ++index) {
            if (index > 0) {
                this->out += ',';
                this->space();
            }
            if (object) {
                this->key();
                this->space();
                this->out += ':';
                this->space();
            }
            this->any(depth + 1);
            this->space();
        }
        this->out += object ? '}' : ']';
    }

public:
    explicit generator(std::uint64_t seed) : random(seed) {}

    void any(int depth) {
        if (depth >= 6 || this->below(5) < 2) this->scalar();
        else this->container(depth);
    }

    std::string document() {
        this->out.clear();
        this->space();
        this->any(0);
        this->space();
        return this->out;
    }

    /** One value `depth` containers down, the containers in whichever form. */
    std::string nested(int depth) {
        this->out.clear();
        std::string closing;
        for (int level = 0; level < depth; ++level) {
            if (this->below(2) == 0) {
                this->out += '[';
                closing.insert(closing.begin(), ']');
            } else {
                this->out += "{\"a\":";
                closing.insert(closing.begin(), '}');
            }
            this->space();
        }
        this->any(5);
        this->out += closing;
        return this->out;
    }
};

void generated() {
    for (std::uint64_t seed = 0; seed < 3'000; ++seed) {
        generator documents { seed };
        const auto document = documents.document();
        agree(document, "generated " + std::to_string(seed));
        if (seed < 50) mutated(document, "generated " + std::to_string(seed));
    }
}

void deep() {
    // To the limit and past it, through both kinds of container and every shape of value.
    for (std::uint64_t seed = 0; seed < 2'000; ++seed) {
        generator documents { seed };
        const int depth = max_depth - 2 + static_cast<int>(seed % 4);
        agree(documents.nested(depth), "nested " + std::to_string(depth) + " from seed " + std::to_string(seed));
    }
}

} // namespace

int main() {
    fixtures();
    generated();
    deep();
    std::printf("json_tree: %d documents compared, %d of them built\n", compared, built);
    check(built > 2'000, "plenty of the documents are whole");
    return report("json_tree");
}
