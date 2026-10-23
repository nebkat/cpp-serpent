// Block notation. Expected strings are the ones the reference implementation documents.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/bjdata/notation.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace serpent;
using namespace serpent::bjdata;

namespace {

void notated(std::string_view hex, std::string_view expected, std::string_view what) {
    const auto bytes = from_hex(hex);
    check_equal(std::string_view { block_notation(bytes) }, expected, what);
}

void documented() {
    notated("5a", "[Z]", "null");
    notated("54", "[T]", "true");
    notated("46", "[F]", "false");
    notated("552a", "[U][42]", "42");
    notated("441f85eb51b81e0940", "[D][3.14]", "3.14");
    notated("53550d48656c6c6f2c20776f726c6421", "[S][U][13][Hello, world!]", "Hello, world!");
    notated("5b5501550255035d", "[[][U][1][U][2][U][3][]]", "[1,2,3]");
    notated("7b5503666f6f5501550362617255027d", "[{][U][3][foo][U][1][U][3][bar][U][2][}]", "{foo:1,bar:2}");
}

void containers() {
    notated("5b5d", "[[][]]", "empty array");
    notated("7b7d", "[{][}]", "empty object");
    notated("5b235503550155025503", "[[][#][U][3][U][1][U][2][U][3]", "counted array has no terminator");
    notated("5b2455235503010203", "[[][$][U][#][U][3][1][2][3]", "typed array elements carry no markers");
    notated("5b2455235b550255035d010203040506", "[[][$][U][#][[][U][2][U][3][]][1][2][3][4][5][6]",
            "dimension array count");
    notated("5b2455235b5b550255035d5d010203040506", "[[][$][U][#][[][[][U][2][U][3][]][]][1][2][3][4][5][6]",
            "column-major dimension array");
    notated("5b2455235b24552355020203010203040506", "[[][$][U][#][[][$][U][#][U][2][2][3][1][2][3][4][5][6]",
            "optimized dimension array");
    notated("5b23550355014e55025503", "[[][#][U][3][U][1][N][U][2][U][3]", "noops are shown where they occur");
    notated("68003c", "[h][1.0]", "float16 renders as a whole double");
    notated("4361", "[C][a]", "char");
    notated("4855022d31", "[H][U][2][-1]", "high precision keeps its digits");
    notated("5b2442235504deadbeef", "[[][$][B][#][U][4][222][173][190][239]", "binary elements are decimal");
}

// Draft 4 tables: the schema token by token, then each record's fields without markers, in
// the order they lie. Checked against dart-bjdata's notation of the same documents, which it
// writes, whitespace stripped, beside them; offsets.bjd it cannot write, so that one is here.
void tables() {
    const std::filesystem::path fixtures = std::filesystem::path { SERPENT_FIXTURE_DIR } / "soa";
    const auto contents = [&](const std::string &name) {
        std::ifstream stream { fixtures / name, std::ios::binary };
        return std::string { std::istreambuf_iterator<char> { stream }, {} };
    };
    for (const std::string name : { "records", "records_columns", "mixed", "nested_table" }) {
        const auto bytes = contents(name + ".bjd");
        const auto notation = block_notation({ reinterpret_cast<const std::byte *>(bytes.data()), bytes.size() });
        check_equal(std::string_view { notation }, std::string_view { contents(name + ".blocks") }, name + " is the reference's notation");
    }

    const auto offsets = contents("offsets.bjd");
    check_equal(std::string_view { block_notation({ reinterpret_cast<const std::byte *>(offsets.data()), offsets.size() }) },
            std::string_view { "[[][$][{][U][4][name][[][$][U][]][U][1][n][U][}][#][U][3][0][5][1][6][2][7][0][3][3][8][abcdefgh]" },
            "an offset field's index a record, then its offsets and its text");

    // A table runs to the end of its text, so whatever follows it is notated as itself.
    notated("5b5b247b550161557d235502010255035d", "[[][[][$][{][U][1][a][U][}][#][U][2][1][2][U][3][]]", "a value after a table");
}

} // namespace

int main() {
    documented();
    containers();
    tables();
    return report("bjdata_notation");
}
