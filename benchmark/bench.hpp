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
void measure(std::string_view group, std::string_view library, std::size_t payload_bytes, Operation operation) {
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
            best,
            payload_bytes > 0 ? static_cast<double>(payload_bytes) / best * 1e9 / (1024 * 1024) : 0,
            static_cast<double>(allocation_count),
            static_cast<double>(allocation_bytes),
    });
}

inline void report() {
    std::printf("\n%-34s %-10s %12s %10s %10s %12s\n", "benchmark", "library", "ns/op", "MB/s", "allocs", "bytes");
    std::printf("%s\n", std::string(92, '-').c_str());

    std::string current;
    for (const auto &entry : results) {
        if (entry.group != current) {
            if (!current.empty()) std::printf("\n");
            current = entry.group;
        }
        std::printf("%-34s %-10s %12.0f %10.1f %10.0f %12.0f\n", entry.group.c_str(), entry.library.c_str(),
                entry.nanoseconds_per_operation, entry.megabytes_per_second, entry.allocations_per_operation,
                entry.bytes_allocated_per_operation);
    }

    // Speedups, paired by group. Both libraries must have run for a ratio to mean anything.
    std::printf("\n%-34s %14s %14s\n", "ratio (other / serpent)", "time", "allocations");
    std::printf("%s\n", std::string(64, '-').c_str());
    for (std::size_t index = 0; index + 1 < results.size(); ++index) {
        const auto &a = results[index];
        const auto &b = results[index + 1];
        if (a.group != b.group || a.library == b.library) continue;
        const auto &ours = a.library == "serpent" ? a : b;
        const auto &theirs = a.library == "serpent" ? b : a;
        std::printf("%-34s %13.2fx %13.2fx\n", a.group.c_str(),
                theirs.nanoseconds_per_operation / ours.nanoseconds_per_operation,
                ours.allocations_per_operation > 0 ? theirs.allocations_per_operation / ours.allocations_per_operation
                                                   : theirs.allocations_per_operation);
    }
}

} // namespace bench
