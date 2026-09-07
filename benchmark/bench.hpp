#pragma once

// A small timing and allocation harness. Deliberately self-contained: the library has no
// dependencies and the benchmarks should not add one to measure it.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

/** Live totals, maintained by the replaced global operator new / delete in main.cpp. */
inline std::size_t allocation_count = 0;
inline std::size_t allocation_bytes = 0;
inline bool counting = false;

struct result {
    std::string group;
    std::string library;
    bool allocations_visible = true;
    double nanoseconds_per_operation = 0;
    double megabytes_per_second = 0;
    double allocations_per_operation = 0;
    double bytes_allocated_per_operation = 0;
};

inline std::vector<result> results;

/**
 * Times one operation.
 *
 * Reports the best of several rounds rather than the mean: the fastest round is the one least
 * disturbed by the scheduler, and it is the more stable number to compare across runs.
 * `payload_bytes` is the document size, used only to derive a throughput figure.
 */
template<typename Operation>
void measure(std::string_view group, std::string_view library, std::size_t payload_bytes, Operation operation,
        bool allocations_visible = true) {
    constexpr int rounds = 7;
    constexpr auto target = std::chrono::milliseconds { 120 };

    for (int warmup = 0; warmup < 3; ++warmup)
        operation();

    // Size the round from a trial pass so every entry runs for roughly the same wall time.
    const auto trial_start = std::chrono::steady_clock::now();
    operation();
    const auto trial = std::chrono::steady_clock::now() - trial_start;
    const auto per_round =
            std::max<std::size_t>(1, static_cast<std::size_t>(target / std::max(trial, decltype(trial) { 1 })));

    double best = 1e300;
    for (int round = 0; round < rounds; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < per_round; ++iteration)
            operation();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto each = std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(per_round);
        best = std::min(best, each);
    }

    // Allocation totals come from a separate single pass, so counting never taxes the timing.
    allocation_count = 0;
    allocation_bytes = 0;
    counting = true;
    operation();
    counting = false;

    results.push_back({
            std::string { group },
            std::string { library },
            allocations_visible,
            best,
            payload_bytes > 0 ? static_cast<double>(payload_bytes) / best * 1e9 / (1024 * 1024) : 0,
            static_cast<double>(allocation_count),
            static_cast<double>(allocation_bytes),
    });
}

inline void report() {
    std::printf("\n%-36s %-12s %12s %10s %10s %12s\n", "benchmark", "library", "ns/op", "MB/s", "allocs", "bytes");
    std::printf("%s\n", std::string(96, '-').c_str());

    std::string current;
    for (const auto &entry : results) {
        if (entry.group != current) {
            if (!current.empty()) std::printf("\n");
            current = entry.group;
        }
        // A library that allocates through malloc rather than operator new is invisible to the
        // counter, and printing its zero would read as "allocates nothing".
        if (entry.allocations_visible) {
            std::printf("%-36s %-12s %12.0f %10.1f %10.0f %12.0f\n", entry.group.c_str(), entry.library.c_str(),
                    entry.nanoseconds_per_operation, entry.megabytes_per_second, entry.allocations_per_operation,
                    entry.bytes_allocated_per_operation);
        } else {
            std::printf("%-36s %-12s %12.0f %10.1f %10s %12s\n", entry.group.c_str(), entry.library.c_str(),
                    entry.nanoseconds_per_operation, entry.megabytes_per_second, "(malloc)", "(malloc)");
        }
    }

    // Every library in a group, relative to serpent. Above 1.00 means serpent is ahead.
    std::printf("\n%-36s %-12s %10s %14s\n", "relative to serpent", "library", "time", "allocations");
    std::printf("%s\n", std::string(76, '-').c_str());

    current.clear();
    for (const auto &entry : results) {
        if (entry.library == "serpent") continue;

        const auto ours = std::find_if(results.begin(), results.end(), [&](const result &candidate) {
            return candidate.group == entry.group && candidate.library == "serpent";
        });
        if (ours == results.end()) continue;

        if (entry.group != current) {
            if (!current.empty()) std::printf("\n");
            current = entry.group;
        }
        const auto allocations = !entry.allocations_visible ? std::string { "(malloc)" }
                : ours->allocations_per_operation > 0
                ? std::to_string(static_cast<long>(entry.allocations_per_operation / ours->allocations_per_operation))
                        + "x"
                : std::to_string(static_cast<long>(entry.allocations_per_operation)) + " vs 0";
        std::printf("%-36s %-12s %9.2fx %13s\n", entry.group.c_str(), entry.library.c_str(),
                entry.nanoseconds_per_operation / ours->nanoseconds_per_operation, allocations.c_str());
    }
}

} // namespace bench
