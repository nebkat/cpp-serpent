#pragma once

// The measuring half of the benchmark: timing by nanobench, allocation counts by a replaced
// operator new, and one record per measurement that can be written out as JSON so that runs of
// differently configured builds can be laid side by side (tools/bench.sh does that).
//
//   serpent_bench [--corpus DIR] [--filter TEXT] [--json FILE] [--quick] [--list]
//
// --filter keeps the measurements whose group or library contains TEXT. --quick takes fewer
// samples, for checking that everything runs rather than for numbers to quote.

#include <nanobench.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

/** Live totals, maintained by the replaced global operator new / delete in main.cpp. */
inline std::size_t allocation_count = 0;
inline std::size_t allocation_bytes = 0;
inline bool counting = false;

struct options {
    std::string corpus;
    std::string filter;
    std::string json_path;
    std::string configuration;
    bool quick = false;
    bool list_only = false;
};

inline options chosen;

struct result {
    std::string group;
    std::string library;
    bool allocations_visible = true;
    double nanoseconds_per_operation = 0;
    double error_percent = 0;
    double megabytes_per_second = 0;
    double allocations_per_operation = 0;
    double bytes_allocated_per_operation = 0;
};

inline std::vector<result> results;

/** One nanobench table per group, so that each is printed relative to its own first row. */
inline std::map<std::string, std::unique_ptr<ankerl::nanobench::Bench>, std::less<>> tables;

inline void parse_arguments(int argc, char **argv, std::string_view default_corpus, std::string_view configuration) {
    chosen.corpus = default_corpus;
    chosen.configuration = configuration;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto value = [&]() -> std::string {
            if (index + 1 < argc) return argv[++index];
            std::fprintf(stderr, "%s needs a value\n", argv[index]);
            std::exit(2);
        };

        if (argument == "--corpus") chosen.corpus = value();
        else if (argument == "--filter") chosen.filter = value();
        else if (argument == "--json") chosen.json_path = value();
        else if (argument == "--quick") chosen.quick = true;
        else if (argument == "--list") chosen.list_only = true;
        else {
            std::fprintf(stderr,
                    "usage: %s [--corpus DIR] [--filter TEXT] [--json FILE] [--quick] [--list]\n", argv[0]);
            std::exit(argument == "--help" ? 0 : 2);
        }
    }
}

[[nodiscard]] inline bool wanted(std::string_view group, std::string_view library) {
    if (chosen.filter.empty()) return true;
    return group.find(chosen.filter) != std::string_view::npos || library.find(chosen.filter) != std::string_view::npos;
}

/**
 * Times one operation, and counts what it allocates.
 *
 * The first library measured in a group is the one the rest are shown relative to, so serpent
 * goes first. `payload_bytes` is the document size, from which the throughput comes.
 */
template<typename Operation>
void measure(std::string_view group, std::string_view library, std::size_t payload_bytes, Operation operation,
        bool allocations_visible = true) {
    if (!wanted(group, library)) return;
    if (chosen.list_only) {
        std::printf("%.*s | %.*s\n", static_cast<int>(group.size()), group.data(), static_cast<int>(library.size()),
                library.data());
        return;
    }

    auto &table = tables[std::string { group }];
    if (!table) {
        table = std::make_unique<ankerl::nanobench::Bench>();
        table->title(std::string { group }).relative(true).performanceCounters(false).warmup(3);
        table->unit("byte").batch(std::max<std::size_t>(payload_bytes, 1));
        if (chosen.quick) table->epochs(3).minEpochTime(std::chrono::milliseconds { 10 });
        else table->minEpochTime(std::chrono::milliseconds { 50 });
    }
    table->batch(std::max<std::size_t>(payload_bytes, 1));
    table->run(std::string { library }, [&] { ankerl::nanobench::doNotOptimizeAway(operation()); });

    // Allocation totals come from a separate single pass, so counting never taxes the timing.
    allocation_count = 0;
    allocation_bytes = 0;
    counting = true;
    ankerl::nanobench::doNotOptimizeAway(operation());
    counting = false;

    using ankerl::nanobench::Result;
    const auto &timed = table->results().back();
    const double seconds = timed.median(Result::Measure::elapsed);
    results.push_back({
            std::string { group },
            std::string { library },
            allocations_visible,
            seconds * 1e9,
            timed.medianAbsolutePercentError(Result::Measure::elapsed) * 100,
            payload_bytes > 0 ? static_cast<double>(payload_bytes) / seconds / (1024 * 1024) : 0,
            static_cast<double>(allocation_count),
            static_cast<double>(allocation_bytes),
    });
}

/** What nanobench's own tables do not show: allocations, and every group in one place. */
inline void report() {
    if (results.empty()) return;

    std::printf("\n%-40s %-18s %12s %7s %9s %10s %12s\n", "benchmark", "library", "ns/op", "+/-", "x first",
            "allocs", "bytes");
    std::printf("%s\n", std::string(114, '-').c_str());

    const result *first = nullptr;
    for (const auto &entry : results) {
        if (first == nullptr || first->group != entry.group) {
            if (first != nullptr) std::printf("\n");
            first = &entry;
        }
        // A library that allocates through malloc rather than operator new is invisible to the
        // counter, and printing its zero would read as "allocates nothing".
        const auto count = entry.allocations_visible
                ? std::to_string(static_cast<long long>(entry.allocations_per_operation))
                : std::string { "(malloc)" };
        const auto bytes = entry.allocations_visible
                ? std::to_string(static_cast<long long>(entry.bytes_allocated_per_operation))
                : std::string { "(malloc)" };
        std::printf("%-40s %-18s %12.0f %6.1f%% %8.2fx %10s %12s\n", entry.group.c_str(), entry.library.c_str(),
                entry.nanoseconds_per_operation, entry.error_percent,
                entry.nanoseconds_per_operation / first->nanoseconds_per_operation, count.c_str(), bytes.c_str());
    }
}

/** The same records for tools/bench-summary.py, which joins them across configurations. */
inline void write_json() {
    if (chosen.json_path.empty()) return;

    std::ofstream out { chosen.json_path };
    out << "{\n  \"configuration\": \"" << chosen.configuration << "\",\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto &entry = results[index];
        out << "    {\"group\": \"" << entry.group << "\", \"library\": \"" << entry.library
            << "\", \"ns\": " << entry.nanoseconds_per_operation << ", \"error_percent\": " << entry.error_percent
            << ", \"mb_per_s\": " << entry.megabytes_per_second << ", \"allocations\": "
            << (entry.allocations_visible ? entry.allocations_per_operation : -1) << ", \"allocated_bytes\": "
            << (entry.allocations_visible ? entry.bytes_allocated_per_operation : -1) << "}"
            << (index + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
}

} // namespace bench
