#pragma once

// Which members of a described type can be written together.
//
// A member whose written form has a longest possible length - a boolean, a number, and the key
// before it - does not need to ask whether there is room for it if room for the longest it could
// be has already been found. Consecutive members of that kind are a run, and a writer that knows
// its runs asks for room once per run rather than once per key and once per value.
//
// What "longest possible" is differs by format, so the format says: `Widest<T, Member>::value`
// is the most a member can take, key included, or zero if it has no limit or is not written at
// all. A string has no limit the compiler can know, but its width is known the moment it is looked
// at, so a format may mark a member `text`: its `value` is then everything around the text, and
// the text's own length is added when the run is written. Everything else here is settled when
// the program is compiled.

#include <serpent/reflect.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

#if SERPENT_HAS_REFLECTION

namespace serpent::detail {

template<typename T, template<typename, std::meta::info> typename Widest>
struct member_runs {
    static constexpr auto members = std::define_static_array(members_including_bases<T>());

    static consteval std::size_t position_of(std::meta::info member) {
        return static_cast<std::size_t>(std::ranges::find(members, member) - members.begin());
    }

    /** The most each member can take, by position; zero ends a run. */
    static constexpr auto widest = [] {
        std::array<std::size_t, members.size()> each {};
        template for (constexpr auto member : members) each[position_of(member)] = Widest<T, member>::value;
        return each;
    }();

    /** Whether each member is text, whose own length is only known when it is written. */
    static constexpr auto text = [] {
        std::array<bool, members.size()> each {};
        template for (constexpr auto member : members) {
            if constexpr (requires { Widest<T, member>::text; }) each[position_of(member)] = Widest<T, member>::text;
        }
        return each;
    }();

    static consteval bool begins_run(std::size_t position) {
        return widest[position] != 0 && (position == 0 || widest[position - 1] == 0);
    }

    /** One past the last member of the run that `position` is in. */
    static consteval std::size_t run_end(std::size_t position) {
        while (position < widest.size() && widest[position] != 0) ++position;
        return position;
    }

    /** The most the members from `first` up to `last` can take together. */
    static consteval std::size_t widest_run(std::size_t first, std::size_t last) {
        std::size_t total = 0;
        for (std::size_t position = first; position < last; ++position) total += widest[position];
        return total;
    }

    /** Whether `member` is one of those from `first` up to `last`. */
    static consteval bool within(std::meta::info member, std::size_t first, std::size_t last) {
        return position_of(member) >= first && position_of(member) < last;
    }
};

} // namespace serpent::detail

#endif
