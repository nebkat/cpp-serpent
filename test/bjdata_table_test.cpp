// Draft 4 Structure-of-Arrays tables, read every way there is: validated and stepped over,
// into a described type record by record, into a value tree, and through reader handles by key
// and by index. The fixtures under fixtures/soa are dart-bjdata's encodings of the JSON beside
// each, so a table read as a tree must equal that JSON read as a tree, and the same table laid
// out by column must read exactly as it does by row.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;
using serpent::value;

namespace {

struct [[= serpent::serializable {}]] position {
    double lat = 0;
    double lon = 0;
    friend bool operator==(const position &, const position &) = default;
};

struct [[= serpent::serializable {}]] record {
    std::uint32_t at = 0;
    position pos;
    std::array<float, 3> v {};
    std::string station;
    bool flag = false;
    std::optional<int> nothing;
    std::string note;
    friend bool operator==(const record &, const record &) = default;
};

struct [[= serpent::serializable {}]] table_row {
    std::uint32_t at = 0;
    position pos;
    std::array<float, 3> v {};
    std::string station;
    bool flag = false;
    std::string note;
    friend bool operator==(const table_row &, const table_row &) = default;
};

struct [[= serpent::serializable {}]] named {
    std::string name;
    int n = 0;
    friend bool operator==(const named &, const named &) = default;
};

struct [[= serpent::serializable {}]] holder {
    std::vector<record> table;
    int other = 0;
};

enum class [[= serpent::serializable {}]] mode { idle, active, fault [[= serpent::fallback {}]] };
enum class plain_enum { one = 1, two = 2 };

struct [[= serpent::serializable {}]] with_enums {
    std::uint16_t at = 0;
    mode state = mode::idle;
    plain_enum kind = plain_enum::one;
    char tag = 'x';
    std::array<std::array<std::int8_t, 2>, 2> cells {};
    friend bool operator==(const with_enums &, const with_enums &) = default;
};

struct [[= serpent::serializable {}]] not_a_record {
    int a = 0;
    std::optional<int> maybe;
};

struct [[= serpent::serializable {}]] nor_this {
    int a = 0;
    std::vector<int> grows;
};

struct [[= serpent::serializable {}]] narrower {
    std::uint8_t at = 0; ///< the table's `at` is a uint32 and does not fit
};

struct [[= serpent::serializable {}]] demanding {
    std::uint32_t at = 0;
    int absent = 0; ///< no such field, and not defaulted
};

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream in { path, std::ios::binary };
    std::stringstream buffer;
    buffer << in.rdbuf();
    const auto text = buffer.str();
    return { reinterpret_cast<const std::byte *>(text.data()), reinterpret_cast<const std::byte *>(text.data() + text.size()) };
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream in { path };
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const std::filesystem::path fixtures = std::filesystem::path { SERPENT_FIXTURE_DIR } / "soa";

} // namespace

int main() {
    // Every fixture: well formed, and the same tree as its JSON.
    int seen = 0;
    for (const auto &entry : std::filesystem::directory_iterator { fixtures }) {
        if (entry.path().extension() != ".bjd") continue;
        ++seen;
        const auto bytes = read_file(entry.path());
        const auto name = entry.path().stem().string();
        check(bjdata::validate(bytes).has_value(), name + " validates");
        const auto tree = bjdata::decode<value>(bytes);
        auto expected = json::decode<value>(read_text(std::filesystem::path { entry.path() }.replace_extension(".json")));
        check(tree.has_value() && expected.has_value(), name + " reads as a tree");
        if (tree && expected) check(*tree == *expected, name + " is the tree its JSON is");
    }
    check_equal(seen, 5, "all five fixtures were tried");

    const auto rows = read_file(fixtures / "records.bjd");
    const auto columns = read_file(fixtures / "records_columns.bjd");

    // Into a described type, from either layout.
    const auto by_row = bjdata::decode<std::vector<record>>(rows);
    const auto by_column = bjdata::decode<std::vector<record>>(columns);
    check(by_row.has_value() && by_row->size() == 40, "records read into a type");
    check(by_column.has_value() && by_row == by_column, "and the same by column as by row");
    if (by_row) {
        const auto &first = by_row->front();
        const auto &last = by_row->back();
        check(first.at == 1700000000 && last.at == 1700000039, "a widened integer");
        check(first.pos.lat == 51.5 && last.pos.lon == -0.1, "a nested record");
        check(last.v == std::array<float, 3> { 19.5f, 1.0f, 2.0f }, "a fixed run of float16");
        check(first.station == "station-0" && last.station == "station-39", "a dictionary string");
        check(first.flag && !last.flag, "a boolean byte");
        check(!first.nothing.has_value(), "a null field leaves an optional empty");
        check(first.note == "n", "a character as a string");
    }

    // A field that does not fit, or a required member the schema does not have, refuses the read.
    check(!bjdata::decode<std::vector<narrower>>(rows).has_value(), "a value that does not fit its member refuses");
    check(!bjdata::decode<std::vector<demanding>>(rows).has_value(), "a required member without a field refuses");

    // Offset-table text, and a table as a member of a type.
    const auto offsets = bjdata::decode<std::vector<named>>(read_file(fixtures / "offsets.bjd"));
    check(offsets.has_value() && *offsets == std::vector<named> { { "abc", 5 }, { "", 6 }, { "defgh", 7 } }, "offset-table text");
    const auto held = bjdata::decode<holder>(read_file(fixtures / "nested_table.bjd"));
    check(held.has_value() && held->table.size() == 3 && held->other == 5, "a table inside an object, and the member after it");

    // Through handles: by index and by key, into nested records and runs, in either layout.
    for (const auto *bytes : { &rows, &columns }) {
        const auto doc = bjdata::reader::over(*bytes);
        check(doc.is_array() && doc.size() == 40, "a table is an array of records");
        check(doc[1]["station"].as<std::string>() == "station-1", "a record by index, a field by key");
        check(doc[2]["pos"]["lat"].as<double>() == 51.502, "into a nested record");
        check(doc[0]["v"][2].as<double>() == 2.0 && doc[0]["v"].size() == 3, "into a fixed run");
        check(doc[39]["flag"].is_boolean() && doc[39]["nothing"].is_null(), "a boolean and a null keep their kinds");
        check(!doc[40].is_valid() && !doc[0]["missing"].is_valid(), "past the end, and a key that is not there");
        std::size_t records = 0;
        for (const auto &row : doc.array()) {
            std::size_t fields = 0;
            for ([[maybe_unused]] const auto &member : row.items()) ++fields;
            check_equal(fields, std::size_t { 7 }, "every record has the schema's fields");
            ++records;
        }
        check_equal(records, std::size_t { 40 }, "walked to the end");
        const auto as_record = doc[3].as<record>();
        check(as_record.has_value() && as_record->at == 1700000003, "a record handle reads as the type");
    }

    // Malformed tables are refused by validate(), and so by try_decode(), which validates first.
    const auto refuse = [](std::vector<std::byte> bytes, const char *what) {
        check(!bjdata::validate(bytes).has_value(), what);
        check(!bjdata::try_decode<value>(bytes).has_value(), what);
    };
    auto cut = rows;
    cut.resize(cut.size() - 1);
    refuse(cut, "a table short of its last byte");
    auto bad_type = rows;
    bad_type[7] = std::byte { '?' }; // the marker after "at"
    refuse(bad_type, "a schema type that is not one");
    auto no_count = rows;
    for (std::size_t index = 0; index + 1 < no_count.size(); ++index) {
        if (no_count[index] == std::byte { '}' } && no_count[index + 1] == std::byte { '#' }) {
            no_count[index + 1] = std::byte { 'U' };
            break;
        }
    }
    refuse(no_count, "a schema not followed by a count");
    auto offsets_bad = read_file(fixtures / "offsets.bjd");
    offsets_bad[offsets_bad.size() - 9] = std::byte { 200 }; // the last offset, past the text
    refuse(offsets_bad, "an offset past the text");
    auto index_bad = read_file(fixtures / "mixed.bjd");
    index_bad[index_bad.size() - 1] = std::byte { 9 }; // a dictionary index past its entries
    check(bjdata::validate(index_bad).has_value(), "an index past the dictionary is well formed");
    check(!bjdata::decode<value>(index_bad).has_value(), "but does not read");

    // ---------------- writing ----------------

    // `record` has an optional member, so it is not a table record: what dart-bjdata inferred
    // from data, a type cannot promise. Without that member the same records are a table that
    // reads back as themselves and as the tree dart-bjdata's table is.
    const std::vector<record> forty = *by_row;
    check(bjdata::encode(forty)[1] != std::byte { '$' }, "a type with an optional member writes an array of objects");
    std::vector<table_row> rows_out;
    for (const auto &row : forty) rows_out.push_back({ row.at, row.pos, row.v, row.station, row.flag, row.note });
    const auto written = bjdata::encode(rows_out);
    check(bjdata::validate(written).has_value(), "a written table validates");
    check(written[0] == std::byte { '[' } && written[1] == std::byte { '$' } && written[2] == std::byte { '{' }, "and is a table");
    check(bjdata::decode<std::vector<table_row>>(written) == rows_out, "and reads back as the records");
    const auto ours = bjdata::decode<value>(written);
    auto theirs = bjdata::decode<value>(rows);
    for (auto &row : theirs->as_writable_array()) row.erase_member("nothing");
    check(ours && *ours == *theirs, "and as the same tree as dart-bjdata's, less the null column");
    check(written.size() < bjdata::encode(rows_out, { .tables = false }).size() / 2, "at less than half the size of the array of objects");
    check(bjdata::measure(rows_out) == written.size(), "measure() agrees");

    // Text columns: single characters as C, few distinct values as a dictionary, many as an
    // offset table with repeats and empties intact.
    std::vector<named> many;
    for (int index = 0; index < 300; ++index) many.push_back({ "name-" + std::to_string(index), index });
    many[5].name = "";
    many[7].name = many[3].name;
    const auto offsets_written = bjdata::encode(many);
    check(std::string_view { reinterpret_cast<const char *>(offsets_written.data()), 16 }.find("[$u]") != std::string_view::npos,
            "three hundred distinct names are an offset table");
    check(bjdata::decode<std::vector<named>>(offsets_written) == many, "which reads back, repeats and empties included");
    std::vector<named> few;
    for (int index = 0; index < 300; ++index) few.push_back({ index % 2 ? "odd" : "even", index });
    const auto dictionary_written = bjdata::encode(few);
    check(std::string_view { reinterpret_cast<const char *>(dictionary_written.data()), 32 }.find("[$S#U\x02" "U\x04" "evenU\x03" "odd") != std::string_view::npos,
            "two distinct names are a dictionary in order of first use");
    check(bjdata::decode<std::vector<named>>(dictionary_written) == few, "which reads back");

    // Enumerations: a mapped one is a dictionary of its forms with the fallback taking the rest,
    // a plain one its underlying number; and runs of runs.
    std::vector<with_enums> enums;
    for (int index = 0; index < 6; ++index)
        enums.push_back({ static_cast<std::uint16_t>(index), mode(index % 3), plain_enum(1 + index % 2), char('a' + index), { { { std::int8_t(index), std::int8_t(-index) }, { 7, 8 } } } });
    const auto enums_written = bjdata::encode(enums);
    check(bjdata::validate(enums_written).has_value(), "a table with enumerations validates");
    check(bjdata::decode<std::vector<with_enums>>(enums_written) == enums, "and reads back");
    const auto enums_tree = bjdata::decode<value>(enums_written);
    check(enums_tree && enums_tree->at(1).at("state").as<std::string_view>() == "active"
                    && enums_tree->at(1).at("kind").as<int>() == 2 && enums_tree->at(1).at("cells").at(0).at(1).as<int>() == -1,
            "a mapped enumeration is its name, a plain one its number");
    auto beyond = enums;
    beyond[0].state = static_cast<mode>(9);
    const auto fallback = bjdata::decode<std::vector<with_enums>>(bjdata::encode(beyond));
    check(fallback && fallback->at(0).state == mode::fault, "a value that is no enumerator writes as the fallback");

    // Not tables: one record, a type with an optional or a growable member, and tables off.
    check(bjdata::encode(std::vector<record> { forty[0] })[1] != std::byte { '$' }, "one record is an array of objects");
    check(bjdata::encode(std::vector<not_a_record>(3))[1] != std::byte { '$' }, "an optional member makes a type no record");
    check(bjdata::encode(std::vector<nor_this>(3))[1] != std::byte { '$' }, "so does a growable one");
    check(bjdata::encode(forty, { .tables = false })[1] != std::byte { '$' }, "and tables can be turned off");
    check(bjdata::decode<std::vector<record>>(bjdata::encode(forty, { .tables = false })) == forty, "reading the plain form still");

    // A fixed buffer: exactly the table's size succeeds, every shorter length latches overflow
    // and holds the start of the document.
    {
        const auto whole = bjdata::encode(std::vector<record> { forty[0], forty[1], forty[2] });
        for (std::size_t capacity = 0; capacity <= whole.size(); ++capacity) {
            std::vector<std::byte> storage(capacity);
            serpent::span_sink out { storage };
            bjdata::writer target { out };
            target.value(std::vector<record> { forty[0], forty[1], forty[2] });
            const bool finished = target.finish().has_value();
            if (capacity == whole.size()) {
                check(finished && !out.overflowed() && std::ranges::equal(out.written(), whole), "an exactly sized buffer holds the table");
            } else {
                check(!finished && out.overflowed(), "a short buffer overflows");
                check(std::ranges::equal(out.written(), std::span { whole }.first(out.written().size())), "holding the start of the table");
            }
        }
    }

    return report("bjdata_table");
}
