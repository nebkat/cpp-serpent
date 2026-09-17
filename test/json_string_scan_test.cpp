// Where a string's plain text ends is found eight bytes at a time, by arithmetic on the whole
// word that is easy to get subtly wrong: a borrow that spills into the next byte, a byte with
// its top bit set, a word that straddles the end. It has to give the answer that looking at
// every byte gives, so it is compared with that - for every byte value at every position in
// text of every short length, over fillers chosen to sit at the edges of each test.

#include "check.hpp"

#include <serpent/json/scan.hpp>

#include <random>
#include <string>

namespace scanner = serpent::json::scanner;

namespace {

int compared = 0;

void same_as_one_at_a_time(const std::string &text) {
    const char *const begin = text.data();
    const char *const end = begin + text.size();
    // From every starting point, since a run begins wherever the last escape left off.
    for (const char *from = begin; from <= end; ++from) {
        const auto wide = scanner::end_of_plain_text(from, end) - begin;
        const auto narrow = scanner::advance_while(from, end, scanner::class_string_body) - begin;
        ++compared;
        if (wide != narrow) check_equal(wide, narrow, "where plain text ends");
    }
}

} // namespace

int main() {
    // 0x20 is the smallest byte that needs no escape, 0x1f the largest that does; 0x21 and 0x23
    // are the neighbours of the quote, 0x5b and 0x5d of the backslash; the rest have the top bit.
    for (const char filler : { 'a', '\x20', '\x21', '\x23', '\x5b', '\x5d', '\x7f', '\x80', '\xff' }) {
        for (std::size_t length = 0; length <= 26; ++length) {
            same_as_one_at_a_time(std::string(length, filler));
            for (std::size_t position = 0; position < length; ++position) {
                for (int value = 0; value < 256; ++value) {
                    std::string text(length, filler);
                    text[position] = static_cast<char>(value);
                    same_as_one_at_a_time(text);
                }
            }
        }
    }

    // Several bytes to escape close together, which is where a borrow has somewhere to spill.
    std::mt19937_64 random { 20260917 };
    constexpr std::string_view alphabet = "ab \x1f\x20\x21\"\\\x01\x7f\x80\xff";
    for (int index = 0; index < 200'000; ++index) {
        std::string text;
        const std::size_t length = random() % 40;
        for (std::size_t each = 0; each < length; ++each) text.push_back(alphabet[random() % alphabet.size()]);
        same_as_one_at_a_time(text);
    }

    std::printf("SERPENT_WIDE_STRING_SCAN=%d, %d runs compared\n", SERPENT_WIDE_STRING_SCAN, compared);
    return report("json_string_scan");
}
