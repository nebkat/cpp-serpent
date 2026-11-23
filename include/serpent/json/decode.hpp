#pragma once

// decode() and decode_into(): a whole document into one of your types.

#include <serpent/json/reader.hpp>
#include <serpent/locate.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace serpent::json {

namespace scanner {

/**
 * Whether a document that has been read ends where its text does, whitespace aside. A container
 * cut short or broken partway leaves its walk short of its end, so this is what turns "the
 * elements ran out" into "the document is broken" - and anything after it into a refusal.
 */
template<bool Terminated>
[[nodiscard]] bool ends_with_text(const basic_reader<Terminated> &root, std::string_view text) noexcept {
    const char *const end = root.value_end();
    if (end == nullptr) return false;
    basic_cursor<Terminated> scan { text, end };
    skip_whitespace(scan);
    return scan.position == scan.limit;
}

/**
 * Why a document did not decode as T: where it stops parsing, or, valid, why it does not fit -
 * which takes walking it again, so this is only ever on the way to an error.
 */
template<bool Terminated, typename T>
[[nodiscard]] error explain(std::string_view text, bool read, decode_options options) {
    if (const auto checked = validate<Terminated>(text); !checked) return checked.error();
    if (!read) return serpent::detail::explain_mismatch<T>(basic_reader<Terminated>::over(text), options);
    // Read, and valid, yet not ending where the text does: no walk can disagree with validate()
    // about where a value ends, so this is only here to be total.
    return error { errc::trailing_data, 0 };
}

/** Reads a whole document into a value, refusing one that does not end where its text does. */
template<bool Terminated, typename T>
[[nodiscard]] std::expected<T, error> decode_whole(std::string_view text, decode_options options) {
    const auto root = basic_reader<Terminated>::over(text);
    auto value = root.template as<T>();
    if (value && ends_with_text(root, text)) return std::expected<T, error> { std::move(*value) };
    return std::unexpected { explain<Terminated, T>(text, value.has_value(), options) };
}

template<bool Terminated, typename T>
[[nodiscard]] std::expected<void, error> decode_whole_into(std::string_view text, T &value, decode_options options) {
    const auto root = basic_reader<Terminated>::over(text);
    const bool read = read_into(root, value);
    if (read && ends_with_text(root, text)) return {};
    return std::unexpected { explain<Terminated, T>(text, read, options) };
}

} // namespace scanner

/**
 * Decodes a value from JSON text, or says why it cannot.
 *
 * The whole text is the document: one that ends early, breaks partway or has anything but
 * whitespace after it is refused, as validate() would refuse it. Checked as it is read, so a
 * document that decodes is read once; only one that does not is walked again, to say why.
 *
 * A document that does not parse is reported with the kind and the byte offset validate() found;
 * one that parses but does not fit the type is errc::type_mismatch, unless a member the type
 * needed was missing, which is named. With `options.trace` the error also has the path(), offset
 * and found() kind of the value at fault, and a missing key is found at any depth.
 *
 * Text that is known to end in a zero byte - a std::string, a string literal - is read as
 * terminated, which is the faster reading: see scanner::basic_cursor. A string_view promises
 * nothing about what follows it and is read within its bounds. The choice is made by the
 * overload, so the text's own type says which; a buffer of one's own that does end in a zero
 * byte is read the terminated way through terminated_reader.
 */
template<typename T>
[[nodiscard]] std::expected<T, error> decode(std::string_view text, decode_options options = {}) {
    return scanner::decode_whole<false, T>(text, options);
}

template<typename T>
[[nodiscard]] std::expected<T, error> decode(const std::string &text, decode_options options = {}) {
    return scanner::decode_whole<true, T>(text, options);
}

template<typename T>
[[nodiscard]] std::expected<T, error> decode(const char *text, decode_options options = {}) {
    return scanner::decode_whole<true, T>(text, options);
}

/**
 * Decodes into a value that already exists, which then keeps whatever capacity its containers
 * had - for reading many documents into the one object. A member the document leaves out keeps
 * its value, as it does for any read into an existing object; whether the document fits the type
 * is the answer, and why not is told as decode() tells it.
 */
template<typename T>
std::expected<void, error> decode_into(std::string_view text, T &value, decode_options options = {}) {
    return scanner::decode_whole_into<false>(text, value, options);
}

template<typename T>
std::expected<void, error> decode_into(const std::string &text, T &value, decode_options options = {}) {
    return scanner::decode_whole_into<true>(text, value, options);
}

template<typename T>
std::expected<void, error> decode_into(const char *text, T &value, decode_options options = {}) {
    return scanner::decode_whole_into<true>(text, value, options);
}

} // namespace serpent::json
