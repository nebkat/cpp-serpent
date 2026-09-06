#pragma once

namespace nonstd::bjdata {

/** The JSON-level shape of a value, as opposed to whatever a format spells it with. */
enum class kind {
    invalid,
    null,
    boolean,
    integer,
    real,
    string,
    array,
    object,
};

}// namespace nonstd::bjdata
