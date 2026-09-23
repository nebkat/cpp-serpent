// A field that says how its value is written: serpent::enum_as_name, serpent::enum_as_number and
// serpent::with.
//
// Each is checked through every path a member can take - the generated JSON and BJData readers
// and writers, BJData written for size and for speed, and the generic walk an indexed document
// is read by - because each of those decides for itself how to handle a member, and one that
// forgot about these would write the field as its type rather than as the annotation says.

#include "check.hpp"

#include <serpent/bjdata.hpp>
#include <serpent/json.hpp>
#include <serpent/json/indexed.hpp>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

static_assert(serpent::reflection_available, "this test is only built where reflection works");

namespace json = serpent::json;
namespace bjdata = serpent::bjdata;

// Nothing about serialization on either of these: they are what a field makes of them.
enum class probe_state { idle, busyNow, lost };
enum class reachability : std::uint8_t { unknown, reachable, unreachable };

// Says for itself that it is named. A field may still want it as a number.
enum class[[= serpent::serializable {}]] link_kind { wired, wireless[[= serpent::as("wifi")]] };

// A naming rule of its own, which a field without one inherits.
enum class[[= serpent::naming { serpent::naming_style::kebab_case }]] cut_mode { autoHeight, manualHeight };

struct[[= serpent::serializable {}]] states {
    [[= serpent::enum_as_name {}]] probe_state as_written = probe_state::busyNow;
    [[= serpent::enum_as_name { serpent::naming_style::snake_case }]] probe_state snake = probe_state::busyNow;
    [[= serpent::enum_as_name {}]] cut_mode inherited = cut_mode::manualHeight;
    [[= serpent::enum_as_number {}]] link_kind numbered = link_kind::wireless;
    link_kind named = link_kind::wireless;
    probe_state plain = probe_state::lost;

    bool operator==(const states &) const = default;
};

struct[[= serpent::serializable {}]] overridden {
    [[= serpent::enum_as_name {
        serpent::enum_entry { probe_state::idle, "waiting" },
        serpent::enum_entry { probe_state::lost, "gone", serpent::fallback {} },
    }]] probe_state renamed = probe_state::idle;
    [[= serpent::enum_as_number { serpent::enum_entry { reachability::unknown, -1 } }]] reachability reach =
            reachability::unknown;
    [[= serpent::enum_as_name { serpent::enum_entry { probe_state::lost, serpent::skip {} } }]] probe_state
            without_lost = probe_state::idle;

    bool operator==(const overridden &) const = default;
};

struct[[= serpent::serializable {}]] containers {
    [[= serpent::enum_as_name {}]] std::optional<probe_state> maybe = probe_state::lost;
    [[= serpent::enum_as_name {}]] std::optional<probe_state> nothing {};
    [[= serpent::enum_as_name {}]] std::vector<probe_state> history { probe_state::idle, probe_state::lost };
    [[= serpent::enum_as_name {}]] std::array<probe_state, 2> pair_of { probe_state::busyNow, probe_state::idle };
    [[= serpent::enum_as_name {}]] std::set<probe_state> seen { probe_state::idle, probe_state::lost };
    [[= serpent::enum_as_name {}]] std::map<std::string, probe_state> by_name { { "a", probe_state::lost } };
    [[= serpent::enum_as_name {}]] std::map<probe_state, int> counts { { probe_state::busyNow, 3 } };
    [[= serpent::enum_as_name {}]] std::vector<std::optional<probe_state>> gaps { probe_state::idle, std::nullopt };

    bool operator==(const containers &) const = default;
};

/** A codec: the static write and read a serializer<T> has. */
struct seconds_since_boot {
    template<typename Writer>
    static void write(Writer &out, const std::chrono::milliseconds &value) {
        out.value(value.count() / 1000);
    }

    template<typename Source>
    static bool read(const Source &source, std::chrono::milliseconds &value) {
        const auto seconds = source.template as<std::int64_t>();
        if (!seconds) return false;
        value = std::chrono::seconds { *seconds };
        return true;
    }
};

/** One the fast paths would take for themselves: a fixed-width number, a boolean, a short string. */
struct doubled {
    template<typename Writer>
    static void write(Writer &out, const int &value) {
        out.value(value * 2);
    }

    template<typename Source>
    static bool read(const Source &source, int &value) {
        const auto held = source.template as<int>();
        if (!held) return false;
        value = *held / 2;
        return true;
    }
};

struct yes_no {
    template<typename Writer>
    static void write(Writer &out, const bool &value) {
        out.value(value ? "yes" : "no");
    }

    template<typename Source>
    static bool read(const Source &source, bool &value) {
        const auto text = source.template as<std::string>();
        if (!text) return false;
        value = *text == "yes";
        return true;
    }
};

struct shouted {
    template<typename Writer>
    static void write(Writer &out, const std::string &value) {
        out.value(value + "!");
    }

    template<typename Source>
    static bool read(const Source &source, std::string &value) {
        auto text = source.template as<std::string>();
        if (!text || text->empty() || text->back() != '!') return false;
        text->pop_back();
        value = std::move(*text);
        return true;
    }
};

struct timestamp {
    std::int64_t unix_seconds = 0;
    bool operator==(const timestamp &) const = default;
};

template<>
struct serpent::serializer<timestamp, void> {
    template<typename Writer>
    static void write(Writer &out, const timestamp &value) {
        out.value(std::to_string(value.unix_seconds));
    }

    template<typename Source>
    static bool read(const Source &source, timestamp &value) {
        const auto text = source.template as<std::string>();
        if (!text) return false;
        value.unix_seconds = std::stoll(*text);
        return true;
    }
};

struct[[= serpent::serializable {}]] coded {
    [[= serpent::with<seconds_since_boot> {}]] std::chrono::milliseconds uptime { 42'000 };
    [[= serpent::with<doubled> {}]] int count = 21;
    [[= serpent::with<yes_no> {}]] bool enabled = true;
    [[= serpent::with<shouted> {}]] std::string greeting = "hi";
    [[= serpent::with<serpent::serializer<timestamp>> {}]] timestamp started { 1'700'000'000 };
    [[= serpent::with {
        [](auto &out, const int &value) { out.value(value + 100); },
        [](const auto &source, int &value) {
            const auto held = source.template as<int>();
            if (held) value = *held - 100;
            return held.has_value();
        },
    }]] int offset = 5;

    bool operator==(const coded &) const = default;
};

struct[[= serpent::serializable {}]] partly_there {
    [[= serpent::with<doubled> {}]] int needed = 1;
    [[ = serpent::with<doubled> {}, = serpent::defaulted {} ]] int optional_one = 7;
};

/** Written and read back through every path a member takes; each must give back what went in. */
template<typename T>
void round_trips_everywhere(const T &original, std::string_view what) {
    const std::string text = json::encode(original);
    const auto from_json = json::decode<T>(text);
    check(from_json && *from_json == original, std::string { what } + ": JSON");

    const auto index = json::structural_index::over(text);
    const auto generic = index.root().template as<T>();
    check(generic && *generic == original, std::string { what } + ": JSON through the generic walk");

    const auto compact = bjdata::decode<T>(bjdata::encode<bjdata::prefer::size>(original));
    check(compact && *compact == original, std::string { what } + ": BJData written for size");

    const auto fast = bjdata::decode<T>(bjdata::encode<bjdata::prefer::speed>(original));
    check(fast && *fast == original, std::string { what } + ": BJData written for speed");

    const auto bytes = bjdata::encode(original);
    const auto through_reader = bjdata::reader::over(bytes).template as<T>();
    check(through_reader && *through_reader == original, std::string { what } + ": BJData through a reader");

    const std::vector<T> many(3, original);
    const auto table = bjdata::decode<std::vector<T>>(bjdata::encode(many));
    check(table && *table == many, std::string { what } + ": BJData, a sequence of them");
}

void a_field_names_an_enumeration_that_does_not() {
    check_equal(json::encode(states {}),
            R"({"as_written":"busyNow","snake":"busy_now","inherited":"manual-height","numbered":1,"named":"wifi","plain":2})",
            "each field chooses; the enumeration's own naming rule is the default, and an unannotated one is a number");
    round_trips_everywhere(states {}, "states");
    round_trips_everywhere(states { probe_state::idle, probe_state::lost, cut_mode::autoHeight, link_kind::wired,
                                   link_kind::wired, probe_state::idle },
            "states, other values");
}

void an_entry_overrides_one_enumerator() {
    check_equal(json::encode(overridden {}), R"({"renamed":"waiting","reach":-1,"without_lost":"idle"})",
            "an entry renames one enumerator and renumbers another");
    round_trips_everywhere(overridden {}, "overridden");
    round_trips_everywhere(overridden { probe_state::busyNow, reachability::unreachable, probe_state::busyNow },
            "overridden, the enumerators left alone");

    const auto unknown = json::decode<overridden>(R"({"renamed":"exploded","reach":1,"without_lost":"idle"})");
    check(unknown && unknown->renamed == probe_state::lost, "the entry's fallback is the fallback");
    const auto by_identifier = json::decode<overridden>(R"({"renamed":"idle","reach":1,"without_lost":"idle"})");
    check(by_identifier && by_identifier->renamed == probe_state::lost,
            "an enumerator renamed here is no longer read by its identifier, so that is the fallback too");
    check(!json::decode<overridden>(R"({"renamed":"waiting","reach":1,"without_lost":"lost"})"),
            "and a skipped one is not read at all");
}

void it_reaches_into_containers() {
    check_equal(json::encode(containers {}),
            R"({"maybe":"lost","nothing":null,"history":["idle","lost"],"pair_of":["busyNow","idle"],)"
            R"("seen":["idle","lost"],"by_name":{"a":"lost"},"counts":[["busyNow",3]],"gaps":["idle",null]})",
            "every enumeration in the field is named, and the shapes are what they always are");
    round_trips_everywhere(containers {}, "containers");
}

void a_codec_writes_the_field() {
    check_equal(json::encode(coded {}),
            R"({"uptime":42,"count":42,"enabled":"yes","greeting":"hi!","started":"1700000000","offset":105})",
            "a codec, a serializer specialization and a pair of lambdas each write their field");
    round_trips_everywhere(coded {}, "coded");
    round_trips_everywhere(coded { std::chrono::milliseconds { 0 }, -4, false, "", timestamp { 0 }, -100 },
            "coded, other values");
}

void a_coded_member_is_still_required() {
    check(!json::decode<partly_there>(R"({"optional_one":2})"), "a member the type needs may not be left out");
    const auto without = json::decode<partly_there>(R"({"needed":4})");
    check(without && without->needed == 2 && without->optional_one == 7, "and a defaulted one may");
    const auto binary = bjdata::decode<partly_there>(bjdata::encode(partly_there {}));
    check(binary && binary->needed == 1 && binary->optional_one == 7, "in BJData too");
}

int main() {
    a_field_names_an_enumeration_that_does_not();
    an_entry_overrides_one_enumerator();
    it_reaches_into_containers();
    a_codec_writes_the_field();
    a_coded_member_is_still_required();
    return report("member_codec");
}
