// Reading a document without allocating: strings and arrays are views into the bytes.

#include <serpent/bjdata.hpp>

#include <array>
#include <cstdio>
#include <span>

namespace bjdata = serpent::bjdata;

int main() {
    // Build a document in a fixed buffer. Nothing here reaches for the heap.
    std::array<std::byte, 128> storage {};
    serpent::span_sink out { storage };
    bjdata::writer writer { out };
    {
        const auto document = writer.object();
        document.member("id", "sensor-7");
        document.member("samples", std::vector<std::uint16_t> { 900, 901, 902, 903, 904, 905 });
    }
    const auto written = writer.finish();
    if (!written) {
        std::printf("did not fit: %s\n", written.error().what());
        return 1;
    }
    std::printf("wrote %zu bytes into a %zu byte buffer\n", *written, storage.size());

    // Read it back. The document is never inflated into an intermediate representation.
    const auto document = bjdata::view::over(out.written());

    const auto id = document["id"].as_string();
    if (!id) return 1;
    std::printf("id = %.*s\n", static_cast<int>(id->size()), id->data());

    // A string_view straight into the buffer: same address, no copy.
    const bool borrowed = id->data() >= reinterpret_cast<const char *>(storage.data())
            && id->data() < reinterpret_cast<const char *>(storage.data()) + storage.size();
    std::printf("that string points into the buffer: %s\n", borrowed ? "yes" : "no");

    // A typed array comes back as a span over those same bytes, indexable in place.
    const auto samples = document["samples"].as_span<std::uint16_t>();
    if (!samples) return 1;
    std::printf("%zu samples, first %u last %u\n", samples->size(), unsigned { samples->front() },
            unsigned { samples->back() });

    // Overflow is latched, never truncated: ask for more than fits and it says so.
    std::array<std::byte, 8> tiny {};
    serpent::span_sink small { tiny };
    bjdata::writer bounded { small };
    bounded.value(std::string("a string that will not fit in eight bytes"));
    std::printf("bounded write failed as expected: %s\n", bounded.finish() ? "no" : "yes");
    return bounded.finish() ? 1 : 0;
}
