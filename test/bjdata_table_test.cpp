// Draft 4 Structure-of-Arrays tables, read every way there is: validated and stepped over,
// into a described type record by record, into a value tree, and through reader handles by key
// and by index. The fixtures under fixtures/soa are dart-bjdata's encodings of the JSON beside
// each, so a table read as a tree must equal that JSON read as a tree, and the same table laid
// out by column must read exactly as it does by row.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
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

struct [[= serpent::serializable {}]] named {
    std::string name;
    int n = 0;
    friend bool operator==(const named &, const named &) = default;
};

struct [[= serpent::serializable {}]] holder {
    std::vector<record> table;
    int other = 0;
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

    return report("bjdata_table");
}
