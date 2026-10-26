// The BJData tree builder against the walk it replaces: every document - each fixture, every
// prefix of one, each with any byte changed, and generated ones of every shape - builds into the
// same tree both ways or fails both ways. The walk is kept here as it was, a reader asked what
// each value is and then what it holds, a call deeper for every level, so it can be the
// reference for as long as the builder exists.

#include "check.hpp"

#include <serpent/bjdata.hpp>
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
bool walked(const bjdata::reader &source, value &item) {
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
        for (const auto &entry : source.items()) {
            value held;
            if (!walked(entry.value, held)) return false;
            built.append_member(entry.key_string(), std::move(held));
        }
        built.coalesce_members();
        item = std::move(built);
        return true;
    }
    }
    return false;
}

/** Decoded the old way: walked, then held to ending where the buffer does, as decode() holds it. */
std::optional<value> decoded_by_walking(std::span<const std::byte> buffer) {
    const auto root = bjdata::reader::over(buffer);
    value tree;
    if (!walked(root, tree) || !bjdata::detail::ends_with_buffer(root, buffer)) return std::nullopt;
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
    const auto tree = bjdata::decode<value>(buffer);
    if (reference.has_value() != tree.has_value()) {
        check(false, what + (tree ? ": builds, but the walk refuses it" : ": the walk reads it, but it does not build"));
        return;
    }
    if (!tree) return;
    ++built;
    check(same(*reference, *tree), what + ": builds a different tree");

    // try_decode builds the same, or fails as validate() says the document is broken.
    const auto tried = bjdata::try_decode<value>(buffer);
    check(tried.has_value() && same(*tried, *tree), what + ": try_decode builds it too");
}

void agree_refused(std::span<const std::byte> buffer, const std::string &what) {
    agree(buffer, what);
    const auto tried = bjdata::try_decode<value>(buffer);
    const auto checked = bjdata::validate(buffer);
    if (!tried && !checked) check(tried.error().code() == checked.error().code(), what + ": try_decode fails as validate does");
}

std::vector<std::byte> bytes_of(const std::string &text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream in { path, std::ios::binary };
    const std::string text { std::istreambuf_iterator<char> { in }, {} };
    return bytes_of(text);
}

/**
 * Each document whole, cut short at every length, and with every byte in turn changed - every
 * one of a small document, and spread over a large one, whose tables the reference walk is slow
 * to read.
 */
void mutated(const std::vector<std::byte> &document, const std::string &name) {
    const std::size_t stride = document.size() <= 256 ? 1 : document.size() / 16 + 1;
    agree_refused(document, name);
    for (std::size_t length = 0; length < document.size(); length += stride)
        agree_refused(std::span { document }.first(length), name + " cut to " + std::to_string(length));

    // Bytes that change what a document means: the structural markers, a length's widths, and
    // values that make counts huge, negative or zero.
    static constexpr unsigned char replacements[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 'N', 'Z', 'T', 'U', 'i', 'l', 'L', 'M', 'd', 'C',
        'S', 'H', 'B', 'E', '[', ']', '{', '}', '$', '#' };
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

void fixtures() {
    for (const auto &entry : std::filesystem::recursive_directory_iterator { SERPENT_FIXTURE_DIR }) {
        if (entry.path().extension() != ".bjd") continue;
        mutated(read_file(entry.path()), entry.path().filename().string());
    }
}

/** Documents of every shape the format has, made at random from a seed. */
class generator {
    std::mt19937_64 random;
    std::string out;

    int below(int bound) { return static_cast<int>(this->random() % static_cast<std::uint64_t>(bound)); }

    void raw(std::uint64_t bits, int width) {
        for (int index = 0; index < width; ++index) this->out.push_back(static_cast<char>(bits >> (8 * index)));
    }

    void length(std::size_t count) {
        // Whichever width the writer would not have chosen, now and then.
        if (count < 256 && this->below(4) != 0) {
            this->out += 'U';
            this->raw(count, 1);
        } else {
            this->out += 'l';
            this->raw(count, 4);
        }
    }

    void noops() {
        while (this->below(8) == 0) this->out += 'N';
    }

    void key() {
        // Few names, so that objects have duplicates.
        static constexpr const char *names[] = { "", "a", "b", "id", "name" };
        const std::string name = names[this->below(5)];
        this->length(name.size());
        this->out += name;
    }

    void scalar() {
        static constexpr char markers[] = { 'Z', 'T', 'F', 'U', 'i', 'u', 'I', 'm', 'l', 'M', 'L', 'h', 'd', 'D', 'C', 'B', 'S', 'H' };
        const char marker = markers[this->below(sizeof markers)];
        this->out += marker;
        switch (marker) {
        case 'U': case 'i': case 'C': case 'B': this->raw(this->random(), 1); break;
        case 'u': case 'I': case 'h': this->raw(this->random(), 2); break;
        case 'm': case 'l': case 'd': this->raw(this->random(), 4); break;
        case 'M': case 'L': case 'D': this->raw(this->random(), 8); break;
        case 'S': case 'H': {
            const auto size = static_cast<std::size_t>(this->below(6));
            this->length(size);
            for (std::size_t index = 0; index < size; ++index) this->out += static_cast<char>('a' + this->below(26));
            break;
        }
        default: break;
        }
    }

    void typed(bool object) {
        static constexpr char types[] = { 'U', 'i', 'u', 'I', 'm', 'l', 'M', 'L', 'h', 'd', 'D', 'C', 'B' };
        const char type = types[this->below(sizeof types)];
        const int width = type == 'u' || type == 'I' || type == 'h' ? 2 : type == 'm' || type == 'l' || type == 'd' ? 4
                : type == 'M' || type == 'L' || type == 'D' ? 8 : 1;
        this->out += object ? '{' : '[';
        this->out += '$';
        this->out += type;
        this->out += '#';
        std::size_t count = static_cast<std::size_t>(this->below(5));
        if (!object && this->below(4) == 0) {
            // A shape rather than a count: the elements of an n-dimensional array, in order.
            const int first = this->below(3) + 1;
            const int second = this->below(3);
            this->out += "[U";
            this->raw(static_cast<std::uint64_t>(first), 1);
            this->out += 'U';
            this->raw(static_cast<std::uint64_t>(second), 1);
            this->out += ']';
            count = static_cast<std::size_t>(first * second);
        } else {
            this->length(count);
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (object) {
                this->noops();
                this->key();
            }
            this->raw(this->random(), width);
        }
    }

    void container(int depth) {
        const bool object = this->below(2) == 0;
        const bool counted = this->below(2) == 0;
        const auto count = static_cast<std::size_t>(this->below(depth > 3 ? 2 : 5));
        this->out += object ? '{' : '[';
        if (counted) {
            this->out += '#';
            this->length(count);
        }
        for (std::size_t index = 0; index < count; ++index) {
            this->noops();
            if (object) this->key();
            this->any(depth + 1);
        }
        if (!counted) {
            this->noops();
            this->out += object ? '}' : ']';
        }
    }

    /** A field of a table's schema, and what each record holds in it. */
    struct field {
        char kind = 'Z';   ///< a strong type's marker, T, Z, S, # for a dictionary, O for offset text, { or [
        int width = 0;     ///< the bytes a record spends on it, children included
        char index = 'U';  ///< an offset field's index type
        std::string name;
        std::vector<field> children;
        int entries = 0;
        std::vector<std::string> texts; ///< an offset field's text for each record
    };

    static int width_of(char type) {
        return type == 'u' || type == 'I' || type == 'h' ? 2 : type == 'm' || type == 'l' || type == 'd' ? 4
                : type == 'M' || type == 'L' || type == 'D' ? 8 : 1;
    }

    field table_field(int depth, std::size_t records, std::string &schema) {
        static constexpr char numbers[] = { 'U', 'i', 'u', 'I', 'm', 'l', 'M', 'L', 'h', 'd', 'D', 'C', 'B' };
        field made;
        const int pick = this->below(depth < 2 ? 9 : 7);
        switch (pick) {
        case 0: case 1:
            made.kind = numbers[this->below(sizeof numbers)];
            made.width = width_of(made.kind);
            schema += made.kind;
            break;
        case 2: made.kind = 'T'; made.width = 1; schema += 'T'; break;
        case 3: made.kind = 'Z'; schema += 'Z'; break;
        case 4:
            made.kind = 'S';
            made.width = this->below(5);
            schema += 'S';
            this->out.swap(schema);
            this->length(static_cast<std::size_t>(made.width));
            this->out.swap(schema);
            break;
        case 5: {
            made.kind = '#';
            made.width = 1;
            made.entries = this->below(3) + 1;
            schema += "[$S#";
            this->out.swap(schema);
            this->length(static_cast<std::size_t>(made.entries));
            for (int entry = 0; entry < made.entries; ++entry) {
                const std::string text = "e" + std::to_string(this->below(100));
                this->length(text.size());
                this->out += text;
            }
            this->out.swap(schema);
            break;
        }
        case 6:
            made.kind = 'O';
            made.index = this->below(2) == 0 ? 'U' : 'I';
            made.width = width_of(made.index);
            schema += "[$";
            schema += made.index;
            schema += ']';
            for (std::size_t record = 0; record < records; ++record) made.texts.push_back(std::string(static_cast<std::size_t>(this->below(4)), 'o'));
            break;
        default: {
            made.kind = pick == 7 ? '{' : '[';
            schema += made.kind;
            const int count = this->below(3) + 1;
            for (int child = 0; child < count; ++child) {
                std::string name;
                if (made.kind == '{') {
                    static constexpr const char *names[] = { "a", "b", "c" };
                    name = names[this->below(3)];
                    this->out.swap(schema);
                    this->length(name.size());
                    this->out += name;
                    this->out.swap(schema);
                }
                auto &added = made.children.emplace_back(this->table_field(depth + 1, records, schema));
                added.name = name;
                made.width += added.width;
            }
            schema += made.kind == '{' ? '}' : ']';
            break;
        }
        }
        return made;
    }

    /** One record's bytes for a field. */
    void table_value(const field &of, std::size_t record) {
        switch (of.kind) {
        case 'T': this->out += this->below(2) == 0 ? 'T' : 'F'; break;
        case 'Z': break;
        case 'S': {
            const int used = of.width == 0 ? 0 : this->below(of.width + 1);
            for (int index = 0; index < of.width; ++index) this->out += index < used ? static_cast<char>('a' + this->below(26)) : '\0';
            break;
        }
        case '#': this->raw(static_cast<std::uint64_t>(this->below(of.entries)), 1); break;
        case 'O': this->raw(record, of.width); break;
        case '{':
        case '[':
            for (const auto &child : of.children) this->table_value(child, record);
            break;
        default: this->raw(this->random(), of.width); break;
        }
    }

    static void offset_fields(const std::vector<field> &fields, std::vector<const field *> &into) {
        for (const auto &one : fields) {
            if (one.kind == 'O') into.push_back(&one);
            offset_fields(one.children, into);
        }
    }

    void table() {
        const bool by_column = this->below(2) == 0;
        const auto records = static_cast<std::size_t>(this->below(4));
        std::string schema;
        std::vector<field> fields;
        const int count = this->below(4) + 1;
        for (int index = 0; index < count; ++index) {
            static constexpr const char *names[] = { "x", "y", "id", "x" };
            const std::string name = names[this->below(4)];
            this->out.swap(schema);
            this->length(name.size());
            this->out += name;
            this->out.swap(schema);
            fields.push_back(this->table_field(0, records, schema));
            fields.back().name = name;
        }
        this->out += by_column ? "{${" : "[${";
        this->out += schema;
        this->out += "}#";
        this->length(records);
        if (by_column) {
            for (const auto &one : fields)
                for (std::size_t record = 0; record < records; ++record) this->table_value(one, record);
        } else {
            for (std::size_t record = 0; record < records; ++record)
                for (const auto &one : fields) this->table_value(one, record);
        }
        // Each offset field's table and text after the payload, in schema order.
        std::vector<const field *> offsets;
        offset_fields(fields, offsets);
        for (const auto *one : offsets) {
            std::uint64_t at = 0;
            this->raw(0, one->width);
            for (const auto &text : one->texts) {
                at += text.size();
                this->raw(at, one->width);
            }
            for (const auto &text : one->texts) this->out += text;
        }
    }

public:
    explicit generator(std::uint64_t seed) : random(seed) {}

    void any(int depth) {
        const int pick = this->below(12);
        if (pick >= 10) this->table();
        else if (depth >= 6 || pick < 4) this->scalar();
        else if (pick < 6) this->typed(this->below(3) == 0);
        else this->container(depth);
    }

    std::vector<std::byte> document() {
        this->out.clear();
        this->any(0);
        return bytes_of(this->out);
    }

    /** One value `depth` arrays down, the arrays in whichever form. */
    std::vector<std::byte> nested(int depth) {
        this->out.clear();
        std::string closing;
        for (int level = 0; level < depth; ++level) {
            switch (this->below(3)) {
            case 0: this->out += '['; closing.insert(closing.begin(), ']'); break;
            case 1: this->out += "[#U\x01"; break;
            default: this->out += "{#U\x01U\x01" "a"; break;
            }
        }
        this->any(5);
        this->out += closing;
        return bytes_of(this->out);
    }
};

void generated() {
    for (std::uint64_t seed = 0; seed < 2'000; ++seed) {
        generator documents { seed };
        const auto document = documents.document();
        agree_refused(document, "generated " + std::to_string(seed));
        if (seed < 50) mutated(document, "generated " + std::to_string(seed));
    }
}

void deep() {
    // To the limit and past it, through every form of container and every shape of value, so
    // the two agree on where max_depth is for elements with markers and without.
    for (std::uint64_t seed = 0; seed < 2'000; ++seed) {
        generator documents { seed };
        const int depth = max_depth - 2 + static_cast<int>(seed % 4);
        agree_refused(documents.nested(depth), "nested " + std::to_string(depth) + " from seed " + std::to_string(seed));
    }
}

} // namespace

int main() {
    fixtures();
    generated();
    deep();
    std::printf("bjdata_tree: %d documents compared, %d of them built\n", compared, built);
    check(built > 2'000, "plenty of the documents are whole");
    return report("bjdata_tree");
}
