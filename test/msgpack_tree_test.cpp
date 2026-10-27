// The MessagePack tree builder against the walk it replaces: every document - each JSON fixture
// written as MessagePack, every prefix of one, each with any byte changed, and generated ones of
// every shape - builds into the same tree both ways or fails both ways. The walk is kept here as
// it was, a reader asked what each value is and then what it holds, a call deeper for every
// level, so it can be the reference for as long as the builder exists.

#include "check.hpp"

#include <serpent/json.hpp>
#include <serpent/msgpack.hpp>
#include <serpent/value.hpp>

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace serpent;

namespace {

/** The generic walk, recursing into itself rather than into whatever read() would find. */
bool walked(const msgpack::reader &source, value &item) {
    switch (source.type()) {
    case kind::invalid: return false;
    case kind::null: item = value {}; return true;
    case kind::boolean: item = value { *source.as<bool>() }; return true;
    case kind::integer:
        if (const auto whole = source.as<std::int64_t>()) item = value { *whole };
        else if (const auto unsigned_whole = source.as<std::uint64_t>()) item = value { *unsigned_whole };
        else return false;
        return true;
    case kind::real: {
        const auto number = source.as<double>();
        if (!number) return false;
        item = value { *number };
        return true;
    }
    case kind::string: {
        const auto text = source.as<std::string_view>();
        if (!text) return false;
        item = value { std::string { *text } };
        return true;
    }
    case kind::array: {
        if (const auto bytes = source.as<std::span<const std::byte>>()) {
            item = value { *bytes };
            return true;
        }
        value built = value::array();
        for (const auto &element : source.array()) {
            value held;
            if (!walked(element, held)) return false;
            built.push_back(std::move(held));
        }
        item = std::move(built);
        return true;
    }
    case kind::object: {
        value built = value::object();
        std::size_t members = 0;
        for (const auto &entry : source.items()) {
            value held;
            if (!walked(entry.value, held)) return false;
            built.append_member(entry.key_string(), std::move(held));
            ++members;
        }
        // items() stops at a key that is not a str, which the builder refuses rather than
        // leaving out what follows it.
        if (members != source.size()) return false;
        built.coalesce_members();
        item = std::move(built);
        return true;
    }
    }
    return false;
}

/** Decoded the old way: walked, then held to ending where the buffer does, as decode() holds it. */
std::optional<value> decoded_by_walking(std::span<const std::byte> buffer) {
    const auto root = msgpack::reader::over(buffer);
    value tree;
    if (!walked(root, tree) || !msgpack::detail::ends_with_buffer(root, buffer)) return std::nullopt;
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
        if (left.is_binary()) return std::ranges::equal(*left.as<std::span<const std::byte>>(), *right.as<std::span<const std::byte>>());
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

void agree(std::span<const std::byte> buffer, const std::string &what) {
    ++compared;
    const auto reference = decoded_by_walking(buffer);
    const auto tree = msgpack::decode<value>(buffer);
    if (reference.has_value() != tree.has_value()) {
        check(false, what + (tree ? ": builds, but the walk refuses it" : ": the walk reads it, but it does not build"));
        return;
    }
    if (!tree) return;
    ++built;
    check(same(*reference, *tree), what + ": builds a different tree");

    const auto tried = msgpack::try_decode<value>(buffer);
    check(tried.has_value() && same(*tried, *tree), what + ": try_decode builds it too");
}

void agree_refused(std::span<const std::byte> buffer, const std::string &what) {
    agree(buffer, what);
    const auto tried = msgpack::try_decode<value>(buffer);
    const auto checked = msgpack::validate(buffer);
    if (!tried && !checked) check(tried.error().code() == checked.error().code(), what + ": try_decode fails as validate does");
}

std::vector<std::byte> bytes_of(const std::string &text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

/** Each document whole, cut short at every length, and with every byte in turn changed - every one of a small document, and spread over a large one. */
void mutated(const std::vector<std::byte> &document, const std::string &name) {
    const std::size_t stride = document.size() <= 256 ? 1 : document.size() / 16 + 1;
    agree_refused(document, name);
    for (std::size_t length = 0; length < document.size(); length += stride)
        agree_refused(std::span { document }.first(length), name + " cut to " + std::to_string(length));

    // Bytes that change what a document means: each family's format bytes, the ends of the
    // ranges that hold a length or count in themselves, and values that make lengths huge or zero.
    static constexpr unsigned char replacements[] = { 0x00, 0x01, 0x7F, 0x80, 0x81, 0x8F, 0x90, 0x91, 0x9F, 0xA0, 0xA1, 0xBF,
        0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC7, 0xCA, 0xCB, 0xCC, 0xCF, 0xD0, 0xD3, 0xD4, 0xD9, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xFF };
    std::vector<std::byte> changed = document;
    for (std::size_t at = 0; at < document.size(); at += stride) {
        for (const auto replacement : replacements) {
            if (std::byte { replacement } == document[at]) continue;
            changed[at] = std::byte { replacement };
            agree_refused(changed, name + " byte " + std::to_string(at) + " = " + std::to_string(replacement));
        }
        changed[at] = document[at];
    }
}

/** Every JSON fixture that parses, written as MessagePack. */
void fixtures() {
    for (const auto &entry : std::filesystem::recursive_directory_iterator { SERPENT_FIXTURE_DIR }) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in { entry.path(), std::ios::binary };
        const std::string text { std::istreambuf_iterator<char> { in }, {} };
        const auto tree = json::decode<value>(text);
        if (!tree) continue;
        mutated(msgpack::encode(*tree), entry.path().filename().string());
    }
}

/** Documents of every shape the format has, made at random from a seed. */
class generator {
    std::mt19937_64 random;
    std::string out;

    int below(int bound) { return static_cast<int>(this->random() % static_cast<std::uint64_t>(bound)); }

    void big(std::uint64_t bits, int width) {
        for (int index = width - 1; index >= 0; --index) this->out.push_back(static_cast<char>(bits >> (8 * index)));
    }

    void head(int value) { this->out.push_back(static_cast<char>(value)); }

    /** A length or count after one of three format bytes, or in the fix byte where it fits - whichever the writer would not have chosen, now and then. */
    void sized(std::size_t count, int fix, int fix_limit, int one, int two, int four) {
        const int pick = this->below(4);
        if (fix >= 0 && count <= static_cast<std::size_t>(fix_limit) && pick < 2) {
            this->head(fix | static_cast<int>(count));
        } else if (one >= 0 && count < 256 && pick < 3) {
            this->head(one);
            this->big(count, 1);
        } else if (pick < 3 || four < 0) {
            this->head(two);
            this->big(count, 2);
        } else {
            this->head(four);
            this->big(count, 4);
        }
    }

    void text(const std::string &contents) {
        this->sized(contents.size(), 0xa0, 31, 0xd9, 0xda, 0xdb);
        this->out += contents;
    }

    void key() {
        // Few names, so that maps have duplicates; and now and then a key that is not a str.
        if (this->below(20) == 0) {
            this->scalar();
            return;
        }
        static constexpr const char *names[] = { "", "a", "b", "id", "name" };
        this->text(names[this->below(5)]);
    }

    void scalar() {
        switch (this->below(14)) {
        case 0: this->head(0xc0); break;
        case 1: this->head(this->below(2) == 0 ? 0xc2 : 0xc3); break;
        case 2: this->head(this->below(0x80)); break;
        case 3: this->head(0xe0 + this->below(32)); break;
        case 4: {
            static constexpr int widths[] = { 1, 2, 4, 8 };
            const int which = this->below(4);
            this->head(0xcc + which);
            this->big(this->random(), widths[which]);
            break;
        }
        case 5: {
            static constexpr int widths[] = { 1, 2, 4, 8 };
            const int which = this->below(4);
            this->head(0xd0 + which);
            this->big(this->random(), widths[which]);
            break;
        }
        case 6: this->head(0xca); this->big(this->random(), 4); break;
        case 7: this->head(0xcb); this->big(this->random(), 8); break;
        case 8:
        case 9: {
            std::string contents(static_cast<std::size_t>(this->below(40)), 'x');
            for (auto &letter : contents) letter = static_cast<char>('a' + this->below(26));
            this->text(contents);
            break;
        }
        case 10:
        case 11: {
            const auto size = static_cast<std::size_t>(this->below(6));
            this->sized(size, -1, 0, 0xc4, 0xc5, 0xc6);
            for (std::size_t index = 0; index < size; ++index) this->head(this->below(256));
            break;
        }
        case 12:
            if (this->below(4) == 0) {
                // An extension, which a tree has no place for.
                static constexpr int widths[] = { 1, 2, 4, 8, 16 };
                const int which = this->below(5);
                this->head(0xd4 + which);
                this->head(this->below(256));
                for (int index = 0; index < widths[which]; ++index) this->head(this->below(256));
                break;
            }
            [[fallthrough]];
        default: this->head(0xc0); break;
        }
    }

    void container(int depth) {
        const bool object = this->below(2) == 0;
        const auto count = static_cast<std::size_t>(this->below(depth > 3 ? 3 : 6));
        if (object) this->sized(count, 0x80, 15, -1, 0xde, 0xdf);
        else this->sized(count, 0x90, 15, -1, 0xdc, 0xdd);
        for (std::size_t index = 0; index < count; ++index) {
            if (object) this->key();
            this->any(depth + 1);
        }
    }

public:
    explicit generator(std::uint64_t seed) : random(seed) {}

    void any(int depth) {
        if (depth >= 6 || this->below(5) < 2) this->scalar();
        else this->container(depth);
    }

    std::vector<std::byte> document() {
        this->out.clear();
        this->any(0);
        return bytes_of(this->out);
    }

    /** One value `depth` containers down, the containers in whichever form. */
    std::vector<std::byte> nested(int depth) {
        this->out.clear();
        for (int level = 0; level < depth; ++level) {
            switch (this->below(3)) {
            case 0: this->head(0x91); break;
            case 1: this->head(0xdc); this->big(1, 2); break;
            default: this->head(0x81); this->text("a"); break;
            }
        }
        this->any(5);
        return bytes_of(this->out);
    }
};

void generated() {
    for (std::uint64_t seed = 0; seed < 3'000; ++seed) {
        generator documents { seed };
        const auto document = documents.document();
        agree_refused(document, "generated " + std::to_string(seed));
        if (seed < 50) mutated(document, "generated " + std::to_string(seed));
    }
}

void deep() {
    // To the limit and past it, through every form of container and every shape of value.
    for (std::uint64_t seed = 0; seed < 2'000; ++seed) {
        generator documents { seed };
        const int depth = max_depth - 2 + static_cast<int>(seed % 4);
        agree_refused(documents.nested(depth), "nested " + std::to_string(depth) + " from seed " + std::to_string(seed));
    }
}

/** What the walk read and the builder refuses, and a handle below the root building only its own value. */
void refusals() {
    const auto integer_key = bytes_of("\x82\xa1" "a\x01\x02\x03");
    check(!msgpack::decode<value>(integer_key), "a map with a key that is not a str is refused");
    check(msgpack::validate(integer_key).has_value(), "though it is a valid document");
    check(!msgpack::decode<value>(bytes_of("\x91\xd4\x01\x00")), "an extension is refused");

    const auto document = bytes_of("\x92\x81\xa1" "a\x93\x01\x02\x03\xc3");
    const auto root = msgpack::reader::over(document);
    const auto inner = root[0]["a"].as<value>();
    check(inner && inner->as_array().size() == 3, "a handle builds the value it stands on");
    std::size_t elements = 0;
    for (const auto &element : root.array()) {
        check(element.as<value>().has_value(), "each element builds");
        ++elements;
    }
    check(elements == 2, "and stepping on from a built element finds the next");
}

} // namespace

int main() {
    refusals();
    fixtures();
    generated();
    deep();
    std::printf("msgpack_tree: %d documents compared, %d of them built\n", compared, built);
    check(built > 2'000, "plenty of the documents are whole");
    return report("msgpack_tree");
}
