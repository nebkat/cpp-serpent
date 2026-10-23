#pragma once

// decode() and try_decode(): a whole document into one of your types.

#include <serpent/json/reader.hpp>

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

/** Reads a whole document into a value, refusing one that does not end where its text does. */
template<bool Terminated, typename T>
[[nodiscard]] std::optional<T> decode_whole(std::string_view text) {
    const auto root = basic_reader<Terminated>::over(text);
    auto value = root.template as<T>();
    if (!value || !ends_with_text(root, text)) return std::nullopt;
    return value;
}

template<bool Terminated, typename T>
[[nodiscard]] bool decode_whole_into(std::string_view text, T &value) {
    const auto root = basic_reader<Terminated>::over(text);
    return read_into(root, value) && ends_with_text(root, text);
}

} // namespace scanner

/**
 * Decodes a value from JSON text, or nullopt when it does not parse or does not fit the type.
 *
 * The whole text is the document: one that ends early, breaks partway or has anything but
 * whitespace after it is refused, as validate() would refuse it. Checked as it is read, so in
 * one pass.
 *
 * Text that is known to end in a zero byte - a std::string, a string literal - is read as
 * terminated, which is the faster reading: see scanner::basic_cursor. A string_view promises
 * nothing about what follows it and is read within its bounds. The choice is made by the
 * overload, so the text's own type says which; a buffer of one's own that does end in a zero
 * byte is read the terminated way through terminated_reader.
 */
template<typename T>
[[nodiscard]] std::optional<T> decode(std::string_view text) {
    return scanner::decode_whole<false, T>(text);
}

template<typename T>
[[nodiscard]] std::optional<T> decode(const std::string &text) {
    return scanner::decode_whole<true, T>(text);
}

template<typename T>
[[nodiscard]] std::optional<T> decode(const char *text) {
    return scanner::decode_whole<true, T>(text);
}

/**
 * Decodes into a value that already exists, which then keeps whatever capacity its containers
 * had - for reading many documents into the one object. A member the document leaves out keeps
 * its value, as it does for any read into an existing object; whether the document fits the type
 * is the answer.
 */
template<typename T>
bool decode_into(std::string_view text, T &value) {
    return scanner::decode_whole_into<false>(text, value);
}

template<typename T>
bool decode_into(const std::string &text, T &value) {
    return scanner::decode_whole_into<true>(text, value);
}

template<typename T>
bool decode_into(const char *text, T &value) {
    return scanner::decode_whole_into<true>(text, value);
}

/**
 * Decodes, saying why when it cannot.
 *
 * A document that does not parse is reported with the kind and the byte offset validate() found;
 * one that parses but does not fit the type is errc::type_mismatch, unless a member the type
 * needed was missing, which is named.
 *
 * One pass for a document that decodes; only one that does not is walked again, to say why.
 */
template<typename T, bool Terminated = false>
[[nodiscard]] std::expected<T, error> try_decode(std::string_view text) {
    const auto root = basic_reader<Terminated>::over(text);
    auto value = root.template as<T>();
    if (value && scanner::ends_with_text(root, text)) return std::expected<T, error> { std::move(*value) };
    if (const auto checked = validate<Terminated>(text); !checked) return std::unexpected { checked.error() };
    if (!value) {
        if (const auto absent = first_missing_member<T>(reader::over(text)); !absent.empty()) {
            return std::unexpected { error { errc::missing_key, 0, absent } };
        }
        return std::unexpected { error { errc::type_mismatch, 0 } };
    }
    // Read, and valid, yet not ending where the text does: no walk can disagree with validate()
    // about where a value ends, so this is only here to be total.
    return std::unexpected { error { errc::trailing_data, 0 } };
}

template<typename T>
[[nodiscard]] std::expected<T, error> try_decode(const std::string &text) {
    return try_decode<T, true>(std::string_view { text });
}

template<typename T>
[[nodiscard]] std::expected<T, error> try_decode(const char *text) {
    return try_decode<T, true>(std::string_view { text });
}

} // namespace serpent::json
