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

struct reading {
    std::uint64_t timestamp = 0;
    std::string station;
    double celsius = 0;
    double humidity = 0;
    bool valid = false;

    SERPENT_DEFINE_TYPE(reading, timestamp, station, celsius, humidity, valid)
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(reading, timestamp, station, celsius, humidity, valid)
};

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
    std::printf("\ncorrectness (serpent vs other, same inputs)\n");

    const auto text = json::encode(values);
    agree("round-trip 10k records (JSON)", json::decode<std::vector<reading>>(text)->size(),
            other::parse(text).get<std::vector<reading>>().size());
    agree("our JSON parses as theirs", other::parse(text).get<std::vector<reading>>().at(9999).station,
            values.at(9999).station);
    agree("our BJData reads as theirs",
            other::from_bjdata(std::vector<std::uint8_t>(
                                       std::from_range, bjdata::encode(values) | std::views::transform([](std::byte b) {
                                           return static_cast<std::uint8_t>(b);
                                       })))
                    .get<std::vector<reading>>()
                    .at(4321)
                    .station,
            values.at(4321).station);

    {
        double ours = 0;
        auto document = json::reader::over(canada);
        for (auto feature : document["features"].array())
            for (auto ring : feature["geometry"]["coordinates"].array())
                for (auto point : ring.array())
                    for (auto number : point.array())
                        ours += number.as_float<double>().value_or(0);
        double theirs = 0;
        const auto parsed = other::parse(canada);
        for (const auto &feature : parsed["features"])
            for (const auto &ring : feature["geometry"]["coordinates"])
                for (const auto &point : ring)
                    for (const auto &number : point)
                        theirs += number.get<double>();
        agree_near("sum of canada.json coordinates", ours, theirs);
    }
    {
        std::uint64_t ours = 0;
        auto document = json::reader::over(twitter);
        for (auto status : document["statuses"].array())
            ours += status["id"].as_int<std::uint64_t>().value_or(0);
        std::uint64_t theirs = 0;
        const auto parsed = other::parse(twitter);
        for (const auto &status : parsed["statuses"])
            theirs += status["id"].get<std::uint64_t>();
        agree("sum of twitter.json ids", ours, theirs);
    }

    auto citm_document = json::reader::over(citm);
    agree("citm first-key lookup", citm_document["areaNames"]["205705994"].as_string().value_or("<none>"),
            other::parse(citm)["areaNames"]["205705994"].get<std::string>());
    agree("citm last-key lookup", citm_document["subjectNames"].is_object(), true);
    agree("twitter metadata lookup",
            json::reader::over(twitter)["search_metadata"]["count"].as_int<std::uint64_t>().value_or(0),
            other::parse(twitter)["search_metadata"]["count"].get<std::uint64_t>());
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
    bench::measure("decode 10k records (JSON)", "other", size,
            [&] { return other::parse(text).get<std::vector<reading>>().size(); });

    bench::measure("encode 10k records (JSON)", "serpent", size, [&] { return json::encode(values).size(); });
    bench::measure("encode 10k records (JSON)", "other", size, [&] { return other(values).dump().size(); });

    bench::measure("decode 10k records (BJData)", "serpent", bytes.size(),
            [&] { return bjdata::decode<std::vector<reading>>(bytes)->size(); });
    bench::measure("decode 10k records (BJData)", "other", other_bytes.size(),
            [&] { return other::from_bjdata(other_bytes).get<std::vector<reading>>().size(); });

    bench::measure(
            "encode 10k records (BJData)", "serpent", bytes.size(), [&] { return bjdata::encode(values).size(); });
    bench::measure("encode 10k records (BJData)", "other", other_bytes.size(),
            [&] { return other::to_bjdata(other(values)).size(); });
}

static void whole_document_scan(const std::string &canada, const std::string &twitter) {
    // Every coordinate in the document, added up: numeric parsing, deeply nested arrays.
    bench::measure("sum coordinates, canada.json", "serpent", canada.size(), [&] {
        double total = 0;
        auto document = json::reader::over(canada);
        for (auto feature : document["features"].array())
            for (auto ring : feature["geometry"]["coordinates"].array())
                for (auto point : ring.array())
                    for (auto number : point.array())
                        total += number.as_float<double>().value_or(0);
        return total;
    });
    bench::measure("sum coordinates, canada.json", "other", canada.size(), [&] {
        double total = 0;
        const auto document = other::parse(canada);
        for (const auto &feature : document["features"])
            for (const auto &ring : feature["geometry"]["coordinates"])
                for (const auto &point : ring)
                    for (const auto &number : point)
                        total += number.get<double>();
        return total;
    });

    // Strings and mixed objects rather than numbers.
    bench::measure("sum ids, twitter.json", "serpent", twitter.size(), [&] {
        std::uint64_t total = 0;
        auto document = json::reader::over(twitter);
        for (auto status : document["statuses"].array())
            total += status["id"].as_int<std::uint64_t>().value_or(0);
        return total;
    });
    bench::measure("sum ids, twitter.json", "other", twitter.size(), [&] {
        std::uint64_t total = 0;
        const auto document = other::parse(twitter);
        for (const auto &status : document["statuses"])
            total += status["id"].get<std::uint64_t>();
        return total;
    });
}

static void targeted_extraction(const std::string &citm, const std::string &twitter) {
    // Two scalars out of a 1.7 MB document. A DOM has to be built in full to reach them.
    bench::measure("two fields, citm_catalog.json", "serpent", citm.size(), [&] {
        auto document = json::reader::over(citm);
        const auto name = document["areaNames"]["205705994"].as_string();
        const auto count = document["events"]["138586341"]["id"].as_int<std::uint64_t>();
        return name.value_or("").size() + count.value_or(0);
    });
    bench::measure("two fields, citm_catalog.json", "other", citm.size(), [&] {
        const auto document = other::parse(citm);
        const auto name = document["areaNames"]["205705994"].get<std::string>();
        const auto count = document["events"]["138586341"]["id"].get<std::uint64_t>();
        return name.size() + count;
    });

    // areaNames is the first key in the document; subjectNames is the last. A reader that scans
    // lazily is at its best on the first and its worst on the last, and both belong in the table.
    bench::measure("last key, citm_catalog.json", "serpent", citm.size(), [&] {
        auto document = json::reader::over(citm);
        return document["subjectNames"].is_object();
    });
    bench::measure("last key, citm_catalog.json", "other", citm.size(),
            [&] { return other::parse(citm)["subjectNames"].is_object(); });

    bench::measure("one field, twitter.json", "serpent", twitter.size(), [&] {
        auto document = json::reader::over(twitter);
        return document["search_metadata"]["count"].as_int<std::uint64_t>().value_or(0);
    });
    bench::measure("one field, twitter.json", "other", twitter.size(),
            [&] { return other::parse(twitter)["search_metadata"]["count"].get<std::uint64_t>(); });
}

static void full_read(const std::string &citm) {
    // The closest thing to like-for-like: read every value in the document and keep nothing.
    bench::measure("walk every value, citm_catalog.json", "serpent", citm.size(),
            [&] { return json::validate(citm).has_value(); });
    bench::measure(
            "walk every value, citm_catalog.json", "other", citm.size(), [&] { return other::parse(citm).size(); });
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
    whole_document_scan(canada, twitter);
    targeted_extraction(citm, twitter);
    full_read(citm);

    bench::report();
    return 0;
}
