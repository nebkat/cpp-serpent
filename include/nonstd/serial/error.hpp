#pragma once

#include <exception>

#include <cstddef>
#include <cstdlib>

namespace nonstd::serial {

enum class errc {
    ok = 0,

    unexpected_end,             ///< the value runs past the end of the buffer
    trailing_data,              ///< the root value does not consume the whole buffer
    invalid_marker,             ///< byte is not a known marker
    unexpected_marker,          ///< a known marker, but not one that may appear here
    extension_unsupported,      ///< 'E'; must be rejected rather than skipped
    depth_exceeded,             ///< nesting deeper than max_depth

    invalid_length,             ///< length or count prefix is not an integer marker
    negative_length,            ///< length or count is negative
    invalid_strong_type,        ///< marker after '$' may not be a strong type
    missing_count,              ///< '$' was not followed by '#'
    unterminated_container,     ///< ran out of input before ']' or '}'

    invalid_dimensions,         ///< empty, negative, or non-integer dimension list
    dimension_overflow,         ///< rank above max_dimensions, or the element product overflows
    object_dimension_count,     ///< an object may not be counted by a dimension array

    type_mismatch,              ///< the value is not of the requested type
    out_of_range,               ///< no such key or index, or the value does not fit the requested type

    unexpected_character,       ///< a character that cannot begin or continue a JSON value
    invalid_number,             ///< not a JSON number: a leading zero, a bare '.', a missing exponent
    invalid_string,             ///< an unescaped control character inside a string
    invalid_escape,             ///< an unknown escape, a short \u, or an unpaired surrogate

    sink_failed,                ///< the destination refused a write, e.g. a full fixed buffer
    unbalanced_container,       ///< an end that does not match its begin
    key_outside_object,         ///< a key written where no object is open
};

[[nodiscard]] constexpr const char *message(errc code) noexcept {
    switch (code) {
        case errc::ok:                     return "ok";
        case errc::unexpected_end:         return "unexpected end of input";
        case errc::trailing_data:          return "trailing data after the root value";
        case errc::invalid_marker:         return "invalid marker";
        case errc::unexpected_marker:      return "unexpected marker";
        case errc::extension_unsupported:  return "extension types are not supported";
        case errc::depth_exceeded:         return "maximum nesting depth exceeded";
        case errc::invalid_length:         return "length is not an integer marker";
        case errc::negative_length:        return "negative length";
        case errc::invalid_strong_type:    return "invalid strong type";
        case errc::missing_count:          return "strong type is not followed by a count";
        case errc::unterminated_container: return "unterminated container";
        case errc::invalid_dimensions:     return "invalid dimension array";
        case errc::dimension_overflow:     return "too many dimensions";
        case errc::object_dimension_count: return "an object may not be counted by a dimension array";
        case errc::type_mismatch:          return "type mismatch";
        case errc::out_of_range:           return "out of range";
        case errc::unexpected_character:   return "unexpected character";
        case errc::invalid_number:         return "invalid number";
        case errc::invalid_string:         return "invalid string";
        case errc::invalid_escape:         return "invalid escape sequence";
        case errc::sink_failed:            return "the destination refused a write";
        case errc::unbalanced_container:   return "unbalanced container";
        case errc::key_outside_object:     return "key written outside an object";
    }
    return "unknown error";
}

/**
 * @brief A parse or access failure, carrying the byte offset it was detected at.
 *
 * Used both as the error type of validate() and as the object thrown by the checked
 * accessors, so a caller may pick either style over the same bytes.
 */
class error : public std::exception {
    errc value = errc::ok;
    std::size_t byte_offset = 0;

public:
    error() = default;
    constexpr error(errc value, std::size_t byte_offset) noexcept: value(value), byte_offset(byte_offset) {}

    [[nodiscard]] constexpr errc code() const noexcept { return this->value; }
    [[nodiscard]] constexpr std::size_t offset() const noexcept { return this->byte_offset; }

    [[nodiscard]] const char *what() const noexcept override { return message(this->value); }
};

/**
 * Raises a failure from the checked accessors. The total accessors never call this, so a
 * build with exceptions disabled keeps the whole poisoning API intact.
 */
[[noreturn]] inline void raise(errc code, std::size_t offset) {
#if defined(__cpp_exceptions) && __cpp_exceptions
    throw error { code, offset };
#else
    (void) code;
    (void) offset;
    std::abort();
#endif
}

}// namespace nonstd::serial
