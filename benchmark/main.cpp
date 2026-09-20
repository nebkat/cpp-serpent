// serpent against other libraries, on your own types and on the corpus the published C++ JSON
// benchmarks use. Every workload is measured in every library that can do it, and printed as one
// table per workload with the same rows and columns throughout. The comparisons are not all the
// same shape - a DOM library builds an object graph, serpent never does - and docs/benchmarks.md
// says what to make of that.

#include "bench.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/json/indexed.hpp>

#include <nlohmann/json.hpp>

#include <glaze/cbor.hpp>
#include <glaze/glaze.hpp>
#include <rapidjson/document.h>
#include <simdjson/simdjson.h>
#include <yyjson/yyjson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;
using other = nlohmann::json;

// These allocate through malloc rather than operator new, so the counter cannot see them.
constexpr bool uncounted = false;

// ---------------- the same query, written for each library ----------------

namespace query {

inline double canada_coordinates(const std::string &text) {
    double total = 0;
    auto document = json::reader::over(text);
    for (const auto &feature : document["features"].array())
        for (const auto &ring : feature["geometry"]["coordinates"].array())
            for (const auto &point : ring.array())
                for (const auto &number : point.array())
                    total += number.as<double>().value_or(0);
    return total;
}

inline double canada_coordinates_indexed(const std::string &text) {
    double total = 0;
    const auto index = json::structural_index::over(text);
    for (const auto &feature : index.root()["features"].array())
        for (const auto &ring : feature["geometry"]["coordinates"].array())
            for (const auto &point : ring.array())
                for (const auto &number : point.array())
                    total += number.as<double>().value_or(0);
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
    for (const auto &status : document["statuses"].array())
        total += status["id"].as<std::uint64_t>().value_or(0);
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

inline std::uint64_t count_values(const json::reader &value) {
    std::uint64_t total = 1;
    if (value.is_array()) {
        for (const auto &child : value.array())
            total += count_values(child);
    } else if (value.is_object()) {
        for (const auto &member : value.items())
            total += count_values(member.value);
    }
    return total;
}

inline std::uint64_t count_values_indexed(json::indexed_reader value) {
    std::uint64_t total = 1;
    if (value.is_array()) {
        for (const auto &child : value.array()) total += count_values_indexed(child);
    } else if (value.is_object()) {
        for (const auto &member : value.items()) total += count_values_indexed(member.value);
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

// ---------------- the types ----------------

// Defined the way serpent means types to be defined where the compiler allows it, since that is
// the path being measured; the mixed record is also written out by hand so the benchmark still
// runs without reflection. The other two libraries reflect an aggregate and use a macro.

#if SERPENT_HAS_REFLECTION
#define SERPENT_BENCH_DESCRIBED [[= serpent::serializable {}]]
#else
#define SERPENT_BENCH_DESCRIBED
#endif

struct SERPENT_BENCH_DESCRIBED reading {
    std::uint64_t timestamp = 0;
    std::string station;
    double celsius = 0;
    double humidity = 0;
    bool valid = false;

#if !SERPENT_HAS_REFLECTION
    friend void json_convert(auto &visitor, serpent::conversion_object_t<decltype(visitor), reading> value) {
        visitor.member("timestamp", value.timestamp);
        visitor.member("station", value.station);
        visitor.member("celsius", value.celsius);
        visitor.member("humidity", value.humidity);
        visitor.member("valid", value.valid);
    }
#endif
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(reading, timestamp, station, celsius, humidity, valid)
};

// Five members of one kind each, the same count and much the same key lengths as the record,
// so that each kind's cost can be read off on its own.
struct SERPENT_BENCH_DESCRIBED integers {
    std::int64_t sequence = 0, latitude = 0, longitude = 0, elevation = 0, heading = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(integers, sequence, latitude, longitude, elevation, heading)
};
struct SERPENT_BENCH_DESCRIBED reals {
    double celsius = 0, humidity = 0, pressure = 0, wind_speed = 0, rainfall = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(reals, celsius, humidity, pressure, wind_speed, rainfall)
};
struct SERPENT_BENCH_DESCRIBED strings {
    std::string station, operator_name, region, firmware, comment;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(strings, station, operator_name, region, firmware, comment)
};
struct SERPENT_BENCH_DESCRIBED switches {
    bool valid = false, calibrated = false, transmitting = false, low_battery = false, maintenance = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(switches, valid, calibrated, transmitting, low_battery, maintenance)
};

// A record that is mostly numbers in bulk, which is what a recording is.
struct SERPENT_BENCH_DESCRIBED trace {
    std::uint32_t id = 0;
    std::vector<double> samples;
    std::vector<std::int32_t> counts;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(trace, id, samples, counts)
};

template<typename T, typename Make>
static std::vector<T> sample(std::size_t count, Make make) {
    std::vector<T> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) values.push_back(make(index));
    return values;
}

static std::vector<reading> sample_readings() {
    return sample<reading>(10'000, [](std::size_t index) {
        return reading { 1'700'000'000 + index, "station-" + std::to_string(index % 97),
            -40.0 + static_cast<double>(index % 800) * 0.1, static_cast<double>(index % 1000) * 0.001, index % 3 != 0 };
    });
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
    // The DOM library reads draft 3, so it is shown the array-of-objects form; the table the
    // records would be by default is draft 4.
    agree("our BJData reads as nlohmann's",
            other::from_bjdata(std::vector<std::uint8_t>(
                                       std::from_range, bjdata::encode(values, { .tables = false }) | std::views::transform([](std::byte b) {
                                           return static_cast<std::uint8_t>(b);
                                       })))
                    .get<std::vector<reading>>()
                    .at(4321)
                    .station,
            values.at(4321).station);

    {
        std::string beve;
        std::ignore = glz::write_beve(values, beve);
        std::vector<reading> out;
        const bool read = !glz::read_beve(out, beve);
        agree("BEVE round-trips the same records", read && out.size() == values.size() ? out.at(777).station : "",
                values.at(777).station);
    }
    {
        std::string encoded;
        std::ignore = glz::write_cbor(values, encoded);
        std::vector<reading> out;
        const bool read = !glz::read_cbor(out, encoded);
        agree("glaze CBOR round-trips the same records", read && out.size() == values.size() ? out.at(777).station : "",
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

    const auto first_key = json::reader::over(citm)["areaNames"]["205705994"].as<std::string>().value_or("<none>");
    agree("citm first-key lookup", first_key, other::parse(citm)["areaNames"]["205705994"].get<std::string>());
    agree("citm last-key lookup", json::reader::over(citm)["subjectNames"].is_object(), true);
    agree("citm validates", json::validate(citm).has_value(), true);

    if (failures > 0) {
        std::printf("\n%d benchmark(s) do not compute the same thing - numbers withheld\n", failures);
        std::exit(1);
    }
}

// ---------------- your own types ----------------

/**
 * One workload of values, through every library in both formats.
 *
 * Binary is each library's own: BJData for serpent, BEVE for the struct-mapping library, CBOR
 * for the DOM library - and CBOR through the struct-mapping library too, which holds the format
 * still and varies only the library.
 */
template<typename T>
static void typed_workload(const std::string &workload, const std::vector<T> &values) {
    const std::string text = json::encode(values);
    // Binary is each library's own format: BJData as an array of objects, written for speed or
    // for size, and - where the record type allows - as a draft 4 table, which is what the
    // library writes for such records unless told not to.
    const auto fast = bjdata::encode<bjdata::prefer::speed>(values, { .tables = false });
    const auto small = bjdata::encode<bjdata::prefer::size>(values, { .tables = false });
    const auto table = bjdata::encode(values);
    const bool tabled = table.size() > 2 && table[1] == std::byte { '$' };
    std::string beve, glaze_cbor;
    std::ignore = glz::write_beve(values, beve);
    std::ignore = glz::write_cbor(values, glaze_cbor);
    const auto cbor = other::to_cbor(other(values));

    bench::measure(workload, "JSON encode", "serpent", text.size(), [&] { return json::encode(values).size(); });
    bench::measure(workload, "JSON encode", "glaze", text.size(), [&] {
        std::string buffer;
        std::ignore = glz::write_json(values, buffer);
        return buffer.size();
    });
    bench::measure(workload, "JSON encode", "glaze (size build)", text.size(), [&] {
        std::string buffer;
        std::ignore = glz::write<glz::opts_size {}>(values, buffer);
        return buffer.size();
    });
    bench::measure(workload, "JSON encode", "nlohmann", text.size(), [&] { return other(values).dump().size(); });

    // A std::string is read as terminated, as glaze reads it by default; the bounded reading is
    // what a text with nothing known past its end gets, and glaze's null_terminated = false
    // is its equivalent.
    bench::measure(workload, "JSON decode", "serpent", text.size(),
            [&] { return json::decode<std::vector<T>>(text)->size(); });
    bench::measure(workload, "JSON decode", "serpent (bounded)", text.size(),
            [&] { return json::decode<std::vector<T>>(std::string_view { text })->size(); });
    bench::measure(workload, "JSON decode", "glaze", text.size(), [&] {
        std::vector<T> out;
        return glz::read_json(out, text) ? 0 : out.size();
    });
    bench::measure(workload, "JSON decode", "glaze (bounded)", text.size(), [&] {
        std::vector<T> out;
        return glz::read<glz::opts { .null_terminated = false }>(out, text) ? 0 : out.size();
    });
    bench::measure(workload, "JSON decode", "glaze (size build)", text.size(), [&] {
        std::vector<T> out;
        return glz::read<glz::opts_size {}>(out, text) ? 0 : out.size();
    });
    bench::measure(workload, "JSON decode", "nlohmann", text.size(),
            [&] { return other::parse(text).get<std::vector<T>>().size(); });

    bench::measure(workload, "binary encode", "serpent", fast.size(),
            [&] { return bjdata::encode<bjdata::prefer::speed>(values, { .tables = false }).size(); });
    bench::measure(workload, "binary encode", "serpent (for size)", small.size(),
            [&] { return bjdata::encode<bjdata::prefer::size>(values, { .tables = false }).size(); });
    if (tabled) {
        bench::measure(workload, "binary encode", "serpent (table)", table.size(),
                [&] { return bjdata::encode(values).size(); });
    }
    bench::measure(workload, "binary encode", "glaze", beve.size(), [&] {
        std::string buffer;
        std::ignore = glz::write_beve(values, buffer);
        return buffer.size();
    });
    bench::measure(workload, "binary encode", "glaze (CBOR)", glaze_cbor.size(), [&] {
        std::string buffer;
        std::ignore = glz::write_cbor(values, buffer);
        return buffer.size();
    });
    bench::measure(workload, "binary encode", "nlohmann", cbor.size(),
            [&] { return other::to_cbor(other(values)).size(); });

    bench::measure(workload, "binary decode", "serpent", fast.size(),
            [&] { return bjdata::decode<std::vector<T>>(fast)->size(); });
    bench::measure(workload, "binary decode", "serpent (for size)", small.size(),
            [&] { return bjdata::decode<std::vector<T>>(small)->size(); });
    if (tabled) {
        bench::measure(workload, "binary decode", "serpent (table)", table.size(),
                [&] { return bjdata::decode<std::vector<T>>(table)->size(); });
    }
    bench::measure(workload, "binary decode", "glaze", beve.size(), [&] {
        std::vector<T> out;
        return glz::read_beve(out, beve) ? 0 : out.size();
    });
    bench::measure(workload, "binary decode", "glaze (CBOR)", glaze_cbor.size(), [&] {
        std::vector<T> out;
        return glz::read_cbor(out, glaze_cbor) ? 0 : out.size();
    });
    bench::measure(workload, "binary decode", "nlohmann", cbor.size(),
            [&] { return other::from_cbor(cbor).get<std::vector<T>>().size(); });
}

static void your_own_types(const std::vector<reading> &readings) {
    constexpr std::size_t count = 10'000;
    typed_workload("10k records", readings);
    typed_workload("10k records of 5 integers", sample<integers>(count, [](std::size_t index) {
        const auto value = static_cast<std::int64_t>(index);
        return integers { value, 49'000'000 + value * 37, -123'000'000 - value * 91, value % 3000, value % 360 };
    }));
    typed_workload("10k records of 5 reals", sample<reals>(count, [](std::size_t index) {
        const auto value = static_cast<double>(index);
        return reals { -40.0 + value * 0.1, value * 0.001, 1013.25 - value * 0.013, value / 7.0, value * 1e-5 };
    }));
    typed_workload("10k records of 5 strings", sample<strings>(count, [](std::size_t index) {
        return strings { "station-" + std::to_string(index % 97), "operator " + std::to_string(index % 13),
            "north-west", "v1.4." + std::to_string(index % 50), "nothing to report for this interval" };
    }));
    typed_workload("10k records of 5 booleans", sample<switches>(count, [](std::size_t index) {
        return switches { index % 2 == 0, index % 3 == 0, index % 5 == 0, index % 7 == 0, index % 11 == 0 };
    }));
    typed_workload("100 records of 2000 numbers", sample<trace>(100, [](std::size_t index) {
        trace one { static_cast<std::uint32_t>(index) };
        for (int sample = 0; sample < 1000; ++sample) {
            one.samples.push_back(sample * 0.37 + static_cast<double>(index));
            one.counts.push_back(sample * 3 - 500);
        }
        return one;
    }));
}

// ---------------- documents without a type ----------------

/**
 * The same six libraries on every task: serpent reading in place, serpent over an index built
 * first, an on-demand parser, two DOM parsers built for speed, and the DOM library.
 */
static void documents(const std::string &canada, const std::string &twitter, const std::string &citm) {
    simdjson::ondemand::parser parser;
    const simdjson::padded_string canada_padded { canada }, twitter_padded { twitter }, citm_padded { citm };
    const auto in_yyjson = [](const std::string &text, auto use) {
        yyjson_doc *document = yyjson_read(text.data(), text.size(), 0);
        const auto answer = use(yyjson_doc_get_root(document));
        yyjson_doc_free(document);
        return answer;
    };
    const auto in_rapidjson = [](const std::string &text, auto use) {
        rapidjson::Document document;
        document.Parse(text.c_str());
        return use(document);
    };

    // areaNames is the first key in citm_catalog.json and subjectNames the last: a reader that
    // scans lazily is at its best on the one and its worst on the other. The others walk a whole
    // document, or most of one.
    const char *const workload = "JSON documents";
    bench::measure(workload, "citm first key", "serpent", citm.size(),
            [&] { return json::reader::over(citm)["areaNames"]["205705994"].as<std::string>().value_or("").size(); });
    bench::measure(workload, "citm first key", "serpent (indexed)", citm.size(), [&] {
        return json::structural_index::over(citm).root()["areaNames"]["205705994"].as<std::string>().value_or("").size();
    });
    bench::measure(workload, "citm first key", "simdjson", citm.size(), [&] {
        auto document = parser.iterate(citm_padded);
        return std::string_view(document["areaNames"]["205705994"]).size();
    }, uncounted);
    bench::measure(workload, "citm first key", "yyjson", citm.size(), [&] {
        return in_yyjson(citm, [](yyjson_val *root) {
            return std::string_view { yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "areaNames"), "205705994")) }.size();
        });
    }, uncounted);
    bench::measure(workload, "citm first key", "rapidjson", citm.size(), [&] {
        return in_rapidjson(citm, [](const rapidjson::Document &document) {
            return std::string_view { document["areaNames"]["205705994"].GetString() }.size();
        });
    }, uncounted);
    bench::measure(workload, "citm first key", "nlohmann", citm.size(),
            [&] { return other::parse(citm)["areaNames"]["205705994"].get<std::string>().size(); });

    const char *const last = workload;
    bench::measure(last, "citm last key", "serpent", citm.size(), [&] { return json::reader::over(citm)["subjectNames"].is_object(); });
    bench::measure(last, "citm last key", "serpent (indexed)", citm.size(),
            [&] { return json::structural_index::over(citm).root()["subjectNames"].is_object(); });
    bench::measure(last, "citm last key", "simdjson", citm.size(), [&] {
        auto document = parser.iterate(citm_padded);
        return document["subjectNames"].type() == simdjson::ondemand::json_type::object;
    }, uncounted);
    bench::measure(last, "citm last key", "yyjson", citm.size(), [&] {
        return in_yyjson(citm, [](yyjson_val *root) { return yyjson_is_obj(yyjson_obj_get(root, "subjectNames")); });
    }, uncounted);
    bench::measure(last, "citm last key", "rapidjson", citm.size(), [&] {
        return in_rapidjson(citm, [](const rapidjson::Document &document) { return document["subjectNames"].IsObject(); });
    }, uncounted);
    bench::measure(last, "citm last key", "nlohmann", citm.size(), [&] { return other::parse(citm)["subjectNames"].is_object(); });

    const char *const ids = workload;
    bench::measure(ids, "twitter sum ids", "serpent", twitter.size(), [&] { return query::twitter_ids(twitter); });
    bench::measure(ids, "twitter sum ids", "serpent (indexed)", twitter.size(), [&] {
        std::uint64_t total = 0;
        const auto index = json::structural_index::over(twitter);
        for (const auto &status : index.root()["statuses"].array()) total += status["id"].as<std::uint64_t>().value_or(0);
        return total;
    });
    bench::measure(ids, "twitter sum ids", "simdjson", twitter.size(), [&] { return query::twitter_ids_simdjson(parser, twitter_padded); }, uncounted);
    bench::measure(ids, "twitter sum ids", "yyjson", twitter.size(), [&] { return query::twitter_ids_yyjson(twitter); }, uncounted);
    bench::measure(ids, "twitter sum ids", "rapidjson", twitter.size(), [&] { return query::twitter_ids_rapidjson(twitter); }, uncounted);
    bench::measure(ids, "twitter sum ids", "nlohmann", twitter.size(), [&] { return query::twitter_ids_nlohmann(twitter); });

    const char *const count = workload;
    bench::measure(count, "citm count values", "serpent", citm.size(), [&] { return query::count_values(json::reader::over(citm)); });
    bench::measure(count, "citm count values", "serpent (indexed)", citm.size(), [&] {
        const auto index = json::structural_index::over(citm);
        return query::count_values_indexed(index.root());
    });
    bench::measure(count, "citm count values", "simdjson", citm.size(), [&] {
        auto document = parser.iterate(citm_padded);
        return query::count_values_simdjson(document.get_value().value());
    }, uncounted);
    bench::measure(count, "citm count values", "yyjson", citm.size(), [&] { return in_yyjson(citm, query::count_values_yyjson); }, uncounted);
    bench::measure(count, "citm count values", "rapidjson", citm.size(), [&] {
        return in_rapidjson(citm, [](const rapidjson::Document &document) { return query::count_values_rapidjson(document); });
    }, uncounted);
    bench::measure(count, "citm count values", "nlohmann", citm.size(), [&] { return query::count_values_nlohmann(other::parse(citm)); });

    const char *const coordinates = workload;
    bench::measure(coordinates, "canada sum coords", "serpent", canada.size(), [&] { return query::canada_coordinates(canada); });
    bench::measure(coordinates, "canada sum coords", "serpent (indexed)", canada.size(), [&] { return query::canada_coordinates_indexed(canada); });
    bench::measure(coordinates, "canada sum coords", "simdjson", canada.size(), [&] { return query::canada_coordinates_simdjson(parser, canada_padded); }, uncounted);
    bench::measure(coordinates, "canada sum coords", "yyjson", canada.size(), [&] { return query::canada_coordinates_yyjson(canada); }, uncounted);
    bench::measure(coordinates, "canada sum coords", "rapidjson", canada.size(), [&] { return query::canada_coordinates_rapidjson(canada); }, uncounted);
    bench::measure(coordinates, "canada sum coords", "nlohmann", canada.size(), [&] { return query::canada_coordinates_nlohmann(canada); });
}

/** Which of the library's switches this build has on, since that is what the numbers depend on. */
static void print_configuration() {
    std::printf("configuration: %s\n", bench::chosen.configuration.c_str());
    std::printf("  SERPENT_USE_ZMIJ              %d\n", SERPENT_USE_ZMIJ);
    std::printf("  SERPENT_ZMIJ_OPTIMIZE_SIZE    %d  (how the Zmij this is linked to was built)\n",
            SERPENT_ZMIJ_OPTIMIZE_SIZE);
    std::printf("  SERPENT_BOUNDED_OBJECT_WRITE  %d\n", SERPENT_BOUNDED_OBJECT_WRITE);
    std::printf("  SERPENT_INTEGER_TABLE         %d\n", SERPENT_INTEGER_TABLE);
    std::printf("  SERPENT_USE_FAST_FLOAT        %d\n", SERPENT_USE_FAST_FLOAT);
    std::printf("  SERPENT_WIDE_STRING_SCAN      %d\n", SERPENT_WIDE_STRING_SCAN);
    std::printf("  SERPENT_FORCE_INLINE          %d\n", SERPENT_FORCE_INLINE);
}

int main(int argc, char **argv) {
    bench::parse_arguments(argc, argv, SERPENT_CORPUS_DIR, SERPENT_BENCH_CONFIGURATION);
    print_configuration();

    const auto canada = read_file(bench::chosen.corpus + "/canada.json");
    const auto citm = read_file(bench::chosen.corpus + "/citm_catalog.json");
    const auto twitter = read_file(bench::chosen.corpus + "/twitter.json");
    const auto readings = sample_readings();

    if (!bench::chosen.list_only) check_results(readings, canada, citm, twitter);

    bench::row_order = { "serpent", "serpent (bounded)", "serpent (for size)", "serpent (table)", "serpent (indexed)", "glaze", "glaze (bounded)", "glaze (CBOR)",
        "glaze (size build)", "simdjson", "yyjson", "rapidjson", "nlohmann" };
    std::printf("\nmeasuring ");
    your_own_types(readings);
    documents(canada, twitter, citm);
    std::printf("\n");

    std::printf("\nbinary is BJData written for speed (serpent), BEVE (glaze) and CBOR (nlohmann, and glaze (CBOR)); allocations are "
                "counted decoding 10k records: serpent %.0f, glaze %.0f, nlohmann %.0f\n",
            bench::measured("10k records", "JSON decode", "serpent") ? bench::measured("10k records", "JSON decode", "serpent")->allocations : 0.0,
            bench::measured("10k records", "JSON decode", "glaze") ? bench::measured("10k records", "JSON decode", "glaze")->allocations : 0.0,
            bench::measured("10k records", "JSON decode", "nlohmann") ? bench::measured("10k records", "JSON decode", "nlohmann")->allocations : 0.0);
    bench::report();
    bench::write_json();
    return failures == 0 ? 0 : 1;
}
