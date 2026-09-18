// Every handle keeps its own note of how far a walk of it got, and nothing is shared between
// traversals - not between two on one thread, not between threads. Traversals on several
// threads at once, over different documents and over the same one, must each get the answer a
// lone traversal gets.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/value.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;

namespace {

/** A document of nested arrays and objects with a number at every leaf, whose sum is known. */
std::string document(int seed, std::uint64_t &sum) {
    std::string text = "{";
    sum = 0;
    for (int outer = 0; outer < 12; ++outer) {
        text += (outer ? ",\"k" : "\"k") + std::to_string(outer) + "\":[";
        for (int inner = 0; inner < 12; ++inner) {
            const std::uint64_t leaf = static_cast<std::uint64_t>(seed * 1000 + outer * 12 + inner);
            sum += leaf * 2;
            text += (inner ? ",{\"a\":[" : "{\"a\":[") + std::to_string(leaf) + ",{\"b\":" + std::to_string(leaf) + "}]}";
        }
        text += "]";
    }
    return text + "}";
}

std::uint64_t sum_json(const json::reader &value) {
    std::uint64_t total = 0;
    if (const auto number = value.as<std::uint64_t>()) return *number;
    if (value.is_array())
        for (const auto &child : value.array()) total += sum_json(child);
    else if (value.is_object())
        for (const auto &member : value.items()) total += sum_json(member.value);
    return total;
}

std::uint64_t sum_binary(const bjdata::reader &value) {
    std::uint64_t total = 0;
    if (const auto number = value.as<std::uint64_t>()) return *number;
    if (value.is_array())
        for (const auto &child : value.array()) total += sum_binary(child);
    else if (value.is_object())
        for (const auto &member : value.items()) total += sum_binary(member.value);
    return total;
}

} // namespace

int main() {
    constexpr int threads = 8;
    constexpr int rounds = 200;

    std::vector<std::string> texts;
    std::vector<std::vector<std::byte>> binaries;
    std::vector<std::uint64_t> sums;
    for (int seed = 0; seed < threads; ++seed) {
        std::uint64_t sum = 0;
        texts.push_back(document(seed, sum));
        sums.push_back(sum);
        binaries.push_back(bjdata::encode(json::decode<serpent::value>(texts.back()).value()));
    }
    for (int seed = 0; seed < threads; ++seed) {
        check_equal(sum_json(json::reader::over(texts[seed])), sums[seed], "a lone traversal of the text");
        check_equal(sum_binary(bjdata::reader::over(binaries[seed])), sums[seed], "and of the bytes");
    }

    // Each thread walks its own document and, every other round, the first one - so notes about
    // different documents and about the same document race through the one memo.
    std::vector<int> wrong(threads, 0);
    std::vector<std::thread> workers;
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back([&, index] {
            for (int round = 0; round < rounds; ++round) {
                const int which = round % 2 == 0 ? index : 0;
                if (sum_json(json::reader::over(texts[which])) != sums[which]) ++wrong[index];
                if (sum_binary(bjdata::reader::over(binaries[which])) != sums[which]) ++wrong[index];
            }
        });
    }
    for (auto &worker : workers) worker.join();
    for (int index = 0; index < threads; ++index)
        check_equal(wrong[index], 0, "every traversal on every thread got the lone traversal's answer");

    return report("traversals_apart");
}
