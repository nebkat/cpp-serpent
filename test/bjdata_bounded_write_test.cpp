// A described type's booleans and numbers are written in runs, keys and all, into room claimed
// once for the longest the run could be. Two things can go wrong with that and neither shows in
// a round trip: the bytes could differ from what writing member by member produces, and a claim
// for the longest case could fail a fixed buffer that the actual bytes would have fitted.

#include "check.hpp"

#include <serpent/bjdata.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bjdata = serpent::bjdata;
using serpent::span_sink;

namespace {

struct [[= serpent::serializable {}]] all_bounded {
    bool flag = true;
    std::int8_t tiny = -128;
    std::uint16_t small = 65535;
    std::int32_t negative = -70000;
    std::int64_t least = std::numeric_limits<std::int64_t>::min();
    std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    float single = 0.1f;
    double half = 0.5;
    double whole = 3;
    double precise = 0.1;
    double missing = std::numeric_limits<double>::quiet_NaN();
    char letter = 'a';
};

struct [[= serpent::serializable {}]] inner {
    int a = 1;
    int b = 2;
};

enum class colour { red, green };

struct [[= serpent::serializable {}]] interrupted {
    int before = 1;
    double also_before = 2.5;
    std::string text = "between runs";
    bool after = false;
    [[= serpent::skip {}]] int never = 99;
    int after_the_skipped = 7;
    std::optional<int> maybe;
    colour shade = colour::green;
    inner nested;
    std::vector<int> list { 1, 2 };
    [[= serpent::tagged("kind")]] std::variant<int, inner> choice = inner {};
    int last = -1;
};

template<bjdata::writer_options Options, typename T>
std::vector<std::byte> as_a_value(const T &value) {
    return bjdata::encode<Options>(value);
}

/** The same members, written one at a time through the visitor every format shares. */
template<bjdata::writer_options Options, typename T>
std::vector<std::byte> member_by_member(const T &value) {
    std::vector<std::byte> out;
    serpent::container_sink sink { out };
    bjdata::basic_writer<Options> target { sink };
    {
        const auto scope = target.object();
        serpent::write_members(target, value);
    }
    check(target.finish().has_value(), "the member by member form is written");
    return out;
}

template<typename T>
void same_either_way(const T &value, std::string_view what) {
    check(as_a_value<bjdata::writer_options {}>(value) == member_by_member<bjdata::writer_options {}>(value), what);
    check(as_a_value<bjdata::no_compaction>(value) == member_by_member<bjdata::no_compaction>(value), what);
    check(as_a_value<bjdata::reference_parity>(value) == member_by_member<bjdata::reference_parity>(value), what);
}

/** Every capacity short of the document fails without losing what fitted; the exact one succeeds. */
template<typename T>
void fits_exactly(const T &value, std::size_t longest_piece) {
    const auto whole = bjdata::encode(value);

    for (std::size_t capacity = 0; capacity <= whole.size(); ++capacity) {
        std::vector<std::byte> storage(capacity);
        span_sink out { storage };
        bjdata::writer target { out };
        target.value(value);
        const bool finished = target.finish().has_value();

        if (capacity == whole.size()) {
            check(finished && !out.overflowed(), "a buffer of exactly the document's size succeeds");
            check(std::ranges::equal(out.written(), whole), "and holds the document");
            continue;
        }
        check(!finished && out.overflowed(), "a short fixed buffer fails");
        check(std::ranges::equal(out.written(), std::span { whole }.first(out.written().size())),
                "holding the start of the document");
        check(capacity - out.written().size() <= longest_piece, "and everything that fitted");
    }
}

} // namespace

int main() {
    same_either_way(all_bounded {}, "one run is the same bytes as its members one at a time");
    same_either_way(interrupted {}, "and so are runs with other members between them");
    same_either_way(inner {}, "a nested type");

    interrupted set;
    set.maybe = 4;
    set.choice = 9;
    same_either_way(set, "an optional that is present and a variant holding a scalar");

    // The claim is for a marker and eight bytes a member, and most of these take two or three.
    fits_exactly(all_bounded {}, 24);
    fits_exactly(interrupted {}, 24);
    fits_exactly(std::vector<inner>(40), 8);

    return report("bjdata_bounded_write");
}
