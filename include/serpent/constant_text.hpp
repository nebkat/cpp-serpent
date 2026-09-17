#pragma once

// Text known when the program is compiled, held so that its length is part of its type.
//
// A key and the punctuation around it never change, so reading one is a comparison and writing one
// is a copy - and both are several times quicker when the compiler knows how many characters are
// involved. It knows that for certain only when the length is in the type. Passed as a
// std::string_view the length is a constant just where the call happens to be inlined, and whether
// it is depends on what else the translation unit holds: the same source has measured two and
// three times slower in a larger program for no other reason.

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace serpent::detail {

/**
 * Several pieces of text run together as one array.
 *
 * `Width` has to be the sum of the pieces' lengths, stated by the caller because a return type
 * cannot depend on an argument's value. Built a character at a time rather than by joining
 * std::strings: under the undefined-behaviour sanitizer GCC will not fold std::string's
 * null-pointer check on a literal, and the concatenation stops being a constant expression.
 */
template<std::size_t Width>
[[nodiscard]] consteval std::array<char, Width> joined(std::initializer_list<std::string_view> pieces) {
    std::array<char, Width> characters {};
    std::size_t index = 0;
    for (const auto piece : pieces)
        for (const char character : piece) characters[index++] = character;
    return characters;
}

} // namespace serpent::detail
