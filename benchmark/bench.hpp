#pragma once

// The measuring half of the benchmark: timing by nanobench, allocation counts by a replaced
// operator new, and one record per measurement, printed as one table per workload with the
// libraries as rows and the operations as columns - the same rows and columns in every table.
//
//   serpent_bench [--corpus DIR] [--filter TEXT] [--json FILE] [--quick] [--verbose] [--list]
//
// --filter keeps the measurements whose workload, operation or library contains TEXT. --quick
// takes fewer samples, for checking that everything runs. --verbose shows nanobench's own
// table for every measurement as it is taken.

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
    bool verbose = false;
    bool list_only = false;
};

inline options chosen;

struct result {
    std::string workload;
    std::string operation;
    std::string library;
    bool allocations_visible = true;
    double nanoseconds = 0;
    double error_percent = 0;
    double allocations = 0;
};

inline std::vector<result> results;

/** A table's rows and columns, in the order first seen. */
inline std::vector<std::string> workloads, operations, libraries;

/** The order rows are printed in, where main() has one in mind; a library not named comes last. */
inline std::vector<std::string> row_order;

inline void note(std::vector<std::string> &into, std::string_view name) {
    if (std::ranges::find(into, name) == into.end()) into.emplace_back(name);
}

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
        else if (argument == "--verbose") chosen.verbose = true;
        else if (argument == "--list") chosen.list_only = true;
        else {
            std::fprintf(stderr, "usage: %s [--corpus DIR] [--filter TEXT] [--json FILE] [--quick] [--verbose] [--list]\n",
                    argv[0]);
            std::exit(argument == "--help" ? 0 : 2);
        }
    }
}

[[nodiscard]] inline bool wanted(std::string_view workload, std::string_view operation, std::string_view library) {
    if (chosen.filter.empty()) return true;
    const auto has = [](std::string_view text, std::string_view piece) { return text.find(piece) != std::string_view::npos; };
    return has(workload, chosen.filter) || has(operation, chosen.filter) || has(library, chosen.filter);
}

/**
 * Times one operation on one workload in one library, and counts what it allocates.
 *
 * The first library measured is the one the rest are shown relative to, so serpent goes first.
 * `payload_bytes` is the document size, for nanobench's throughput figure.
 */
template<typename Operation>
void measure(std::string_view workload, std::string_view operation, std::string_view library,
        std::size_t payload_bytes, Operation run, bool allocations_visible = true) {
    if (!wanted(workload, operation, library)) return;
    if (chosen.list_only) {
        std::printf("%.*s | %.*s | %.*s\n", static_cast<int>(workload.size()), workload.data(),
                static_cast<int>(operation.size()), operation.data(), static_cast<int>(library.size()), library.data());
        return;
    }

    ankerl::nanobench::Bench timing;
    timing.title(std::string { workload } + ", " + std::string { operation }).performanceCounters(false).warmup(3);
    timing.unit("byte").batch(std::max<std::size_t>(payload_bytes, 1));
    if (!chosen.verbose) timing.output(nullptr);
    if (chosen.quick) timing.epochs(3).minEpochTime(std::chrono::milliseconds { 10 });
    else timing.minEpochTime(std::chrono::milliseconds { 50 });
    timing.run(std::string { library }, [&] { ankerl::nanobench::doNotOptimizeAway(run()); });

    // Allocation totals come from a separate single pass, so counting never taxes the timing.
    allocation_count = 0;
    counting = true;
    ankerl::nanobench::doNotOptimizeAway(run());
    counting = false;

    using ankerl::nanobench::Result;
    const auto &timed = timing.results().back();
    note(workloads, workload);
    note(operations, operation);
    note(libraries, library);
    results.push_back({ std::string { workload }, std::string { operation }, std::string { library },
        allocations_visible, timed.median(Result::Measure::elapsed) * 1e9,
        timed.medianAbsolutePercentError(Result::Measure::elapsed) * 100, static_cast<double>(allocation_count) });
    if (!chosen.verbose) {
        std::printf(".");
        std::fflush(stdout);
    }
}

[[nodiscard]] inline const result *measured(std::string_view workload, std::string_view operation, std::string_view library) {
    const auto found = std::ranges::find_if(results, [&](const result &entry) {
        return entry.workload == workload && entry.operation == operation && entry.library == library;
    });
    return found == results.end() ? nullptr : &*found;
}

/**
 * One table per workload: a row per library, a column per operation, each cell the time in
 * microseconds and how many times the first row's it is. A cell no library-operation pair has
 * is a dash, so every table has the same shape.
 */
inline void report() {
    if (results.empty()) return;
    std::printf("\n");

    const auto cell = [](double microseconds) {
        char text[32];
        std::snprintf(text, sizeof text, microseconds < 10 ? "%.2f" : "%.0f", microseconds);
        return std::string { text };
    };
    const auto factor = [](double ratio) {
        char text[32];
        if (ratio >= 1000) std::snprintf(text, sizeof text, ">999x");
        else std::snprintf(text, sizeof text, "%.2fx", ratio);
        return std::string { text };
    };

    for (const auto &workload : workloads) {
        // The rows and columns this workload has, in the order they were measured.
        std::vector<std::string> columns, rows;
        for (const auto &entry : results) {
            if (entry.workload != workload) continue;
            note(columns, entry.operation);
            note(rows, entry.library);
        }
        std::ranges::stable_sort(rows, {}, [](const std::string &library) {
            return std::ranges::find(row_order, library) - row_order.begin();
        });

        std::printf("%s  (us, and times the first row)\n", workload.c_str());
        std::printf("  %-22s", "");
        for (const auto &operation : columns) std::printf(" %19s", operation.c_str());
        std::printf("\n");

        for (const auto &library : rows) {
            std::printf("  %-22s", library.c_str());
            for (const auto &operation : columns) {
                const auto *entry = measured(workload, operation, library);
                const auto *first = measured(workload, operation, rows.front());
                if (entry == nullptr) {
                    std::printf(" %19s", "-");
                } else if (first == nullptr || first == entry) {
                    std::printf(" %12s       ", cell(entry->nanoseconds / 1000).c_str());
                } else {
                    std::printf(" %12s %6s", cell(entry->nanoseconds / 1000).c_str(),
                            factor(entry->nanoseconds / first->nanoseconds).c_str());
                }
            }
            std::printf("\n");
        }
        std::printf("\n");
    }
}

/** The same records for tools/bench-summary.py, which joins them across configurations. */
inline void write_json() {
    if (chosen.json_path.empty()) return;

    std::ofstream out { chosen.json_path };
    out << "{\n  \"configuration\": \"" << chosen.configuration << "\",\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto &entry = results[index];
        out << "    {\"workload\": \"" << entry.workload << "\", \"operation\": \"" << entry.operation
            << "\", \"library\": \"" << entry.library << "\", \"ns\": " << entry.nanoseconds
            << ", \"error_percent\": " << entry.error_percent
            << ", \"allocations\": " << (entry.allocations_visible ? entry.allocations : -1) << "}"
            << (index + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
}

} // namespace bench
