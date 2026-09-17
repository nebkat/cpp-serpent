// The writer: the bytes each thing it can be asked for comes out as, preferring size and preferring speed.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/ostream_sink.hpp>

#include <array>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace serpent;
using namespace serpent::bjdata;

// The bodies below take `auto &` so they bind to whichever writer emit() hands them.

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

template<prefer Preference = prefer::size, typename Body>
std::vector<std::byte> emit(Body body) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    basic_writer<Preference> target { out };
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
    produces("5a", [](auto &w) { w.null(); }, "null");
    produces("54", [](auto &w) { w.value(true); }, "true");
    produces("46", [](auto &w) { w.value(false); }, "false");

    produces("5500", [](auto &w) { w.value(0); }, "U 0");
    produces("55ff", [](auto &w) { w.value(255); }, "U 255");
    produces("69ff", [](auto &w) { w.value(-1); }, "i -1");
    produces("750001", [](auto &w) { w.value(256); }, "u 256");
    produces("497fff", [](auto &w) { w.value(-129); }, "I -129");
    produces("6dffffffff", [](auto &w) { w.value(4294967295u); }, "m 4294967295");
    produces("4d0000000001000000", [](auto &w) { w.value(4294967296ull); }, "M 4294967296");
    produces("4c00000000ffffffff", [](auto &w) { w.value(-4294967296ll); }, "L -4294967296");

    // Unsigned is preferred at every width, so 200 is U and never i.
    produces("55c8", [](auto &w) { w.value(200); }, "200 prefers unsigned");

    // Floats narrow wherever the round trip is exact, so 1.0 is three bytes.
    produces("68003c", [](auto &w) { w.value(1.0); }, "h 1.0");
    produces("6800c0", [](auto &w) { w.value(-2.0); }, "h -2.0");
    produces("44182d4454fb210940", [](auto &w) { w.value(3.141592653589793); }, "D pi");
    produces("4e", [](auto &w) { w.noop(); }, "N");
    produces("4361", [](auto &w) { w.character('a'); }, "C a");

    produces("535500", [](auto &w) { w.value(""); }, "S empty");
    produces("53550568656c6c6f", [](auto &w) { w.value("hello"); }, "S hello");
    produces("53550668c3a96c6c6f", [](auto &w) { w.value("h\xc3\xa9llo"); }, "S utf-8 counts bytes");
    produces("4855022d31", [](auto &w) { w.high_precision("-1"); }, "H -1");

    // Preferring speed, a number goes under the marker of the type it has and is not looked at.
    const auto fast = [](auto body) { return hex(emit<prefer::speed>(body)); };
    check_equal(fast([](auto &w) { w.value(1); }), "6c01000000", "an int is l, four bytes");
    check_equal(fast([](auto &w) { w.value(std::uint8_t { 7 }); }), "5507", "a uint8 is U");
    check_equal(fast([](auto &w) { w.value(std::int16_t { -2 }); }), "49feff", "an int16 is I");
    check_equal(fast([](auto &w) { w.value(std::uint64_t { 1 }); }), "4d0100000000000000", "a uint64 is M");
    check_equal(fast([](auto &w) { w.value(1.0); }), "44000000000000f03f", "a double is D");
    check_equal(fast([](auto &w) { w.value(1.0f); }), "640000803f", "a float is d");
    check_equal(fast([](auto &w) { w.value(true); }), "54", "a boolean is its marker either way");
    check_equal(fast([](auto &w) { w.value("hello"); }), "53550568656c6c6f", "and a length is always the narrowest");
}

void containers() {
    produces("5b5d", [](auto &w) { const auto scope = w.array(); }, "empty array");
    produces("7b7d", [](auto &w) { const auto scope = w.object(); }, "empty object");
    produces("5b246c235503010000000200000003000000", [](auto &w) { w.value(std::vector<int> { 1, 2, 3 }); },
            "[1,2,3] is a typed array of what it is");
    produces(
            "5b5a54465501535501615d",
            [](auto &w) {
                const auto scope = w.array();
                w.null();
                w.value(true);
                w.value(false);
                w.value(1);
                w.value("a");
            },
            "heterogeneous array");

    produces(
            "7b5503666f6f5501550362617255027d",
            [](auto &w) {
                const auto scope = w.object();
                scope.member("foo", 1);
                scope.member("bar", 2);
            },
            "{foo:1,bar:2}");

    produces(
            "5b5355016153550262625355036364655d",
            [](auto &w) { w.value(std::vector<std::string> { "a", "bb", "cde" }); }, "array of strings");

    // std::map writes as an object, in key order.
    produces(
            "7b55016155015501625502550163550355016455047d",
            [](auto &w) {
                const auto scope = w.object();
                for (const auto &[key, value] : std::map<std::string, int> { { "a", 1 }, { "b", 2 }, { "c", 3 } }) {
                    scope.member(key, value);
                }
                w.key("d");
                w.value(4);
            },
            "object built from a map");

    produces(
            "5b5b24552355030102035b24552355030405065d",
            [](auto &w) { w.value(std::vector<std::vector<std::uint8_t>> { { 1, 2, 3 }, { 4, 5, 6 } }); },
            "nested arrays");

    // Preferring speed, a range that is not of numbers says how many elements it has.
    check_equal(std::string_view { hex(emit<prefer::speed>(
                        [](auto &w) { w.value(std::vector<std::string> { "a", "bb", "cde" }); })) },
            "5b235503535501615355026262535503636465", "a counted array of strings");

    produces(
            "5b5a5d", [](auto &w) { w.value(std::vector<std::optional<int>> { std::nullopt }); },
            "an empty optional writes null");
    produces(
            "5b55075d", [](auto &w) { w.value(std::vector<std::optional<int>> { 7 }); },
            "an engaged optional writes its value");
}

/**
 * A range of numbers is a typed array at the width the numbers already have, whichever is
 * preferred: narrowing is for a value on its own. So it is one copy to write and one to read,
 * and never turns into an array of separately marked elements because of what is in it.
 */
void ranges_of_numbers() {
    for (const auto &values : { std::vector<std::int32_t> { 1, 2, 3 }, std::vector<std::int32_t> { 1, 2, 3, 1000000 },
                 std::vector<std::int32_t>(300, 7), std::vector<std::int32_t> {} }) {
        const auto small = encode<prefer::size>(values);
        check(encode<prefer::speed>(values) == small, "the same bytes whichever is preferred");
        check_equal(std::string_view { hex(small).substr(0, 8) }, "5b246c23", "[$l# whatever the values are");
        check(decode<std::vector<std::int32_t>>(small) == values, "and reads back");
        check(view::over(small).as_span<std::int32_t>().has_value(), "or is viewed in place");
    }

    check_equal(std::string_view { hex(encode(std::vector<double> { 1.5, 2.5 })) },
            "5b2444235502000000000000f83f0000000000000440", "doubles stay doubles");
    check_equal(std::string_view { hex(encode(std::vector<float> { 1.5f })) }, "5b24642355010000c03f", "and floats floats");

    // Read into numbers of the same type it is one copy; into anything else, an element at a time.
    const auto words = encode(std::vector<std::uint16_t> { 1, 2, 300 });
    check(decode<std::vector<std::uint16_t>>(words) == std::vector<std::uint16_t> { 1, 2, 300 }, "the same type");
    check(decode<std::vector<int>>(words) == std::vector<int> { 1, 2, 300 }, "a wider one");
    check(decode<std::vector<double>>(words) == std::vector<double> { 1, 2, 300 }, "reals");
    check(decode<std::list<std::uint16_t>>(words) == std::list<std::uint16_t> { 1, 2, 300 }, "a list");
    check(!decode<std::vector<std::uint8_t>>(words), "and not a narrower one that cannot hold them");
    const auto cut = std::span { words }.first(words.size() - 1);
    check(!decode<std::vector<std::uint16_t>>(cut), "nor one whose payload is cut short");

    // A range that is not contiguous is the same bytes, stored an element at a time.
    check(encode(std::list<std::int16_t> { 1, -2, 300 }) == encode(std::vector<std::int16_t> { 1, -2, 300 }),
            "a list is written as a vector is");

    // An array that is mixed anyway has no type to be of, and each value in it narrows on its own.
    check_equal(std::string_view { hex(encode(std::tuple { 1, 1000000, 1.0 })) }, "5b55016d40420f0068003c5d",
            "a tuple's numbers narrow where size is preferred");
    check_equal(std::string_view { hex(encode<prefer::speed>(std::tuple { 1, 1.0f })) }, "5b6c01000000640000803f5d",
            "and keep their types where speed is");
}

void typed_arrays() {
    const std::array<std::uint8_t, 4> bytes { 0xde, 0xad, 0xbe, 0xef };
    check_equal(
            std::string_view { hex(emit([&](auto &w) { w.typed_array(std::span<const std::uint8_t> { bytes }); })) },
            "5b2455235504deadbeef", "typed uint8 array");

    const std::array<std::uint16_t, 3> words { 1, 2, 3 };
    check_equal(
            std::string_view { hex(emit([&](auto &w) { w.typed_array(std::span<const std::uint16_t> { words }); })) },
            "5b2475235503010002000300", "a typed uint16 array stays uint16, however small its values");

    const std::array<std::uint16_t, 3> wide { 1, 2, 1000 };
    check_equal(
            std::string_view { hex(emit([&](auto &w) { w.typed_array(std::span<const std::uint16_t> { wide }); })) },
            "5b247523550301000200e803", "typed uint16 array keeps its width");

    const std::array<std::byte, 6> mac { std::byte { 0x3c }, std::byte { 0xe9 }, std::byte { 0x0e }, std::byte { 0x12 },
        std::byte { 0x34 }, std::byte { 0x56 } };
    check_equal(std::string_view { hex(emit([&](auto &w) { w.binary(mac); })) }, "5b24422355063ce90e123456",
            "binary is [$B#");
    check_equal(std::string_view { hex(emit([&](auto &w) { w.value(mac); })) }, "5b24422355063ce90e123456",
            "a range of bytes writes as binary");
}

void sinks() {
    const auto reference = emit([](auto &w) { w.value(std::vector<int> { 1, 2, 3 }); });

    // Both sinks over contiguous storage are written into directly. Asserted rather than left
    // to the bytes agreeing, because falling back to the copying path agrees on those too.
    static_assert(lending_sink<span_sink>, "a fixed buffer is written into in place");
    static_assert(lending_sink<container_sink<std::vector<std::byte>>>, "so is a container");

    // A container holding room for its document must not grow: what it was given is enough,
    // whatever size the writer would have preferred to ask for.
    {
        std::vector<std::byte> sized;
        sized.reserve(reference.size());
        const auto *const storage = sized.data();
        const auto capacity = sized.capacity();
        container_sink into { sized };
        basic_writer<prefer::size> filling { into };
        filling.value(std::vector<int> { 1, 2, 3 });
        check(filling.finish().has_value(), "an exactly reserved container accepts the document");
        check_equal(std::string_view { hex(sized) }, std::string_view { hex(reference) },
                "an exactly reserved container holds the same bytes");
        check(sized.capacity() == capacity && sized.data() == storage,
                "and never reallocated to get them");
    }

    // A fixed buffer must latch overflow at every length short of the whole document - and must
    // still hold what it accepted before that. Running out of room is one failure, not two: the
    // room already filled stays filled. Swept over a short document and one long enough to be
    // written in more than one piece, since what is at risk is the piece in hand when the next
    // cannot be had.
    const std::vector<std::string> words { "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta" };
    const auto sweep = [](const auto &document, std::size_t longest_token) {
        const auto whole = emit([&](auto &w) { w.value(document); });
        for (std::size_t capacity = 0; capacity < whole.size(); ++capacity) {
            std::vector<std::byte> storage(capacity);
            span_sink out { storage };
            basic_writer<prefer::size> target { out };
            target.value(document);
            check(!target.finish().has_value(), "a short fixed buffer fails");
            check(out.overflowed(), "a short fixed buffer latches overflow");
            check(out.size() <= capacity, "a short fixed buffer never writes past its end");
            check(std::ranges::equal(out.written(), std::span { whole }.first(out.size())),
                    "what it holds is the start of the document");
            // Everything that fitted was kept: it stopped within one token of the end of the
            // buffer, rather than somewhere back where an earlier piece began.
            check(capacity - out.size() <= longest_token, "and it kept everything that fitted");
        }
    };
    sweep(std::vector<int> { 1, 2, 3 }, 6);
    sweep(words, 8);

    std::vector<std::byte> exact(reference.size());
    span_sink fitted { exact };
    basic_writer<prefer::size> fitting { fitted };
    fitting.value(std::vector<int> { 1, 2, 3 });
    check(fitting.finish().has_value(), "an exactly sized buffer succeeds");
    check_equal(std::string_view { hex(fitted.written()) }, std::string_view { hex(reference) }, "span_sink bytes");

    // The writer batches, so a sink only holds everything once finish() has handed it over.
    counting_sink counter;
    {
        basic_writer<prefer::size> counting { counter };
        counting.value(std::vector<int> { 1, 2, 3 });
        check(counting.finish().has_value(), "counting_sink accepts the document");
    }
    check_equal(counter.size(), reference.size(), "counting_sink agrees with the real output");
    check_equal(measure<prefer::size>(std::vector<int> { 1, 2, 3 }), reference.size(), "measure() agrees");

    std::vector<std::byte> appended;
    iterator_sink iterated { std::back_inserter(appended) };
    {
        basic_writer<prefer::size> iterating { iterated };
        iterating.value(std::vector<int> { 1, 2, 3 });
        check(iterating.finish().has_value(), "iterator_sink accepts the document");
    }
    check_equal(std::string_view { hex(appended) }, std::string_view { hex(reference) }, "iterator_sink bytes");

    std::ostringstream stream;
    ostream_sink streamed { stream };
    {
        basic_writer<prefer::size> streaming { streamed };
        streaming.value(std::vector<int> { 1, 2, 3 });
        check(streaming.finish().has_value(), "ostream_sink accepts the document");
    }
    check_equal(stream.str().size(), reference.size(), "ostream_sink bytes");

    // A bare lambda needs no sink type at all.
    std::size_t seen = 0;
    auto collect = [&](std::span<const std::byte> bytes) { seen += bytes.size(); };
    {
        basic_writer<prefer::size> callback { collect };
        callback.value(std::vector<int> { 1, 2, 3 });
        check(callback.finish().has_value(), "a callable accepts the document");
    }
    check_equal(seen, reference.size(), "a callable is a sink");
}

void error_latching() {
    std::vector<std::byte> buffer;
    container_sink out { buffer };

    {
        basic_writer<prefer::size> target { out };
        target.key("orphan");
        check_equal(target.error_code(), errc::key_outside_object, "a key outside an object fails");
    }
    {
        // A container cannot be left open by accident: the scope is the only way to open one
        // and it closes itself. It can still be held open deliberately, and finish() catches
        // that.
        basic_writer<prefer::size> target { out };
        auto held = std::optional { target.array() };
        check_equal(target.finish().error().code(), errc::unterminated_container,
                "a deliberately held-open container fails at finish");
        held.reset();
    }
    {
        // A moved-from scope closes nothing, so the container closes exactly once.
        std::vector<std::byte> moved;
        container_sink moved_out { moved };
        basic_writer<prefer::size> target { moved_out };
        {
            auto first = target.array();
            auto second = std::move(first);
            second.value(1);
        }
        check(target.finish().has_value(), "a moved scope still closes exactly once");
        check_equal(std::string_view { hex(moved) }, "5b55015d", "and produces one well formed array");
    }
    {
        // Once failed, everything after is a no-op rather than a cascade of errors. The string
        // is longer than the writer's batch, so it goes to the sink directly and fails there
        // rather than waiting for finish().
        std::array<std::byte, 1> tiny {};
        span_sink small { tiny };
        basic_writer<prefer::size> target { small };
        target.value(std::string(1024, 'x'));
        target.value(1);
        target.value(2);
        check_equal(target.error_code(), errc::sink_failed, "the first failure is the one reported");
    }
    {
        basic_writer<prefer::size> target { out };
        const auto nest = [&](auto &self, int remaining) -> void {
            if (remaining == 0) return;
            const auto scope = target.array();
            self(self, remaining - 1);
        };
        nest(nest, max_depth + 2);
        check_equal(target.error_code(), errc::depth_exceeded, "over deep nesting fails");
    }
}

} // namespace

int main() {
    scalars();
    containers();
    ranges_of_numbers();
    typed_arrays();
    sinks();
    error_latching();
    return report("bjdata_writer");
}
