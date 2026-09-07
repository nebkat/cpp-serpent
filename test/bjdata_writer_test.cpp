// The writer. Expected bytes are dart-bjdata's, either quoted from its test vectors or
// produced by its CLI; bjdata_fixture_test checks the whole corpus byte for byte.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/ostream_sink.hpp>

#include <array>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;

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

template<writer_options Options = writer_options {}, typename Body>
std::vector<std::byte> emit(Body body) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    basic_writer<Options> target { out };
    body(target);
    const auto result = target.finish();
    check(result.has_value(), "the writer finished cleanly");
    if (result) check_equal(*result, buffer.size(), "reported size matches the bytes produced");
    return buffer;
}

void produces(std::string_view expected, auto body, std::string_view what) {
    check_equal(std::string_view { hex(emit(body)) }, expected, what);
}

void scalars() {
    produces("5a", [](writer &w) { w.null(); }, "null");
    produces("54", [](writer &w) { w.value(true); }, "true");
    produces("46", [](writer &w) { w.value(false); }, "false");

    produces("5500", [](writer &w) { w.value(0); }, "U 0");
    produces("55ff", [](writer &w) { w.value(255); }, "U 255");
    produces("69ff", [](writer &w) { w.value(-1); }, "i -1");
    produces("750001", [](writer &w) { w.value(256); }, "u 256");
    produces("497fff", [](writer &w) { w.value(-129); }, "I -129");
    produces("6dffffffff", [](writer &w) { w.value(4294967295u); }, "m 4294967295");
    produces("4d0000000001000000", [](writer &w) { w.value(4294967296ull); }, "M 4294967296");
    produces("4c00000000ffffffff", [](writer &w) { w.value(-4294967296ll); }, "L -4294967296");

    // Unsigned is preferred at every width, so 200 is U and never i.
    produces("55c8", [](writer &w) { w.value(200); }, "200 prefers unsigned");

    // Floats narrow wherever the round trip is exact, so 1.0 is three bytes.
    produces("68003c", [](writer &w) { w.value(1.0); }, "h 1.0");
    produces("6800c0", [](writer &w) { w.value(-2.0); }, "h -2.0");
    produces("44182d4454fb210940", [](writer &w) { w.value(3.141592653589793); }, "D pi");
    produces("4e", [](writer &w) { w.noop(); }, "N");
    produces("4361", [](writer &w) { w.character('a'); }, "C a");

    produces("535500", [](writer &w) { w.value(""); }, "S empty");
    produces("53550568656c6c6f", [](writer &w) { w.value("hello"); }, "S hello");
    produces("53550668c3a96c6c6f", [](writer &w) { w.value("h\xc3\xa9llo"); }, "S utf-8 counts bytes");
    produces("4855022d31", [](writer &w) { w.high_precision("-1"); }, "H -1");

    // compact_types off pins every integer to int64 and every real to float64.
    check_equal(std::string_view { hex(emit<writer_options { .compact_types = false }>([](auto &w) { w.value(1); })) },
                "4c0100000000000000", "compact_types off widens an integer");
    check_equal(std::string_view { hex(emit<writer_options { .compact_types = false }>([](auto &w) { w.value(1.0); })) },
                "44000000000000f03f", "compact_types off widens a real");
}

void containers() {
    produces("5b5d", [](writer &w) { const auto scope = w.array(); }, "empty array");
    produces("7b7d", [](writer &w) { const auto scope = w.object(); }, "empty object");
    produces("5b5501550255035d", [](writer &w) { w.value(std::vector<int> { 1, 2, 3 }); }, "[1,2,3]");
    produces("5b5a54465501535501615d", [](writer &w) {
        const auto scope = w.array();
        w.null(); w.value(true); w.value(false); w.value(1); w.value("a");
    }, "heterogeneous array");

    produces("7b5503666f6f5501550362617255027d", [](writer &w) {
        const auto scope = w.object();
        scope.member("foo", 1);
        scope.member("bar", 2);
    }, "{foo:1,bar:2}");

    produces("5b5355016153550262625355036364655d",
             [](writer &w) { w.value(std::vector<std::string> { "a", "bb", "cde" }); }, "array of strings");


    // std::map writes as an object, in key order.
    produces("7b55016155015501625502550163550355016455047d", [](writer &w) {
        const auto scope = w.object();
        for (const auto &[key, value] : std::map<std::string, int> { { "a", 1 }, { "b", 2 }, { "c", 3 } }) {
            scope.member(key, value);
        }
        w.key("d");
        w.value(4);
    }, "object built from a map");

    produces("5b5b5501550255035d5b5504550555065d5d", [](writer &w) {
        w.value(std::vector<std::vector<int>> { { 1, 2, 3 }, { 4, 5, 6 } });
    }, "nested arrays");

    produces("5b5a5d", [](writer &w) {
        w.value(std::vector<std::optional<int>> { std::nullopt });
    }, "an empty optional writes null");
    produces("5b55075d", [](writer &w) {
        w.value(std::vector<std::optional<int>> { 7 });
    }, "an engaged optional writes its value");
}

void numeric_packing() {
    // A generic array stores each value at its own width; a typed one pays the widest
    // throughout. Both are measured and a tie keeps the generic form.
    produces("5b55015502550355045d", [](writer &w) { w.value(std::vector<int> { 1, 2, 3, 4 }); },
             "four small ints tie at 10 bytes and stay generic");

    check_equal(std::string_view { hex(emit([](writer &w) { w.value(std::vector<int> { 1, 2, 3, 4, 5 }); })) },
                "5b24552355050102030405", "five small ints pack");
    check_equal(std::string_view { hex(emit([](writer &w) { w.value(std::vector<int> { 1, 2, 3, 1000000 }); })) },
                "5b5501550255036d40420f005d", "one large value forces the width and keeps it generic");
    check_equal(std::string_view { hex(emit([](writer &w) {
                    w.value(std::vector<int> { 52445, 43707, 13124, 4386 });
                })) },
                "5b75ddcc75bbaa7544337522115d", "a 14 byte tie stays generic");
    check_equal(std::string_view { hex(emit([](writer &w) { w.value(std::vector<double> { 1.5, 2.5, -0.25 }); })) },
                "5b68003e6800416800b45d", "three halves stay generic");

    // Packing off falls back to the generic form whatever the measurement says.
    check_equal(std::string_view { hex(emit<writer_options { .numeric_packing = false }>(
                        [](auto &w) { w.value(std::vector<int> { 1, 2, 3, 4, 5 }); })) },
                "5b550155025503550455055d", "numeric_packing off keeps the generic form");
}

/**
 * A contiguous range packed at the element's own width goes out in one copy; narrowed or
 * written generically it costs a store per element. That is not a size question alone, and
 * the reference encoder - being Dart, where the copy is not available - only ever asks the
 * size one. copy_tolerance_percent is how much size you will pay for the copy.
 */
void contiguous_copy() {
    std::vector<double> real;
    for (int index = 0; index < 200; ++index) real.push_back(index * 0.1);
    std::vector<double> halves;
    for (int index = 0; index < 200; ++index) halves.push_back(index * 0.5);
    std::vector<std::int32_t> positive;
    for (int index = 0; index < 200; ++index) positive.push_back(index * 100000);

    constexpr writer_options slack { .copy_tolerance_percent = 5 };
    constexpr writer_options generous { .copy_tolerance_percent = 400 };

    const auto marker_of = [](std::span<const std::byte> bytes) {
        return bytes.size() > 2 && static_cast<char>(bytes[1]) == '$' ? static_cast<char>(bytes[2]) : '-';
    };

    // Doubles that do not narrow: generic is barely smaller, so a little slack buys the copy.
    check_equal(marker_of(encode(real)), '-', "real doubles are generic at zero tolerance");
    check_equal(marker_of(encode<slack>(real)), 'D', "and copied whole once a little slack is allowed");
    check(encode<slack>(real).size() > encode(real).size(), "which does cost a few bytes");

    // Doubles that all fit a float16: the copy would cost four times the space, so slack of
    // this size must not buy it.
    check_equal(marker_of(encode(halves)), 'h', "half-exact doubles narrow at zero tolerance");
    check_equal(marker_of(encode<slack>(halves)), 'h', "and small slack does not undo that");
    check_equal(marker_of(encode<generous>(halves)), 'D', "only generous slack takes the copy");

    // Already copyable at the chosen marker: positive int32 packs as uint32, whose bytes are
    // identical, so this is a copy at zero tolerance and the marker must not drift.
    check_equal(marker_of(encode(positive)), 'm', "positive int32 packs as uint32");
    check_equal(marker_of(encode<slack>(positive)), 'm', "and tolerance does not change that");
}

void typed_arrays() {
    const std::array<std::uint8_t, 4> bytes { 0xde, 0xad, 0xbe, 0xef };
    check_equal(std::string_view { hex(emit([&](writer &w) { w.typed_array(std::span<const std::uint8_t> { bytes }); })) },
                "5b2455235504deadbeef", "typed uint8 array");

    const std::array<std::uint16_t, 3> words { 1, 2, 3 };
    check_equal(std::string_view { hex(emit([&](writer &w) { w.typed_array(std::span<const std::uint16_t> { words }); })) },
                "5b2455235503010203", "typed uint16 array narrows to uint8");

    const std::array<std::uint16_t, 3> wide { 1, 2, 1000 };
    check_equal(std::string_view { hex(emit([&](writer &w) { w.typed_array(std::span<const std::uint16_t> { wide }); })) },
                "5b247523550301000200e803", "typed uint16 array keeps its width");

    const std::array<std::byte, 6> mac { std::byte { 0x3c }, std::byte { 0xe9 }, std::byte { 0x0e },
                                         std::byte { 0x12 }, std::byte { 0x34 }, std::byte { 0x56 } };
    check_equal(std::string_view { hex(emit([&](writer &w) { w.binary(mac); })) },
                "5b24422355063ce90e123456", "binary is [$B#");
    check_equal(std::string_view { hex(emit([&](writer &w) { w.value(mac); })) },
                "5b24422355063ce90e123456", "a range of bytes writes as binary");
}

void sinks() {
    const auto reference = emit([](writer &w) { w.value(std::vector<int> { 1, 2, 3 }); });

    // A fixed buffer must latch overflow at every length short of the whole document.
    for (std::size_t capacity = 0; capacity < reference.size(); ++capacity) {
        std::vector<std::byte> storage(capacity);
        span_sink out { storage };
        writer target { out };
        target.value(std::vector<int> { 1, 2, 3 });
        check(!target.finish().has_value(), "a short fixed buffer fails");
        check(out.overflowed(), "a short fixed buffer latches overflow");
        check(out.size() <= capacity, "a short fixed buffer never writes past its end");
    }
    std::vector<std::byte> exact(reference.size());
    span_sink fitted { exact };
    writer fitting { fitted };
    fitting.value(std::vector<int> { 1, 2, 3 });
    check(fitting.finish().has_value(), "an exactly sized buffer succeeds");
    check_equal(std::string_view { hex(fitted.written()) }, std::string_view { hex(reference) }, "span_sink bytes");

    counting_sink counter;
    writer counting { counter };
    counting.value(std::vector<int> { 1, 2, 3 });
    check_equal(counter.size(), reference.size(), "counting_sink agrees with the real output");
    check_equal(measure(std::vector<int> { 1, 2, 3 }), reference.size(), "measure() agrees");

    std::vector<std::byte> appended;
    iterator_sink iterated { std::back_inserter(appended) };
    writer iterating { iterated };
    iterating.value(std::vector<int> { 1, 2, 3 });
    check_equal(std::string_view { hex(appended) }, std::string_view { hex(reference) }, "iterator_sink bytes");

    std::ostringstream stream;
    ostream_sink streamed { stream };
    writer streaming { streamed };
    streaming.value(std::vector<int> { 1, 2, 3 });
    check_equal(stream.str().size(), reference.size(), "ostream_sink bytes");

    // A bare lambda needs no sink type at all.
    std::size_t seen = 0;
    auto collect = [&](std::span<const std::byte> bytes) { seen += bytes.size(); };
    writer callback { collect };
    callback.value(std::vector<int> { 1, 2, 3 });
    check_equal(seen, reference.size(), "a callable is a sink");
}

void error_latching() {
    std::vector<std::byte> buffer;
    container_sink out { buffer };

    {
        writer target { out };
        target.key("orphan");
        check_equal(target.error_code(), errc::key_outside_object, "a key outside an object fails");
    }
    {
        // A container cannot be left open by accident: the scope is the only way to open one
        // and it closes itself. It can still be held open deliberately, and finish() catches
        // that.
        writer target { out };
        auto held = std::optional { target.array() };
        check_equal(target.finish().error().code(), errc::unterminated_container,
                    "a deliberately held-open container fails at finish");
        held.reset();
    }
    {
        // A moved-from scope closes nothing, so the container closes exactly once.
        std::vector<std::byte> moved;
        container_sink moved_out { moved };
        writer target { moved_out };
        {
            auto first = target.array();
            auto second = std::move(first);
            second.value(1);
        }
        check(target.finish().has_value(), "a moved scope still closes exactly once");
        check_equal(std::string_view { hex(moved) }, "5b55015d", "and produces one well formed array");
    }
    {
        // Once failed, everything after is a no-op rather than a cascade of errors.
        std::array<std::byte, 1> tiny {};
        span_sink small { tiny };
        writer target { small };
        target.value("a long string that will not fit");
        target.value(1);
        target.value(2);
        check_equal(target.error_code(), errc::sink_failed, "the first failure is the one reported");
    }
    {
        writer target { out };
        const auto nest = [&](auto &self, int remaining) -> void {
            if (remaining == 0) return;
            const auto scope = target.array();
            self(self, remaining - 1);
        };
        nest(nest, max_depth + 2);
        check_equal(target.error_code(), errc::depth_exceeded, "over deep nesting fails");
    }
}

}// namespace

int main() {
    scalars();
    containers();
    numeric_packing();
    contiguous_copy();
    typed_arrays();
    sinks();
    error_latching();
    return report("bjdata_writer");
}
