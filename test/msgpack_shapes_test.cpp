// A described type's map is counted before its members are written, so it goes straight to the
// sink: the same bytes the gathered path produces, and the same bytes python-can sends. Members
// skipped, renamed or added by a discriminant all count; a tagged variant inside opens a map of
// its own without a count and is gathered within the counted one.

#include "check.hpp"

#include <serpent/msgpack.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace msgpack = serpent::msgpack;

namespace {

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto value : bytes) {
        out += digits[static_cast<unsigned>(value) >> 4];
        out += digits[static_cast<unsigned>(value) & 0xF];
    }
    return out;
}

struct [[= serpent::serializable {}]] can_frame {
    double timestamp = 0;
    std::uint32_t arbitration_id = 0;
    bool is_extended_id = false;
    bool is_remote_frame = false;
    bool is_error_frame = false;
    std::optional<std::variant<std::int64_t, std::string>> channel;
    std::uint8_t dlc = 0;
    std::vector<std::byte> data;
    bool is_fd = false;
    bool bitrate_switch = false;
    bool error_state_indicator = false;

    friend bool operator==(const can_frame &, const can_frame &) = default;
};

static_assert(requires(msgpack::writer &out, const can_frame &frame) { write_reflected(out, frame); },
        "a described type is written as a counted map, not gathered");

// What python-can 4.6.1 with msgpack 1.2.3 sent for this frame; see msgpack_test.cpp.
const std::string_view python_fd =
        "8ba974696d657374616d70cb3fe0000000000000ae6172626974726174696f6e5f6964cd0123ae69735f657874656e6465645f6964c2af6973"
        "5f72656d6f74655f6672616d65c2ae69735f6572726f725f6672616d65c2a76368616e6e656c01a3646c6303a464617461c403010203a56973"
        "5f6664c3ae626974726174655f737769746368c3b56572726f725f73746174655f696e64696361746f72c2";

const can_frame fd_frame { 0.5, 0x123, false, false, false, std::int64_t { 1 }, 3,
    { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } }, true, true, false };

struct [[= serpent::serializable {}]] trimmed {
    [[= serpent::key("n")]] int number = 7;
    [[= serpent::skip {}]] int cache = 0;
    std::string label = "x";
};

struct [[= serpent::discriminant("unit")]] celsius {
    double degrees = 0;
};

struct [[= serpent::serializable {}]] point {
    int x = 0;
    int y = 0;
};

struct [[= serpent::serializable {}]] circle {
    int radius = 0;
};

struct [[= serpent::serializable {}]] shape {
    [[= serpent::tagged("kind")]] std::variant<point, circle> body;
    std::vector<trimmed> parts;
};

struct [[= serpent::serializable {}]] sighting {
    int id = 0;
    [[= serpent::omit_empty {}]] std::optional<std::string> note {};
    std::optional<int> count {};
};

struct [[= serpent::serializable {}]] id_and_count {
    int id = 0;
    std::optional<int> count {};
};

/** Every map in the document, at whatever depth: all of them must hold what they say they hold. */
std::size_t count_members(const msgpack::reader &value) {
    std::size_t total = 0;
    if (value.is_object()) {
        for (const auto &entry : value.items()) total += 1 + count_members(entry.value);
    } else if (value.is_array() && !value.is_binary()) {
        for (const auto &element : value.array()) total += count_members(element);
    }
    return total;
}

} // namespace

int main() {
    // A member left out while empty is left out of the count too, so the map is still counted up front.
    check_equal(std::string_view { hex(msgpack::encode(sighting { .id = 1 })) },
            std::string_view { hex(msgpack::encode(id_and_count { .id = 1 })) },
            "an empty omit_empty member is neither written nor counted");
    const auto noted = msgpack::decode<sighting>(msgpack::encode(sighting { .id = 1, .note = "owl" }));
    check(noted && noted->note == "owl" && !noted->count.has_value(), "and one that holds something is both");

    check_equal(std::string_view { hex(msgpack::encode(fd_frame)) }, python_fd, "a described frame is python-can's bytes");
    check(msgpack::decode<can_frame>(from_hex(python_fd)) == fd_frame, "and python-can's bytes are the frame");

    // Counted, so not gathered: written into a fixed buffer through a writer whose sink lends.
    std::array<std::byte, 256> room {};
    serpent::span_sink fixed { room };
    check(msgpack::write(fixed, fd_frame).has_value(), "into a fixed buffer");
    check_equal(std::string_view { hex(fixed.written()) }, python_fd, "the same bytes there");

    check_equal(std::string_view { hex(msgpack::encode(trimmed {})) }, "82a16e07a56c6162656ca178",
            "a skipped member is not counted, a renamed one is");
    check_equal(std::string_view { hex(msgpack::encode(celsius { 1.5 })) },
            "82a4756e6974a763656c73697573a764656772656573cb3ff8000000000000", "a discriminant is counted");
    check(msgpack::decode<celsius>(msgpack::encode(celsius { 1.5 }))->degrees == 1.5, "and read back past");

    const shape drawn { circle { 4 }, { trimmed { 1, 0, "a" }, trimmed { 2, 0, "b" } } };
    const auto bytes = msgpack::encode(drawn);
    check(msgpack::validate(bytes).has_value(), "a tagged variant gathered inside a counted map");
    const auto root = msgpack::reader::over(bytes);
    check(root["body"]["kind"].as<std::string_view>() == "circle" && root["body"]["radius"].as<int>() == 4,
            "its tag and its members in one map");
    check_equal(count_members(root), std::size_t { 2 + 2 + 2 * 2 }, "every map holds what its header says");
    const auto again = msgpack::decode<shape>(bytes);
    check(again && std::get<circle>(again->body).radius == 4 && again->parts.size() == 2 && again->parts[1].label == "b",
            "and it reads back");

    return report("msgpack_shapes");
}
