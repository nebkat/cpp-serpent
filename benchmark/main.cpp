// serpent against a DOM library, on the corpus the published C++ JSON benchmarks use.
//
// The two are not the same shape, and the numbers only mean something with that in mind. A DOM
// library parses a document into a mutable object graph you can then index, mutate and re-dump
// any number of times. serpent never builds one: it decodes straight into your types, or walks
// the bytes in place. Where you genuinely want a document object, the comparison below is not
// the question you are asking - see the notes in docs/benchmarks.md.

#include "bench.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>

#include <nlohmann/json.hpp>

#include <glaze/glaze.hpp>
#include <rapidjson/document.h>
#include <simdjson/simdjson.h>
#include <yyjson/yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>
#include <ranges>
#include <vector>

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;
using other = nlohmann::json;

// These allocate through malloc rather than operator new, so the counter cannot see them.
constexpr bool counted = true;
constexpr bool uncounted = false;

// ---------------- the same query, written for each library ----------------

namespace query {

inline double canada_coordinates(const std::string &text) {
    double total = 0;
    auto document = json::reader::over(text);
    for (auto feature : document["features"].array())
        for (auto ring : feature["geometry"]["coordinates"].array())
            for (auto point : ring.array())
                for (auto number : point.array())
                    total += number.as_float<double>().value_or(0);
    return total;
}

inline double canada_coordinates_nlohmann(const std::string &text) {
    double total = 0;
    const auto document = other::parse(text);
    for (const auto &feature : document["features"])
        for (const auto &ring : feature["geometry"]["coordinates"])
            for (const auto &point : ring)
                for (const auto &number : point)
                    total += number.get<double>();
    return total;
}

inline double canada_coordinates_simdjson(simdjson::ondemand::parser &parser, const simdjson::padded_string &text) {
    double total = 0;
    auto document = parser.iterate(text);
    for (auto feature : document["features"])
        for (auto ring : feature["geometry"]["coordinates"])
            for (auto point : ring)
                for (auto number : point)
                    total += double(number);
    return total;
}

inline double canada_coordinates_yyjson(const std::string &text) {
    double total = 0;
    yyjson_doc *document = yyjson_read(text.data(), text.size(), 0);
    yyjson_val *features = yyjson_obj_get(yyjson_doc_get_root(document), "features");
    std::size_t i, n;
    yyjson_val *feature;
    yyjson_arr_foreach(features, i, n, feature) {
        yyjson_val *rings = yyjson_obj_get(yyjson_obj_get(feature, "geometry"), "coordinates");
        std::size_t j, m;
        yyjson_val *ring;
        yyjson_arr_foreach(rings, j, m, ring) {
            std::size_t k, o;
            yyjson_val *point;
            yyjson_arr_foreach(ring, k, o, point) {
                std::size_t l, q;
                yyjson_val *number;
                yyjson_arr_foreach(point, l, q, number) total += yyjson_get_num(number);
            }
        }
    }
    yyjson_doc_free(document);
    return total;
}

inline double canada_coordinates_rapidjson(const std::string &text) {
    double total = 0;
    rapidjson::Document document;
    document.Parse(text.c_str());
    for (const auto &feature : document["features"].GetArray())
        for (const auto &ring : feature["geometry"]["coordinates"].GetArray())
            for (const auto &point : ring.GetArray())
                for (const auto &number : point.GetArray())
                    total += number.GetDouble();
    return total;
}

inline std::uint64_t twitter_ids(const std::string &text) {
    std::uint64_t total = 0;
    auto document = json::reader::over(text);
    for (auto status : document["statuses"].array())
        total += status["id"].as_int<std::uint64_t>().value_or(0);
    return total;
}

inline std::uint64_t twitter_ids_nlohmann(const std::string &text) {
    std::uint64_t total = 0;
    const auto document = other::parse(text);
    for (const auto &status : document["statuses"])
        total += status["id"].get<std::uint64_t>();
    return total;
}

inline std::uint64_t twitter_ids_simdjson(simdjson::ondemand::parser &parser, const simdjson::padded_string &text) {
    std::uint64_t total = 0;
    auto document = parser.iterate(text);
    for (auto status : document["statuses"])
        total += std::uint64_t(status["id"]);
    return total;
}

inline std::uint64_t twitter_ids_yyjson(const std::string &text) {
    std::uint64_t total = 0;
    yyjson_doc *document = yyjson_read(text.data(), text.size(), 0);
    yyjson_val *statuses = yyjson_obj_get(yyjson_doc_get_root(document), "statuses");
    std::size_t i, n;
    yyjson_val *status;
    yyjson_arr_foreach(statuses, i, n, status) total += yyjson_get_uint(yyjson_obj_get(status, "id"));
    yyjson_doc_free(document);
    return total;
}

inline std::uint64_t twitter_ids_rapidjson(const std::string &text) {
    std::uint64_t total = 0;
    rapidjson::Document document;
    document.Parse(text.c_str());
    for (const auto &status : document["statuses"].GetArray())
        total += status["id"].GetUint64();
    return total;
}

// Counting every value in the document, recursively.
//
// The lazy readers need this spelled out: asking simdjson for the root object's field count
// only touches the top level, so it would have looked like a whole-document read while doing a
// fraction of the work.

inline std::uint64_t count_values(json::reader value) {
    std::uint64_t total = 1;
    if (value.is_array()) {
        for (auto child : value.array())
            total += count_values(child);
    } else if (value.is_object()) {
        for (auto member : value.items())
            total += count_values(member.value);
    }
    return total;
}

inline std::uint64_t count_values_nlohmann(const other &value) {
    std::uint64_t total = 1;
    if (value.is_array() || value.is_object())
        for (const auto &child : value)
            total += count_values_nlohmann(child);
    return total;
}

inline std::uint64_t count_values_simdjson(simdjson::ondemand::value value) {
    std::uint64_t total = 1;
    if (value.type() == simdjson::ondemand::json_type::array) {
        for (auto child : value.get_array())
            total += count_values_simdjson(child.value());
    } else if (value.type() == simdjson::ondemand::json_type::object) {
        for (auto field : value.get_object())
            total += count_values_simdjson(field.value());
    }
    return total;
}

inline std::uint64_t count_values_yyjson(yyjson_val *value) {
    std::uint64_t total = 1;
    if (yyjson_is_arr(value)) {
        std::size_t i, n;
        yyjson_val *child;
        yyjson_arr_foreach(value, i, n, child) total += count_values_yyjson(child);
    } else if (yyjson_is_obj(value)) {
        std::size_t i, n;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, i, n, key, child) total += count_values_yyjson(child);
    }
    return total;
}

inline std::uint64_t count_values_rapidjson(const rapidjson::Value &value) {
    std::uint64_t total = 1;
    if (value.IsArray()) {
        for (const auto &child : value.GetArray())
            total += count_values_rapidjson(child);
    } else if (value.IsObject()) {
        for (const auto &member : value.GetObject())
            total += count_values_rapidjson(member.value);
    }
    return total;
}

} // namespace query

// ---------------- allocation counting ----------------

void *operator new(std::size_t size) {
    if (bench::counting) {
        ++bench::allocation_count;
        bench::allocation_bytes += size;
    }
    void *memory = std::malloc(size);
    if (!memory) throw std::bad_alloc {};
    return memory;
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

// ---------------- the type both libraries convert ----------------

// Defined the way serpent means types to be defined where the compiler allows it, since that
// is the path being measured; written out elsewhere so the benchmark still runs.
#if SERPENT_HAS_REFLECTION
struct[[= serpent::serializable {}]] reading {
    std::uint64_t timestamp = 0;
    std::string station;
    double celsius = 0;
    double humidity = 0;
    bool valid = false;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(reading, timestamp, station, celsius, humidity, valid)
};
#else
struct reading {
    std::uint64_t timestamp = 0;
    std::string station;
    double celsius = 0;
    double humidity = 0;
    bool valid = false;

    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), reading> value) {
        visitor.member("timestamp", value.timestamp);
        visitor.member("station", value.station);
        visitor.member("celsius", value.celsius);
        visitor.member("humidity", value.humidity);
        visitor.member("valid", value.valid);
    }

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(reading, timestamp, station, celsius, humidity, valid)
};
#endif

static std::vector<reading> sample_readings(std::size_t count) {
    std::vector<reading> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        values.push_back({
                1'700'000'000 + index,
                "station-" + std::to_string(index % 97),
                -40.0 + static_cast<double>(index % 800) * 0.1,
                static_cast<double>(index % 1000) * 0.001,
                index % 3 != 0,
        });
    }
    return values;
}

static std::string read_file(const std::string &path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        std::fprintf(stderr, "missing corpus file: %s\n", path.c_str());
        std::exit(1);
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// ---------------- correctness ----------------

// A lazy reader that quietly returns nothing would look extremely fast while doing no work, so
// every measured computation is run once against the other library first and has to agree.

static int failures = 0;

template<typename A, typename B>
static void agree(std::string_view what, const A &ours, const B &theirs) {
    const bool same = ours == theirs;
    if (!same) ++failures;
    std::printf("  %-38s %s\n", std::string { what }.c_str(), same ? "ok" : "MISMATCH");
}

static void agree_near(std::string_view what, double ours, double theirs) {
    const bool same = std::abs(ours - theirs) <= std::abs(theirs) * 1e-12;
    if (!same) ++failures;
    std::printf("  %-38s %s\n", std::string { what }.c_str(), same ? "ok" : "MISMATCH");
}

// ---------------- benchmarks ----------------

static void check_results(const std::vector<reading> &values, const std::string &canada, const std::string &citm,
        const std::string &twitter) {
    std::printf("\ncorrectness: every library must compute what serpent computes\n");

    const auto text = json::encode(values);
    agree("round-trip 10k records (JSON)", json::decode<std::vector<reading>>(text)->size(),
            other::parse(text).get<std::vector<reading>>().size());
    agree("our JSON parses as nlohmann's", other::parse(text).get<std::vector<reading>>().at(9999).station,
            values.at(9999).station);
    {
        std::vector<reading> glazed;
        const bool read = !glz::read_json(glazed, text);
        agree("our JSON parses as glaze's", read && glazed.size() == values.size() ? glazed.at(9999).station : "",
                values.at(9999).station);
    }
    agree("our BJData reads as nlohmann's",
            other::from_bjdata(std::vector<std::uint8_t>(
                                       std::from_range, bjdata::encode(values) | std::views::transform([](std::byte b) {
                                           return static_cast<std::uint8_t>(b);
                                       })))
                    .get<std::vector<reading>>()
                    .at(4321)
                    .station,
            values.at(4321).station);

    {
        std::string beve;
        (void)glz::write_beve(values, beve);
        std::vector<reading> out;
        const bool read = !glz::read_beve(out, beve);
        agree("BEVE round-trips the same records", read && out.size() == values.size() ? out.at(777).station : "",
                values.at(777).station);
    }
    {
        const auto cbor = other::to_cbor(other(values));
        agree("CBOR round-trips the same records", other::from_cbor(cbor).get<std::vector<reading>>().at(777).station,
                values.at(777).station);
    }

    simdjson::ondemand::parser parser;
    const simdjson::padded_string canada_padded { canada }, twitter_padded { twitter }, citm_padded { citm };

    const double coordinates = query::canada_coordinates(canada);
    agree_near("canada coordinates vs nlohmann", coordinates, query::canada_coordinates_nlohmann(canada));
    agree_near(
            "canada coordinates vs simdjson", coordinates, query::canada_coordinates_simdjson(parser, canada_padded));
    agree_near("canada coordinates vs yyjson", coordinates, query::canada_coordinates_yyjson(canada));
    agree_near("canada coordinates vs rapidjson", coordinates, query::canada_coordinates_rapidjson(canada));

    const std::uint64_t ids = query::twitter_ids(twitter);
    agree("twitter ids vs nlohmann", ids, query::twitter_ids_nlohmann(twitter));
    agree("twitter ids vs simdjson", ids, query::twitter_ids_simdjson(parser, twitter_padded));
    agree("twitter ids vs yyjson", ids, query::twitter_ids_yyjson(twitter));
    agree("twitter ids vs rapidjson", ids, query::twitter_ids_rapidjson(twitter));

    const std::uint64_t total = query::count_values(json::reader::over(citm));
    agree("citm value count vs nlohmann", total, query::count_values_nlohmann(other::parse(citm)));
    {
        auto document = parser.iterate(citm_padded);
        agree("citm value count vs simdjson", total, query::count_values_simdjson(document.get_value().value()));
    }
    {
        yyjson_doc *document = yyjson_read(citm.data(), citm.size(), 0);
        agree("citm value count vs yyjson", total, query::count_values_yyjson(yyjson_doc_get_root(document)));
        yyjson_doc_free(document);
    }
    {
        rapidjson::Document document;
        document.Parse(citm.c_str());
        agree("citm value count vs rapidjson", total, query::count_values_rapidjson(document));
    }

    const auto first_key = json::reader::over(citm)["areaNames"]["205705994"].as_string().value_or("<none>");
    agree("citm first-key lookup", first_key, other::parse(citm)["areaNames"]["205705994"].get<std::string>());
    agree("citm last-key lookup", json::reader::over(citm)["subjectNames"].is_object(), true);
    agree("citm validates", json::validate(citm).has_value(), true);

    if (failures > 0) {
        std::printf("\n%d benchmark(s) do not compute the same thing - numbers withheld\n", failures);
        std::exit(1);
    }
}

static void your_types(const std::vector<reading> &values) {
    const std::string text = json::encode(values);
    const std::vector<std::byte> bytes = bjdata::encode(values);
    const std::vector<std::uint8_t> other_bytes = other::to_bjdata(other(values));
    const std::size_t size = text.size();

    bench::measure("decode 10k records (JSON)", "serpent", size,
            [&] { return json::decode<std::vector<reading>>(text)->size(); });
    bench::measure("decode 10k records (JSON)", "nlohmann", size,
            [&] { return other::parse(text).get<std::vector<reading>>().size(); });

    bench::measure("decode 10k records (JSON)", "glaze", size, [&] {
        std::vector<reading> out;
        return glz::read_json(out, text) ? 0 : out.size();
    });

    bench::measure("encode 10k records (JSON)", "serpent", size, [&] { return json::encode(values).size(); });
    bench::measure("encode 10k records (JSON)", "nlohmann", size, [&] { return other(values).dump().size(); });
    bench::measure("encode 10k records (JSON)", "glaze", size, [&] {
        std::string buffer;
        (void)glz::write_json(values, buffer);
        return buffer.size();
    });

    bench::measure("decode 10k records (BJData)", "serpent", bytes.size(),
            [&] { return bjdata::decode<std::vector<reading>>(bytes)->size(); });
    bench::measure("decode 10k records (BJData)", "nlohmann", other_bytes.size(),
            [&] { return other::from_bjdata(other_bytes).get<std::vector<reading>>().size(); });

    bench::measure(
            "encode 10k records (BJData)", "serpent", bytes.size(), [&] { return bjdata::encode(values).size(); });
    bench::measure("encode 10k records (BJData)", "nlohmann", other_bytes.size(),
            [&] { return other::to_bjdata(other(values)).size(); });
}

/**
 * Binary against binary.
 *
 * This is also the comparison with no lookup tables in it. The struct-mapping library's tables
 * - digit classification, escape decoding, integer digit pairs - are all in its JSON text path;
 * its binary format touches none of them, and neither does BJData.
 */
static void binary_formats(const std::vector<reading> &values) {
    std::string beve;
    (void)glz::write_beve(values, beve);
    const auto bjdata_bytes = bjdata::encode(values);
    const auto cbor = other::to_cbor(other(values));
    const auto msgpack = other::to_msgpack(other(values));

    std::printf("\nbinary sizes for the same 10k records: BJData %zu B, BEVE %zu B, CBOR %zu B, MessagePack %zu B\n",
            bjdata_bytes.size(), beve.size(), cbor.size(), msgpack.size());

    bench::measure(
            "binary encode 10k records", "serpent", bjdata_bytes.size(), [&] { return bjdata::encode(values).size(); });
    bench::measure("binary encode 10k records", "glaze", beve.size(), [&] {
        std::string buffer;
        (void)glz::write_beve(values, buffer);
        return buffer.size();
    });
    bench::measure(
            "binary encode 10k records", "nlohmann", cbor.size(), [&] { return other::to_cbor(other(values)).size(); });

    bench::measure("binary decode 10k records", "serpent", bjdata_bytes.size(),
            [&] { return bjdata::decode<std::vector<reading>>(bjdata_bytes)->size(); });
    bench::measure("binary decode 10k records", "glaze", beve.size(), [&] {
        std::vector<reading> out;
        return glz::read_beve(out, beve) ? 0 : out.size();
    });
    bench::measure("binary decode 10k records", "nlohmann", cbor.size(),
            [&] { return other::from_cbor(cbor).get<std::vector<reading>>().size(); });
}

static void whole_document_scan(const std::string &canada, const std::string &twitter) {
    simdjson::ondemand::parser canada_parser, twitter_parser;
    const simdjson::padded_string canada_padded { canada }, twitter_padded { twitter };

    bench::measure("sum coordinates, canada.json", "serpent", canada.size(),
            [&] { return query::canada_coordinates(canada); });
    bench::measure(
            "sum coordinates, canada.json", "simdjson", canada.size(),
            [&] { return query::canada_coordinates_simdjson(canada_parser, canada_padded); }, uncounted);
    bench::measure(
            "sum coordinates, canada.json", "yyjson", canada.size(),
            [&] { return query::canada_coordinates_yyjson(canada); }, uncounted);
    bench::measure(
            "sum coordinates, canada.json", "rapidjson", canada.size(),
            [&] { return query::canada_coordinates_rapidjson(canada); }, uncounted);
    bench::measure("sum coordinates, canada.json", "nlohmann", canada.size(),
            [&] { return query::canada_coordinates_nlohmann(canada); });

    bench::measure("sum ids, twitter.json", "serpent", twitter.size(), [&] { return query::twitter_ids(twitter); });
    bench::measure(
            "sum ids, twitter.json", "simdjson", twitter.size(),
            [&] { return query::twitter_ids_simdjson(twitter_parser, twitter_padded); }, uncounted);
    bench::measure(
            "sum ids, twitter.json", "yyjson", twitter.size(), [&] { return query::twitter_ids_yyjson(twitter); },
            uncounted);
    bench::measure(
            "sum ids, twitter.json", "rapidjson", twitter.size(), [&] { return query::twitter_ids_rapidjson(twitter); },
            uncounted);
    bench::measure(
            "sum ids, twitter.json", "nlohmann", twitter.size(), [&] { return query::twitter_ids_nlohmann(twitter); });
}

static void targeted_extraction(const std::string &citm) {
    simdjson::ondemand::parser parser;
    const simdjson::padded_string padded { citm };

    // areaNames is the first key in the document; subjectNames is the last. A reader that scans
    // lazily is at its best on the first and its worst on the last, and both belong in the table.
    bench::measure("first key, citm_catalog.json", "serpent", citm.size(), [&] {
        auto document = json::reader::over(citm);
        return document["areaNames"]["205705994"].as_string().value_or("").size();
    });
    bench::measure(
            "first key, citm_catalog.json", "simdjson", citm.size(),
            [&] {
                auto document = parser.iterate(padded);
                return std::string_view(document["areaNames"]["205705994"]).size();
            },
            uncounted);
    bench::measure(
            "first key, citm_catalog.json", "yyjson", citm.size(),
            [&] {
                yyjson_doc *document = yyjson_read(citm.data(), citm.size(), 0);
                const auto size = std::string_view { yyjson_get_str(yyjson_obj_get(
                                                             yyjson_obj_get(yyjson_doc_get_root(document), "areaNames"),
                                                             "205705994")) }
                                          .size();
                yyjson_doc_free(document);
                return size;
            },
            uncounted);
    bench::measure(
            "first key, citm_catalog.json", "rapidjson", citm.size(),
            [&] {
                rapidjson::Document document;
                document.Parse(citm.c_str());
                return std::string_view { document["areaNames"]["205705994"].GetString() }.size();
            },
            uncounted);
    bench::measure("first key, citm_catalog.json", "nlohmann", citm.size(),
            [&] { return other::parse(citm)["areaNames"]["205705994"].get<std::string>().size(); });

    bench::measure("last key, citm_catalog.json", "serpent", citm.size(),
            [&] { return json::reader::over(citm)["subjectNames"].is_object(); });
    bench::measure(
            "last key, citm_catalog.json", "simdjson", citm.size(),
            [&] {
                auto document = parser.iterate(padded);
                return document["subjectNames"].type() == simdjson::ondemand::json_type::object;
            },
            uncounted);
    bench::measure(
            "last key, citm_catalog.json", "yyjson", citm.size(),
            [&] {
                yyjson_doc *document = yyjson_read(citm.data(), citm.size(), 0);
                const bool object = yyjson_is_obj(yyjson_obj_get(yyjson_doc_get_root(document), "subjectNames"));
                yyjson_doc_free(document);
                return object;
            },
            uncounted);
    bench::measure(
            "last key, citm_catalog.json", "rapidjson", citm.size(),
            [&] {
                rapidjson::Document document;
                document.Parse(citm.c_str());
                return document["subjectNames"].IsObject();
            },
            uncounted);
    bench::measure("last key, citm_catalog.json", "nlohmann", citm.size(),
            [&] { return other::parse(citm)["subjectNames"].is_object(); });
}

static void full_read(const std::string &citm) {
    simdjson::ondemand::parser parser;
    const simdjson::padded_string padded { citm };

    // Every value in the document actually visited, in every library.
    bench::measure("count every value, citm_catalog.json", "serpent", citm.size(),
            [&] { return query::count_values(json::reader::over(citm)); });
    bench::measure(
            "count every value, citm_catalog.json", "simdjson", citm.size(),
            [&] {
                auto document = parser.iterate(padded);
                return query::count_values_simdjson(document.get_value().value());
            },
            uncounted);
    bench::measure(
            "count every value, citm_catalog.json", "yyjson", citm.size(),
            [&] {
                yyjson_doc *document = yyjson_read(citm.data(), citm.size(), 0);
                const auto total = query::count_values_yyjson(yyjson_doc_get_root(document));
                yyjson_doc_free(document);
                return total;
            },
            uncounted);
    bench::measure(
            "count every value, citm_catalog.json", "rapidjson", citm.size(),
            [&] {
                rapidjson::Document document;
                document.Parse(citm.c_str());
                return query::count_values_rapidjson(document);
            },
            uncounted);
    bench::measure("count every value, citm_catalog.json", "nlohmann", citm.size(),
            [&] { return query::count_values_nlohmann(other::parse(citm)); });

    // A structural scan with no values returned, which is the cheapest thing serpent can do to
    // a whole document and has no counterpart in a library that must build one.
    bench::measure("validate only, citm_catalog.json", "serpent", citm.size(),
            [&] { return json::validate(citm).has_value(); });
}

int main(int argc, char **argv) {
    const std::string corpus = argc > 1 ? argv[1] : ".";
    const auto canada = read_file(corpus + "/canada.json");
    const auto citm = read_file(corpus + "/citm_catalog.json");
    const auto twitter = read_file(corpus + "/twitter.json");
    const auto values = sample_readings(10'000);

    std::printf(
            "corpus: canada %zu B, citm_catalog %zu B, twitter %zu B\n", canada.size(), citm.size(), twitter.size());

    check_results(values, canada, citm, twitter);

    your_types(values);
    binary_formats(values);
    whole_document_scan(canada, twitter);
    targeted_extraction(citm);
    full_read(citm);

    bench::report();
    return 0;
}
