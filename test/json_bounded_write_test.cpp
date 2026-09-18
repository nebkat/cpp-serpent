// A described type's booleans and numbers are written in runs, into room claimed once for the
// longest the run could be. Two things can go wrong with that and neither shows in a round trip:
// the text could differ from what writing member by member produces, and a claim for the longest
// case could fail a fixed buffer that the actual text would have fitted.

#include "check.hpp"

#include <serpent/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace json = serpent::json;
using serpent::span_sink;

namespace {

struct [[= serpent::serializable {}]] all_bounded {
    bool flag = true;
    std::int8_t tiny = -128;
    std::uint16_t small = 65535;
    std::int64_t least = std::numeric_limits<std::int64_t>::min();
    std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    float single = 0.1f;
    double whole = 3;
    double smallest = std::numeric_limits<double>::denorm_min();
    double largest = -std::numeric_limits<double>::max();
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
    std::string text = "between \"runs\"";
    bool after = false;
    [[= serpent::skip {}]] int never = 99;
    int still_the_same_run = 7;
    std::optional<int> maybe;
    colour shade = colour::green;
    inner nested;
    std::vector<int> list { 1, 2 };
    [[= serpent::key("needs \"escaping\"")]] int awkward = 5;
    [[= serpent::tagged("kind")]] std::variant<int, inner> choice = inner {};
    int last = -1;
};

struct [[= serpent::serializable {}]] with_text {
    int before = 1;
    std::string plain = "nothing to escape here";
    std::string_view borrowed = "nor here";
    std::string empty;
    double after = 2.5;
    std::string awkward = "a \"quote\" and a\ttab";
    bool last = true;
};

struct [[= serpent::serializable {}]] leading_text {
    std::string name = "first";
    int value = 1;
};

struct [[= serpent::serializable {}]] only_skipped {
    [[= serpent::skip {}]] int never = 0;
};

/** The same members, written one at a time through the visitor every format shares. */
template<typename T>
std::string member_by_member(const T &value, json::writer_options options = {}) {
    std::string text;
    serpent::container_sink sink { text };
    json::writer out { sink, options };
    {
        const auto scope = out.object();
        serpent::write_members(out, value);
    }
    check(out.finish().has_value(), "the member by member form is written");
    return text;
}

template<typename T>
void same_either_way(const T &value, std::string_view what) {
    check_equal(std::string_view { json::encode(value) }, std::string_view { member_by_member(value) }, what);
    check_equal(std::string_view { json::encode(value, { .indent = 2 }) },
            std::string_view { member_by_member(value, { .indent = 2 }) }, what);
    check_equal(std::string_view { json::encode(std::vector<T> { value, value }) },
            std::string_view { "[" + member_by_member(value) + "," + member_by_member(value) + "]" }, what);
}

/** Every capacity short of the document fails without losing what fitted; the exact one succeeds. */
template<typename T>
void fits_exactly(const T &value, std::size_t longest_token) {
    const auto whole = json::encode(value);

    for (std::size_t capacity = 0; capacity <= whole.size(); ++capacity) {
        std::vector<std::byte> storage(capacity);
        span_sink out { storage };
        json::writer target { out };
        target.value(value);
        const bool finished = target.finish().has_value();
        const std::string_view written { reinterpret_cast<const char *>(out.written().data()), out.written().size() };

        if (capacity == whole.size()) {
            check(finished && !out.overflowed(), "a buffer of exactly the document's size succeeds");
            check_equal(written, std::string_view { whole }, "and holds the document");
            continue;
        }
        check(!finished && out.overflowed(), "a short fixed buffer fails");
        check(written == std::string_view { whole }.substr(0, written.size()), "holding the start of the document");
        check(capacity - written.size() <= longest_token, "and everything that fitted");
    }
}

} // namespace

int main() {
    check_equal(std::string_view { json::encode(all_bounded {}) },
            std::string_view { R"({"flag":true,"tiny":-128,"small":65535,"least":-9223372036854775808,)"
                               R"("most":18446744073709551615,"single":0.10000000149011612,"whole":3.0,)"
                               R"("smallest":5e-324,"largest":-1.7976931348623157e+308,"missing":null,)"
                               R"("letter":97})" },
            "every kind of bounded member, at its extremes");

    same_either_way(all_bounded {}, "one run is the same text as its members one at a time");
    same_either_way(interrupted {}, "and so are runs with other members between them");
    same_either_way(leading_text {}, "a run that is not first in its object begins with a comma");
    same_either_way(with_text {}, "strings join a run, and one with an escape in it leaves it");
    same_either_way(only_skipped {}, "an object with nothing to write is still an object");
    same_either_way(inner {}, "a nested type");

    interrupted set;
    set.maybe = 4;
    set.choice = 9;
    same_either_way(set, "an optional that is present and a variant holding a scalar");

    // The claim is for the longest the run could be, which is far more than these take: a bool,
    // five small numbers. A buffer of the size they do take has to be enough.
    fits_exactly(all_bounded {}, 32);
    fits_exactly(interrupted {}, 32);
    fits_exactly(with_text {}, 32);
    fits_exactly(std::vector<inner>(40), 8);

    return report("json_bounded_write");
}
