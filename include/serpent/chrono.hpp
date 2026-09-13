#pragma once

// Durations, time points and hh_mm_ss.
//
// Its own header so that <chrono> stays out of translation units that do not want it, the same
// reason ostream_sink has one.
//
// A duration travels as its count and nothing else: the unit lives in the type and never goes on
// the wire. Both ends therefore have to name the same duration type, and nothing here can check
// that they did - writing milliseconds and reading seconds is silently wrong by a factor of a
// thousand. Say which unit a field is in, in the field's name or in the document's schema, the
// way you would for any other bare number.

#include <serpent/serializer.hpp>

#include <chrono>

namespace serpent {

/** A duration is its count. */
template<typename Rep, typename Period>
struct serializer<std::chrono::duration<Rep, Period>, void> {
    using value_type = std::chrono::duration<Rep, Period>;

    template<typename Writer>
    static void write(Writer &out, const value_type &value) {
        out.value(value.count());
    }

    template<typename Source>
    static bool read(Source source, value_type &value) {
        Rep count {};
        if (!read_into(source, count)) return false;
        value = value_type { count };
        return true;
    }
};

/** A time point is the duration since its clock's epoch, and defers to that duration. */
template<typename Clock, typename Duration>
struct serializer<std::chrono::time_point<Clock, Duration>, void> {
    using value_type = std::chrono::time_point<Clock, Duration>;

    template<typename Writer>
    static void write(Writer &out, const value_type &value) {
        serializer<Duration, void>::write(out, value.time_since_epoch());
    }

    template<typename Source>
    static bool read(Source source, value_type &value) {
        Duration since {};
        if (!serializer<Duration, void>::read(source, since)) return false;
        value = value_type { since };
        return true;
    }
};

/** A time of day is the duration it was built from, and defers to that too. */
template<typename Duration>
struct serializer<std::chrono::hh_mm_ss<Duration>, void> {
    using value_type = std::chrono::hh_mm_ss<Duration>;
    using precision = typename value_type::precision;

    template<typename Writer>
    static void write(Writer &out, const value_type &value) {
        serializer<precision, void>::write(out, value.to_duration());
    }

    template<typename Source>
    static bool read(Source source, value_type &value) {
        precision elapsed {};
        if (!serializer<precision, void>::read(source, elapsed)) return false;
        value = value_type { elapsed };
        return true;
    }
};

} // namespace serpent
