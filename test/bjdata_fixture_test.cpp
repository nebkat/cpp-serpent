// Cross-implementation check against dart-bjdata. For every fixture the C++ view must
// produce the same block notation and decode to the same values as the reference.
// Regenerate with: python3 test/generate_fixtures.py /tmp/bjdatacli

#include "check.hpp"

#include <nonstd/bjdata.hpp>
#include <nonstd/bjdata/json.hpp>
#include <nonstd/json.hpp>
#include <nonstd/bjdata/notation.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef BJDATA_FIXTURE_DIR
#define BJDATA_FIXTURE_DIR "fixtures"
#endif

using namespace nonstd::bjdata;

namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream stream { path, std::ios::binary };
    const std::string text { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> result(text.size());
    std::ranges::transform(text, result.begin(), [](char c) { return static_cast<std::byte>(c); });
    return result;
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream stream { path, std::ios::binary };
    return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
}

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto value : bytes) {
        out += digits[static_cast<unsigned>(value) >> 4];
        out += digits[static_cast<unsigned>(value) & 0xF];
    }
    return out;
}

/** Python's repr for a float, which is what generate_fixtures.py writes. */
std::string repr_real(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    auto text = std::format("{}", value);
    if (text.find_first_of(".ein") == std::string::npos) text += ".0";
    return text;
}

/** Mirrors digest() in generate_fixtures.py. */
std::string digest(const view &value) {
    switch (value.type()) {
        case kind::null:
            return "Z";
        case kind::boolean:
            return value.as_bool() == true ? "T" : "F";
        case kind::integer: {
            if (value.type_marker() == marker::uint64) {
                const auto unsigned_value = value.as_int<unsigned long long>();
                return unsigned_value ? "i:" + std::format("{}", *unsigned_value) : "?";
            }
            const auto signed_value = value.as_int<long long>();
            return signed_value ? "i:" + std::format("{}", *signed_value) : "?";
        }
        case kind::real: {
            const auto real_value = value.as_float<double>();
            return real_value ? "d:" + repr_real(*real_value) : "?";
        }
        case kind::string: {
            const auto text = value.as_string();
            if (!text) return "?";
            return "s" + std::to_string(text->size()) + ":" + std::string { *text };
        }
        case kind::array: {
            std::string out = "[";
            bool first = true;
            for (const auto element : value.array()) {
                if (!std::exchange(first, false)) out += ',';
                out += digest(element);
            }
            return out + "]";
        }
        case kind::object: {
            std::string out = "{";
            bool first = true;
            for (const auto [key, element] : value.items()) {
                if (!std::exchange(first, false)) out += ',';
                out += key;
                out += '=';
                out += digest(element);
            }
            return out + "}";
        }
        default:
            return "?";
    }
}

/**
 * Re-encodes a document through the writer's *value* API rather than by copying markers.
 *
 * This is what makes the comparison meaningful: the reference encoder chose every marker
 * from the value, and the values round-trip, so if our bytes match its bytes then the whole
 * ladder agrees - integer widths, float narrowing, the packing measurement, container shapes
 * and key encoding. High precision is the one value that must be re-emitted by marker, since
 * H reads back as an ordinary string.
 */
void reencode(writer &out, view source) {
    switch (source.type()) {
        case kind::null:
            out.null();
            return;
        case kind::boolean:
            out.value(source.as_bool() == true);
            return;
        case kind::integer:
            out.value(source.as_int<std::int64_t>().value_or(0));
            return;
        case kind::real:
            out.value(source.as_float<double>().value_or(0.0));
            return;
        case kind::string: {
            const auto text = source.as_string().value_or("");
            if (source.type_marker() == marker::high_precision) out.high_precision(text);
            else if (source.type_marker() == marker::character) out.character(text.empty() ? '\0' : text.front());
            else out.value(text);
            return;
        }
        case kind::array: {
            bool any = false;
            bool all_integer = true;
            bool all_real = true;
            for (const auto element : source.array()) {
                any = true;
                all_integer = all_integer && element.type() == kind::integer;
                all_real = all_real && element.type() == kind::real;
            }
            if (any && all_integer) {
                std::vector<std::int64_t> values;
                for (const auto element : source.array()) values.push_back(element.as_int<std::int64_t>().value_or(0));
                out.value(values);
                return;
            }
            if (any && all_real) {
                std::vector<double> values;
                for (const auto element : source.array()) values.push_back(element.as_float<double>().value_or(0.0));
                out.value(values);
                return;
            }
            const auto scope = out.array();
            for (const auto element : source.array()) reencode(out, element);
            return;
        }
        case kind::object: {
            const auto scope = out.object();
            for (const auto [key, element] : source.items()) {
                out.key(key);
                reencode(out, element);
            }
            return;
        }
        default:
            out.fail(errc::type_mismatch);
            return;
    }
}

/** The same canonical rendering as digest(), over the JSON reader instead of the view. */
std::string json_digest(const json_reader &source) {
    switch (source.type()) {
        case kind::null:
            return "Z";
        case kind::boolean:
            return source.as_bool() == true ? "T" : "F";
        case kind::integer: {
            const auto signed_value = source.as_int<long long>();
            if (signed_value) return "i:" + std::format("{}", *signed_value);
            const auto unsigned_value = source.as_int<unsigned long long>();
            return unsigned_value ? "i:" + std::format("{}", *unsigned_value) : "?";
        }
        case kind::real: {
            const auto real_value = source.as_float<double>();
            return real_value ? "d:" + repr_real(*real_value) : "?";
        }
        case kind::string: {
            const auto text = source.as_string();
            if (!text) return "?";
            return "s" + std::to_string(text->size()) + ":" + *text;
        }
        case kind::array: {
            std::string out = "[";
            bool first = true;
            for (const auto element : source.array()) {
                if (!std::exchange(first, false)) out += ',';
                out += json_digest(element);
            }
            return out + "]";
        }
        case kind::object: {
            std::string out = "{";
            bool first = true;
            for (const auto entry : source.items()) {
                if (!std::exchange(first, false)) out += ',';
                out += entry.key_string();
                out += '=';
                out += json_digest(entry.value);
            }
            return out + "}";
        }
        default:
            return "?";
    }
}

}// namespace

int main(int argc, char **argv) {
    const std::filesystem::path directory = argc > 1 ? argv[1] : BJDATA_FIXTURE_DIR;

    if (!std::filesystem::is_directory(directory)) {
        std::printf("bjdata_fixture: no fixtures at %s\n", directory.c_str());
        return 1;
    }

    std::vector<std::filesystem::path> documents;
    for (const auto &entry : std::filesystem::directory_iterator { directory }) {
        if (entry.path().extension() == ".bjd") documents.push_back(entry.path());
    }
    std::ranges::sort(documents);
    check(!documents.empty(), "fixtures directory is not empty");

    for (const auto &document : documents) {
        const auto name = document.stem().string();
        const auto bytes = read_bytes(document);

        const auto validated = validate(bytes);
        check(validated.has_value(), name + ": validates");
        if (!validated) {
            std::printf("    %s at offset %zu\n", validated.error().what(), validated.error().offset());
            continue;
        }

        check_equal(std::string_view { block_notation(bytes) },
                    std::string_view { read_text(directory / (name + ".blocks")) },
                    name + ": block notation matches dart-bjdata");

        check_equal(std::string_view { digest(view::over(bytes)) },
                    std::string_view { read_text(directory / (name + ".digest")) },
                    name + ": decodes to the same values as dart-bjdata");

        // The headline check: re-encoding the decoded values must reproduce dart's bytes.
        {
            std::vector<std::byte> produced;
            container_sink out { produced };
            writer target { out };
            reencode(target, view::over(bytes));
            const auto finished = target.finish();
            check(finished.has_value(), name + ": re-encodes cleanly");
            check_equal(std::string_view { hex(produced) }, std::string_view { hex(bytes) },
                        name + ": re-encodes to the same bytes as dart-bjdata");
        }

        // JSON output is checked against the reference's own JSON rendering of the same
        // bytes, so the number formatting, key order, escaping and indentation all have to
        // agree - not just the structure.
        check_equal(std::string_view { to_json(view::over(bytes), { .indent = 2 }) },
                    std::string_view { read_text(directory / (name + ".json.expected")) },
                    name + ": JSON matches dart-bjdata");

        // And read back: parsing dart-bjdata's own JSON must produce the same values the
        // BJData reader produces from the same document.
        {
            const auto text = read_text(directory / (name + ".json.expected"));
            const auto parsed = validate_json(text);
            check(parsed.has_value(), name + ": dart's JSON validates");
            if (parsed) {
                check_equal(std::string_view { json_digest(json_reader::over(text)) },
                            std::string_view { read_text(directory / (name + ".digest")) },
                            name + ": the JSON reader agrees with the BJData reader");
            }
        }

        // Splicing copies the value verbatim: its marker, then its payload, no re-encoding.
        {
            std::vector<std::byte> spliced;
            container_sink out { spliced };
            writer target { out };
            write_value(target, view::over(bytes));
            check(target.finish().has_value(), name + ": splices cleanly");
            check_equal(std::string_view { hex(spliced) }, std::string_view { hex(bytes) },
                        name + ": splices to identical bytes");
        }

        // Every truncation of a reference document must still fail safely.
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            const auto prefix = std::span { bytes }.first(length);
            check(!validate(prefix).has_value(), name + ": truncation is rejected");
            (void) digest(view::over(prefix));
            (void) block_notation(prefix);
        }
    }

    std::printf("bjdata_fixture: %zu documents\n", documents.size());
    return report("bjdata_fixture");
}
