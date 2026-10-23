// MessagePack: the bytes each thing the writer can be asked for comes out as, what the reader
// makes of every format byte, documents cut short at every length, and the frames python-can
// sends over UDP multicast, byte for byte.

#include "check.hpp"

#include <serpent/msgpack.hpp>
#include <serpent/value.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace serpent;
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

template<typename Body>
std::vector<std::byte> emit(Body body) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    msgpack::writer target { out };
    body(target);
    const auto result = target.finish();
    check(result.has_value(), "the writer finished cleanly");
    if (result) check_equal(*result, buffer.size(), "reported size matches the bytes produced");
    return buffer;
}

void produces(std::string_view expected, auto body, std::string_view what) {
    check_equal(std::string_view { hex(emit(body)) }, expected, what);
}

/** The error a body leaves the writer with. */
errc failure_of(auto body) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    msgpack::writer target { out };
    body(target);
    const auto result = target.finish();
    return result ? errc::ok : result.error().code();
}

/**
 * A CAN frame as python-can's UDP multicast interface sends it: one map per datagram, packed by
 * msgpack.packb(..., use_bin_type=True) and unpacked straight into can.Message(**fields). Written
 * with json_convert, so its map is opened without a count and gathered.
 */
struct can_frame {
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

    friend void json_convert(auto &visitor, conversion_object_t<decltype(visitor), can_frame> item) {
        visitor.member("timestamp", item.timestamp);
        visitor.member("arbitration_id", item.arbitration_id);
        visitor.member("is_extended_id", item.is_extended_id);
        visitor.member("is_remote_frame", item.is_remote_frame);
        visitor.member("is_error_frame", item.is_error_frame);
        visitor.member("channel", item.channel);
        visitor.member("dlc", item.dlc);
        visitor.member("data", item.data);
        visitor.member("is_fd", item.is_fd);
        visitor.member("bitrate_switch", item.bitrate_switch);
        visitor.member("error_state_indicator", item.error_state_indicator);
    }
};

std::vector<std::byte> payload(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) out.push_back(static_cast<std::byte>(value));
    return out;
}

// What python-can 4.6.1 with msgpack 1.2.3 sent for each, from can.interfaces.udp_multicast.utils.pack_message.
const std::string_view python_extended =
        "8ba974696d657374616d70cb41d954fc40100000ae6172626974726174696f6e5f6964ce18fef100ae69735f657874656e6465645f6964c3"
        "af69735f72656d6f74655f6672616d65c2ae69735f6572726f725f6672616d65c2a76368616e6e656cc0a3646c6308a464617461c408000102"
        "0304050607a569735f6664c2ae626974726174655f737769746368c2b56572726f725f73746174655f696e64696361746f72c2";
const std::string_view python_fd =
        "8ba974696d657374616d70cb3fe0000000000000ae6172626974726174696f6e5f6964cd0123ae69735f657874656e6465645f6964c2af6973"
        "5f72656d6f74655f6672616d65c2ae69735f6572726f725f6672616d65c2a76368616e6e656c01a3646c6303a464617461c403010203a56973"
        "5f6664c3ae626974726174655f737769746368c3b56572726f725f73746174655f696e64696361746f72c2";
const std::string_view python_named =
        "8ba974696d657374616d70cb0000000000000000ae6172626974726174696f6e5f6964cd07ffae69735f657874656e6465645f6964c2af6973"
        "5f72656d6f74655f6672616d65c2ae69735f6572726f725f6672616d65c2a76368616e6e656ca463616e30a3646c6300a464617461c400a569"
        "735f6664c2ae626974726174655f737769746368c2b56572726f725f73746174655f696e64696361746f72c2";

const can_frame extended_frame { 1700000000.25, 0x18FEF100, true, false, false, std::nullopt, 8,
    payload({ 0, 1, 2, 3, 4, 5, 6, 7 }), false, false, false };
const can_frame fd_frame { 0.5, 0x123, false, false, false, std::int64_t { 1 }, 3, payload({ 1, 2, 3 }), true, true, false };
const can_frame named_frame { 0.0, 0x7ff, false, false, false, std::string { "can0" }, 0, {}, false, false, false };

void scalars() {
    produces("c0", [](auto &w) { w.null(); }, "nil");
    produces("c3", [](auto &w) { w.value(true); }, "true");
    produces("c2", [](auto &w) { w.value(false); }, "false");

    produces("00", [](auto &w) { w.value(0); }, "positive fixint 0");
    produces("7f", [](auto &w) { w.value(127); }, "positive fixint 127");
    produces("cc80", [](auto &w) { w.value(128); }, "uint 8");
    produces("ccff", [](auto &w) { w.value(255); }, "uint 8 at its top");
    produces("cd0100", [](auto &w) { w.value(256); }, "uint 16");
    produces("cdffff", [](auto &w) { w.value(65535); }, "uint 16 at its top");
    produces("ce00010000", [](auto &w) { w.value(65536); }, "uint 32");
    produces("cf0000000100000000", [](auto &w) { w.value(std::int64_t { 1 } << 32); }, "uint 64");
    produces("cfffffffffffffffff", [](auto &w) { w.value(std::numeric_limits<std::uint64_t>::max()); }, "uint 64 at its top");
    produces("05", [](auto &w) { w.value(std::uint8_t { 5 }); }, "a uint8 is narrowed like any other");

    produces("ff", [](auto &w) { w.value(-1); }, "negative fixint -1");
    produces("e0", [](auto &w) { w.value(-32); }, "negative fixint -32");
    produces("d0df", [](auto &w) { w.value(-33); }, "int 8");
    produces("d080", [](auto &w) { w.value(-128); }, "int 8 at its bottom");
    produces("d1ff7f", [](auto &w) { w.value(-129); }, "int 16");
    produces("d18000", [](auto &w) { w.value(-32768); }, "int 16 at its bottom");
    produces("d2ffff7fff", [](auto &w) { w.value(-32769); }, "int 32");
    produces("d38000000000000000", [](auto &w) { w.value(std::numeric_limits<std::int64_t>::min()); }, "int 64");

    produces("cb3ff8000000000000", [](auto &w) { w.value(1.5); }, "a double is a float 64");
    produces("ca3fc00000", [](auto &w) { w.value(1.5f); }, "a float is a float 32");
    produces("cb3fb999999999999a", [](auto &w) { w.real(0.1); }, "real() is a float 64");

    produces("a0", [](auto &w) { w.value(std::string_view {}); }, "empty fixstr");
    produces("a3616263", [](auto &w) { w.value("abc"); }, "fixstr");
    produces("a178", [](auto &w) { w.character('x'); }, "a character is a one-byte str");
    produces("bf" + hex(std::as_bytes(std::span { std::string_view { std::string(31, 'a') } })),
            [](auto &w) { w.value(std::string(31, 'a')); }, "fixstr at its top");
    produces("d920" + hex(std::as_bytes(std::span { std::string_view { std::string(32, 'a') } })),
            [](auto &w) { w.value(std::string(32, 'a')); }, "str 8");
    produces("da0100" + hex(std::as_bytes(std::span { std::string_view { std::string(256, 'a') } })),
            [](auto &w) { w.value(std::string(256, 'a')); }, "str 16");
    produces("db00010000" + hex(std::as_bytes(std::span { std::string_view { std::string(65536, 'a') } })),
            [](auto &w) { w.value(std::string(65536, 'a')); }, "str 32");

    produces("c400", [](auto &w) { w.value(std::vector<std::byte> {}); }, "empty bin 8");
    produces("c4020102", [](auto &w) { w.value(payload({ 1, 2 })); }, "bin 8");
    produces("c50100" + hex(std::vector<std::byte>(256, std::byte { 7 })),
            [](auto &w) { w.value(std::vector<std::byte>(256, std::byte { 7 })); }, "bin 16");
    produces("c4020102", [](auto &w) { w.value(std::list<std::byte> { std::byte { 1 }, std::byte { 2 } }); },
            "bytes that are not contiguous are bin too");

    produces("d40501", [](auto &w) { w.extension(5, payload({ 1 })); }, "fixext 1");
    produces("d6ff5f5e1000", [](auto &w) { w.extension(msgpack::timestamp_type, payload({ 0x5f, 0x5e, 0x10, 0x00 })); },
            "fixext 4: a 32-bit timestamp");
    produces("c703050a0b0c", [](auto &w) { w.extension(5, payload({ 10, 11, 12 })); }, "ext 8");
    produces("c70005", [](auto &w) { w.extension(5, {}); }, "an empty ext 8");
}

void containers() {
    produces("90", [](auto &w) { w.value(std::vector<int> {}); }, "empty fixarray");
    produces("93010203", [](auto &w) { w.value(std::vector<int> { 1, 2, 3 }); }, "fixarray");
    produces("dc0010" + std::string(32, '0'), [](auto &w) { w.value(std::vector<int>(16, 0)); }, "array 16");
    using mixed = std::variant<int, std::string, std::nullptr_t>;
    produces("9301a161c0", [](auto &w) { w.value(std::vector<mixed> { 1, std::string { "a" }, nullptr }); },
            "elements of any kind");
    produces("92c3c2", [](auto &w) { w.value(std::list<bool> { true, false }); }, "a range that is not contiguous is still counted");

    // Opened without a count: gathered, and the header written in front once it is known.
    produces("80", [](auto &w) { const auto scope = w.object(); }, "an empty map");
    produces("82a16101a16202",
            [](auto &w) {
                const auto scope = w.object();
                scope.member("a", 1);
                scope.member("b", 2);
            },
            "fixmap");
    produces("82a16101a16202", [](auto &w) { w.value(std::map<std::string, int> { { "a", 1 }, { "b", 2 } }); },
            "a map from text");
    produces("92a161a162",
            [](auto &w) {
                const auto scope = w.array();
                scope.value("a");
                scope.value("b");
            },
            "an array opened without a count");

    // Sixteen members is one more than a fixmap holds, so the gap is filled with a map 16.
    std::string sixteen = "de0010";
    for (int index = 0; index < 16; ++index) sixteen += "a1" + hex(payload({ 'a' + index })) + hex(payload({ index }));
    produces(sixteen,
            [](auto &w) {
                const auto scope = w.object();
                for (int index = 0; index < 16; ++index) {
                    const char name = static_cast<char>('a' + index);
                    scope.member(std::string_view { &name, 1 }, index);
                }
            },
            "map 16 when the count turns out to need it");

    produces("82a1619301029102a16281a163c0",
            [](auto &w) {
                const auto outer = w.object();
                w.key("a");
                {
                    const auto inner = w.array();
                    inner.value(1);
                    inner.value(2);
                    const auto deeper = w.array();
                    deeper.value(2);
                }
                w.key("b");
                {
                    const auto counted = w.object(1);
                    counted.member("c", nullptr);
                }
            },
            "nested: every gap filled in its place, counted and uncounted together");

    produces("9281a17801c0",
            [](auto &w) {
                const auto counted = w.array(2);
                {
                    const auto gathered = w.object();
                    gathered.member("x", 1);
                }
                w.null();
            },
            "a gathered container inside a counted one is released on closing");

    produces("01c0a161",
            [](auto &w) {
                w.value(1);
                w.null();
                w.value("a");
            },
            "values one after another at the top: a stream");

    // Splicing a value from another document, as it stands.
    const auto source = emit([](auto &w) {
        const auto scope = w.object();
        scope.member("keep", std::vector<int> { 1, 2 });
    });
    produces("91920102",
            [&](auto &w) {
                const auto scope = w.array();
                msgpack::write_value(w, msgpack::reader::over(source)["keep"]);
            },
            "a value spliced from another document");
}

void misuse() {
    check(failure_of([](auto &w) {
        const auto scope = w.array(2);
        scope.value(1);
    }) == errc::unbalanced_container,
            "a counted array closed short");
    check(failure_of([](auto &w) {
        const auto scope = w.object(1);
        scope.member("a", 1);
        scope.member("b", 2);
    }) == errc::unbalanced_container,
            "a counted map closed long");
    check(failure_of([](auto &w) { w.key("a"); }) == errc::key_outside_object, "a key with no map open");
    check(failure_of([](auto &w) {
        const auto scope = w.object();
        w.value(1);
    }) == errc::key_outside_object,
            "a value in a map with no key before it");
    check(failure_of([](auto &w) {
        const auto scope = w.object();
        w.key("a");
        w.key("b");
    }) == errc::unbalanced_container,
            "two keys in a row");
    check(failure_of([](auto &w) {
        const auto scope = w.object();
        w.key("a");
    }) == errc::unbalanced_container,
            "a map closed on a key with no value");
    {
        // Finished while a gathered container is still open: nothing of it reached the sink.
        std::vector<std::byte> buffer;
        container_sink out { buffer };
        msgpack::writer target { out };
        const auto scope = target.array();
        scope.value(1);
        const auto result = target.finish();
        check(!result && result.error().code() == errc::unterminated_container, "an unclosed container is reported");
    }

    // A fixed buffer too small for the document, with a gathered container that only reaches the
    // sink once it closes.
    std::array<std::byte, 8> small {};
    span_sink fixed { small };
    msgpack::writer target { fixed };
    {
        const auto scope = target.object();
        scope.member("much too long for eight bytes", 1);
    }
    const auto result = target.finish();
    check(!result && result.error().code() == errc::sink_failed, "a full fixed buffer");
}

void sinks() {
    // Written straight into a fixed buffer lent by the sink: counted, gathered, and both.
    std::array<std::byte, 256> room {};
    span_sink fixed { room };
    const auto written = msgpack::write(fixed, fd_frame);
    check(written.has_value(), "a frame written into a fixed buffer");
    check_equal(std::string_view { hex(fixed.written()) }, python_fd, "and the bytes are python-can's");

    std::vector<std::byte> collected;
    auto append = [&](std::span<const std::byte> bytes) { collected.insert(collected.end(), bytes.begin(), bytes.end()); };
    msgpack::writer target { append };
    target.value(named_frame);
    check(target.finish().has_value(), "a frame written to a callable");
    check_equal(std::string_view { hex(collected) }, python_named, "and the bytes are python-can's");

    check_equal(msgpack::measure(extended_frame), from_hex(python_extended).size(), "measure() says how long it will be");
}

void python_can() {
    // Gathered and written in front: the same bytes python-can produces for the same frame.
    check_equal(std::string_view { hex(msgpack::encode(extended_frame)) }, python_extended, "an extended frame, no channel");
    check_equal(std::string_view { hex(msgpack::encode(fd_frame)) }, python_fd, "an FD frame on channel 1");
    check_equal(std::string_view { hex(msgpack::encode(named_frame)) }, python_named, "an empty frame on channel \"can0\"");

    // And what python-can sends reads back as the frame.
    check(msgpack::decode<can_frame>(from_hex(python_extended)) == extended_frame, "read: extended");
    check(msgpack::decode<can_frame>(from_hex(python_fd)) == fd_frame, "read: FD");
    check(msgpack::decode<can_frame>(from_hex(python_named)) == named_frame, "read: named channel");
    check(msgpack::try_decode<can_frame>(from_hex(python_fd)).has_value(), "try_decode agrees");
}

void reading() {
    const auto read = [](std::string_view text) { return from_hex(text); };

    check(msgpack::reader::over(read("c0")).is_null(), "nil");
    check(msgpack::decode<bool>(read("c3")) == true, "true");
    check(msgpack::decode<int>(read("d0df")) == -33, "int 8");
    check(msgpack::decode<std::uint64_t>(read("cfffffffffffffffff")) == std::numeric_limits<std::uint64_t>::max(), "uint 64");
    check(msgpack::decode<std::int64_t>(read("d38000000000000000")) == std::numeric_limits<std::int64_t>::min(), "int 64");
    check(msgpack::decode<std::int64_t>(read("ce00010000")) == 65536, "an unsigned form read as signed");
    check(!msgpack::decode<std::uint8_t>(read("cd0100")), "256 does not fit a uint8");
    check(!msgpack::decode<unsigned>(read("ff")), "-1 does not fit an unsigned");
    check(!msgpack::decode<std::int64_t>(read("cfffffffffffffffff")), "nor does 2^64-1 fit an int64");
    check(msgpack::decode<double>(read("ca3fc00000")) == 1.5, "float 32");
    check(msgpack::decode<double>(read("cb3ff8000000000000")) == 1.5, "float 64");
    check(msgpack::decode<double>(read("d0df")) == -33.0, "an integer read as a real");
    check(!msgpack::decode<int>(read("cb3ff8000000000000")), "a real is not read as an integer");
    check(msgpack::decode<std::string_view>(read("a3616263")) == "abc", "fixstr");
    check(msgpack::decode<std::string>(read("d903616263")) == "abc", "str 8, copied");
    check(msgpack::decode<char>(read("a178")) == 'x', "a one-byte str as a char");

    const auto bin = read("c403010203");
    const auto bytes = msgpack::reader::over(bin);
    check(bytes.is_binary() && bytes.is_array() && bytes.type() == kind::array, "bin reads as an array of bytes");
    check_equal(bytes.size(), std::size_t { 3 }, "of its length");
    const auto view = bytes.as<std::span<const std::byte>>();
    check(view && view->size() == 3 && view->data() == bin.data() + 2, "viewed in place");
    int expected = 1;
    for (const auto &element : bytes.array()) {
        check(element.as<int>() == expected, "walked a byte at a time");
        check(!element.encoded(), "a byte of a bin is no value of its own to splice");
        ++expected;
    }
    check_equal(expected, 4, "every byte");
    check(bytes[2].as<int>() == 3 && !bytes[3].is_valid(), "indexed by arithmetic");
    check(msgpack::decode<std::vector<std::byte>>(bin) == payload({ 1, 2, 3 }), "into a byte vector");
    check(msgpack::decode<std::vector<int>>(bin) == std::vector<int> { 1, 2, 3 }, "or into a vector of numbers");

    // A run of numbers is read straight into its container, each at whatever width it was written.
    check(msgpack::decode<std::vector<int>>(read("9301d0dfcd0100")) == std::vector<int> { 1, -33, 256 }, "numbers of every width");
    check(msgpack::decode<std::vector<double>>(read("92ca3fc0000005")) == std::vector<double> { 1.5, 5.0 },
            "reals, and an integer among them");
    check(!msgpack::decode<std::vector<std::uint8_t>>(read("9201cd0100")), "one that does not fit fails the whole");
    check(!msgpack::decode<std::vector<int>>(read("9201a161")), "as does one that is not a number");
    check(msgpack::decode<std::vector<int>>(read("90"))->empty(), "an empty array is an empty container");

    const auto ext_bytes = read("c703050a0b0c");
    const auto ext = msgpack::reader::over(ext_bytes);
    check(ext.is_extension() && !ext.is_valid() && ext.type() == kind::invalid, "an ext has no kind");
    const auto extension = ext.extension();
    check(extension && extension->type == 5 && extension->data.size() == 3, "its type and bytes");

    // A map whose keys are not text is valid MessagePack with no members to name.
    const auto keyed = read("82a16101" "0102");
    check(msgpack::validate(keyed).has_value(), "a map keyed by an integer validates");
    std::size_t named = 0;
    for ([[maybe_unused]] const auto &entry : msgpack::reader::over(keyed).items()) ++named;
    check_equal(named, std::size_t { 1 }, "and its members stop at the first key that is not a str");

    // Everything stepped over on the way to what was asked for: ext, bin, nested, in a map.
    const auto document = read("84a165d6ff5f5e1000a162c4020102a16e92" "81a17891c0" "93010203" "a17acb3ff8000000000000");
    const auto root = msgpack::reader::over(document);
    check(root["z"].as<double>() == 1.5, "a member after an ext, a bin and a nesting");
    check(root["n"][0]["x"][0].is_null(), "deep inside");
    check(root["n"][1][2].as<int>() == 3, "and beside it");
    check(root.size() == 4 && root.size_hint() == 4, "a map's count, without walking it");
    check(!root["missing"].is_valid() && !root[0].is_valid(), "no such member, and a map is not indexed");

    std::vector<int> sums;
    for (const auto &entry : root.items()) {
        if (entry.key != "n") continue;
        for (const auto &element : entry.value.array()) {
            int sum = 0;
            for (const auto &item : element.array())
                if (const auto number = item.as<int>()) sum += *number;
            sums.push_back(sum);
        }
    }
    check(sums == std::vector<int> { 0, 6 }, "walked by loops nested inside one another");

    // A tree has no place for an ext, so a document holding one is not a tree; one without is,
    // and what is read is what is written.
    check(!root.as<value>().has_value(), "a document with an ext in it is no tree");
    const auto plain = read("83a162c4020102a16e9281a17891c093010203a17acb3ff8000000000000");
    const auto tree = msgpack::reader::over(plain).as<value>();
    check(tree.has_value(), "into a tree");
    if (tree) {
        std::vector<std::byte> again;
        container_sink out { again };
        msgpack::writer target { out };
        target.value(*tree);
        check(target.finish().has_value(), "and the tree written out");
        const auto reread = msgpack::reader::over(again);
        check(reread["n"][1][2].as<int>() == 3 && reread["z"].as<double>() == 1.5, "reads the same");
        check(reread["b"].as<std::span<const std::byte>>().has_value(), "binary stays binary");
    }
}

void malformed() {
    const auto read = [](std::string_view text) { return from_hex(text); };

    check(msgpack::validate(read("c1")).error().code() == errc::invalid_marker, "the one byte that is never used");
    check(msgpack::validate({}).error().code() == errc::unexpected_end, "nothing at all");
    check(msgpack::validate(read("c0c0")).error().code() == errc::trailing_data, "two values where one was expected");
    check(msgpack::validate(read("ddffffffff00")).error().code() == errc::unexpected_end,
            "a count no buffer could hold is refused before it is walked");
    std::size_t found = 0;
    const auto overstated = read("ddffffffff00");
    for ([[maybe_unused]] const auto &element : msgpack::reader::over(overstated).array()) ++found;
    check_equal(found, std::size_t { 0 }, "and walking it finds nothing");

    std::string deep;
    for (int level = 0; level <= max_depth; ++level) deep += "91";
    deep += "c0";
    check(msgpack::validate(read(deep)).error().code() == errc::depth_exceeded, "nesting deeper than the limit");

    // Every document cut short at every length is refused, and walking it does nothing worse
    // than come up short - under the sanitizers, that is the test.
    for (const auto &whole : { from_hex(python_extended), from_hex(python_fd), from_hex(python_named),
                 read("84a165d6ff5f5e1000a162c4020102a16e9281a17891c093010203a17acb3ff8000000000000"),
                 read("9401d0dfcd0100cb3ff8000000000000") }) {
        check(msgpack::validate(whole).has_value(), "the whole document validates");
        for (std::size_t length = 0; length < whole.size(); ++length) {
            const std::span<const std::byte> cut { whole.data(), length };
            if (msgpack::validate(cut).has_value()) check(false, "a document cut short validates");
            std::ignore = msgpack::decode<can_frame>(cut);
            std::ignore = msgpack::decode<std::vector<int>>(cut);
            std::ignore = msgpack::reader::over(cut).as<value>();
        }
    }
}

// decode reads the whole buffer as the document: a count the bytes run out before, or bytes
// after it, is a broken document and refused, with try_decode saying what validate() would have.
void whole_documents() {
    for (const auto *hex : { "930102", "9301020304", "93010203c0" }) {
        const auto bytes = from_hex(hex);
        check(!msgpack::decode<std::vector<int>>(bytes).has_value(), std::string { "decode refuses " } + hex);
        const auto tried = msgpack::try_decode<std::vector<int>>(bytes);
        check(!tried && tried.error().code() == msgpack::validate(bytes).error().code()
                        && tried.error().offset() == msgpack::validate(bytes).error().offset(),
                std::string { "try_decode reports what validate() does: " } + hex);
    }
    check(msgpack::decode<std::vector<int>>(from_hex("93010203")) == std::vector<int> { 1, 2, 3 }, "a whole one still reads");
    check(!msgpack::decode<int>(from_hex("0101")).has_value(), "a scalar with a byte after it is refused");
}

} // namespace

int main() {
    scalars();
    containers();
    misuse();
    sinks();
    python_can();
    reading();
    malformed();
    whole_documents();
    return report("msgpack");
}
