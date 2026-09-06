#pragma once

#include <nonstd/bjdata/reflect.hpp>
#include <nonstd/bjdata/view.hpp>
#include <nonstd/bjdata/visitor.hpp>
#include <nonstd/bjdata/writer.hpp>

#include <algorithm>
#include <concepts>
#include <optional>
#include <vector>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nonstd::bjdata {

/** A type that names its fields once, for both directions. */
template<typename T>
concept convertible_type = requires(write_visitor &visitor, const T &value) { bjdata_convert(visitor, value); };

/** A type with a separate writer, for when the two directions genuinely differ. */
template<typename T>
concept writable_type = requires(writer &out, const T &value) { to_bjdata(out, value); };

/** A type with a separate reader. Names `view`, so it reads BJData only. */
template<typename T>
concept readable_type = requires(view source, T &value) { from_bjdata(source, value); };

/**
 * @brief The dispatch point for user types, specializable for types you cannot add
 *        functions to.
 *
 * By default it prefers a single bjdata_convert and falls back to the to_bjdata /
 * from_bjdata pair.
 */
template<typename T, typename>
struct serializer {
    static void write(writer &out, const T &value) {
        if constexpr (convertible_type<T>) {
            write_visitor visitor { out };
            const auto scope = out.object();
            bjdata_convert(visitor, value);
        } else {
            static_assert(writable_type<T>,
                          "no to_bjdata(writer &, const T &) or bjdata_convert for this type; "
                          "define one, or specialize nonstd::bjdata::serializer<T>");
            to_bjdata(out, value);
        }
    }

    /**
     * Reads from any source that offers the reader interface - a BJData view or a JSON
     * reader. A type using bjdata_convert therefore reads both formats from one definition;
     * one using from_bjdata names `view`, so it reads BJData only.
     */
    template<typename Source>
    static bool read(Source source, T &value) {
        if constexpr (convertible_type<T>) {
            if (!source.is_object()) return false;
            read_visitor<Source> visitor { source };
            bjdata_convert(visitor, value);
            return visitor.ok();
        } else if constexpr (std::same_as<Source, view> && readable_type<T>) {
            return from_bjdata(source, value);
        } else {
            static_assert(always_false<Source>,
                          "this type has no bjdata_convert, so it can only be read from BJData "
                          "through from_bjdata(view, T &); give it a bjdata_convert to read any "
                          "format, or specialize nonstd::bjdata::serializer<T>");
            return false;
        }
    }
};

/** Reads one value into a destination, handling optionals and containers along the way. */
template<typename Source, typename T>
bool read_into(Source source, T &value) {
    if constexpr (detail::optional_like<T>) {
        if (!source.is_valid() || source.is_null()) {
            value.reset();
            return true;
        }
        std::remove_cvref_t<decltype(*value)> item {};
        if (!read_into(source, item)) return false;
        value = std::move(item);
        return true;
    } else if constexpr (detail::byte_range<T>) {
        if constexpr (requires(T &target) { target.clear(); }) value.clear();
        if constexpr (requires { source.as_binary(); }) {
            const auto bytes = source.as_binary();
            if (!bytes) return false;
            if constexpr (requires(T &target) { target.assign(bytes->begin(), bytes->end()); }) {
                value.assign(bytes->begin(), bytes->end());
            } else {
                if (bytes->size() != std::ranges::size(value)) return false;
                std::ranges::copy(*bytes, std::ranges::begin(value));
            }
            return true;
        } else {
            // JSON has no binary, and this library writes it as an array of integers.
            if (!source.is_array()) return false;
            std::size_t index = 0;
            for (const auto element : source.array()) {
                const auto octet = element.template as_int<std::uint8_t>();
                if (!octet) return false;
                if constexpr (requires(T &target) { target.push_back(std::byte {}); }) {
                    value.push_back(static_cast<std::byte>(*octet));
                } else {
                    if (index >= std::ranges::size(value)) return false;
                    *(std::ranges::begin(value) + static_cast<std::ptrdiff_t>(index)) = static_cast<std::byte>(*octet);
                }
                ++index;
            }
            if constexpr (!requires(T &target) { target.push_back(std::byte {}); }) {
                return index == std::ranges::size(value);
            }
            return true;
        }
    } else if constexpr (requires(T &target) { target.clear(); target.emplace_back(); }) {
        if (!source.is_array()) return false;
        value.clear();
        for (const auto element : source.array()) {
            auto &slot = value.emplace_back();
            if (!read_into(element, slot)) return false;
        }
        return true;
    } else if constexpr (requires(T &target) { target.clear(); target.emplace(typename T::key_type {}, typename T::mapped_type {}); }) {
        if (!source.is_object()) return false;
        value.clear();
        for (const auto entry : source.items()) {
            typename T::mapped_type slot {};
            if (!read_into(entry.value, slot)) return false;
            // A BJData key is already a view of the buffer; a JSON key has to be decoded.
            if constexpr (std::constructible_from<typename T::key_type, decltype(entry.key)>) {
                value.emplace(typename T::key_type { entry.key }, std::move(slot));
            } else {
                value.emplace(typename T::key_type { entry.key_string() }, std::move(slot));
            }
        }
        return true;
    } else {
        auto found = source.template try_get<T>();
        if (!found) return false;
        value = std::move(*found);
        return true;
    }
}

// ---------------- document level ----------------

/** Copies a value into a writer with no re-encoding: its marker, then its bytes. */
inline void write_value(writer &out, view source) noexcept {
    const auto payload = source.payload_bytes();
    if (!payload) {
        out.fail(errc::type_mismatch);
        return;
    }
    out.put_marker(source.type_marker());
    out.put(*payload);
}

/** Writes one value to a sink and reports the bytes produced, or the first failure. */
template<sink S, typename T>
std::expected<std::size_t, error> write(S &out, const T &value, writer_options options = {}) {
    writer target { out, options };
    target.value(value);
    return target.finish();
}

/** Encodes a value into a fresh buffer. The allocating convenience over write(). */
template<typename T>
[[nodiscard]] std::vector<std::byte> to_bytes(const T &value, writer_options options = {}) {
    std::vector<std::byte> buffer;
    container_sink out { buffer };
    writer target { out, options };
    target.value(value);
    if (!target.finish()) buffer.clear();
    return buffer;
}

/** Decodes a value from a buffer, or nullopt when it does not parse. */
template<typename T>
[[nodiscard]] std::optional<T> from_bytes(std::span<const std::byte> buffer) {
    return view::over(buffer).try_get<T>();
}

/** The exact byte length a value would occupy, with no allocation. */
template<typename T>
[[nodiscard]] std::size_t measure(const T &value, writer_options options = {}) {
    counting_sink counter;
    writer target { counter, options };
    target.value(value);
    return counter.size();
}

}// namespace nonstd::bjdata

// ---------------- member listing ----------------
//
// Field names cannot be recovered without reflection, so listing them in a macro is the only
// option. The shape deliberately matches the firmware's NONSTD_JSON_DEFINE_TYPE family.

#define BJDATA_EXPAND(x) x
#define BJDATA_MEMBER(name) visitor.member(#name, value.name);

#define BJDATA_PASTE1(step, v1) step(v1)
#define BJDATA_PASTE2(step, v1, v2) step(v1) BJDATA_PASTE1(step, v2)
#define BJDATA_PASTE3(step, v1, v2, v3) step(v1) BJDATA_PASTE2(step, v2, v3)
#define BJDATA_PASTE4(step, v1, v2, v3, v4) step(v1) BJDATA_PASTE3(step, v2, v3, v4)
#define BJDATA_PASTE5(step, v1, v2, v3, v4, v5) step(v1) BJDATA_PASTE4(step, v2, v3, v4, v5)
#define BJDATA_PASTE6(step, v1, v2, v3, v4, v5, v6) step(v1) BJDATA_PASTE5(step, v2, v3, v4, v5, v6)
#define BJDATA_PASTE7(step, v1, v2, v3, v4, v5, v6, v7) step(v1) BJDATA_PASTE6(step, v2, v3, v4, v5, v6, v7)
#define BJDATA_PASTE8(step, v1, v2, v3, v4, v5, v6, v7, v8) step(v1) BJDATA_PASTE7(step, v2, v3, v4, v5, v6, v7, v8)
#define BJDATA_PASTE9(step, v1, v2, v3, v4, v5, v6, v7, v8, v9) step(v1) BJDATA_PASTE8(step, v2, v3, v4, v5, v6, v7, v8, v9)
#define BJDATA_PASTE10(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10) step(v1) BJDATA_PASTE9(step, v2, v3, v4, v5, v6, v7, v8, v9, v10)
#define BJDATA_PASTE11(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11) step(v1) BJDATA_PASTE10(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11)
#define BJDATA_PASTE12(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12) step(v1) BJDATA_PASTE11(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12)
#define BJDATA_PASTE13(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13) step(v1) BJDATA_PASTE12(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13)
#define BJDATA_PASTE14(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14) step(v1) BJDATA_PASTE13(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14)
#define BJDATA_PASTE15(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) step(v1) BJDATA_PASTE14(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15)
#define BJDATA_PASTE16(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16) step(v1) BJDATA_PASTE15(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16)
#define BJDATA_PASTE17(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17) step(v1) BJDATA_PASTE16(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17)
#define BJDATA_PASTE18(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18) step(v1) BJDATA_PASTE17(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18)
#define BJDATA_PASTE19(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19) step(v1) BJDATA_PASTE18(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19)
#define BJDATA_PASTE20(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20) step(v1) BJDATA_PASTE19(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20)
#define BJDATA_PASTE21(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21) step(v1) BJDATA_PASTE20(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21)
#define BJDATA_PASTE22(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22) step(v1) BJDATA_PASTE21(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22)
#define BJDATA_PASTE23(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23) step(v1) BJDATA_PASTE22(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23)
#define BJDATA_PASTE24(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24) step(v1) BJDATA_PASTE23(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24)
#define BJDATA_PASTE25(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25) step(v1) BJDATA_PASTE24(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25)
#define BJDATA_PASTE26(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26) step(v1) BJDATA_PASTE25(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26)
#define BJDATA_PASTE27(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27) step(v1) BJDATA_PASTE26(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27)
#define BJDATA_PASTE28(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28) step(v1) BJDATA_PASTE27(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28)
#define BJDATA_PASTE29(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29) step(v1) BJDATA_PASTE28(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29)
#define BJDATA_PASTE30(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30) step(v1) BJDATA_PASTE29(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30)
#define BJDATA_PASTE31(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31) step(v1) BJDATA_PASTE30(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31)
#define BJDATA_PASTE32(step, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32) step(v1) BJDATA_PASTE31(step, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32)

#define BJDATA_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, NAME, ...) NAME
#define BJDATA_FOR_EACH(step, ...) \
    BJDATA_EXPAND(BJDATA_SELECT(__VA_ARGS__, BJDATA_PASTE32, BJDATA_PASTE31, BJDATA_PASTE30, BJDATA_PASTE29, BJDATA_PASTE28, BJDATA_PASTE27, BJDATA_PASTE26, BJDATA_PASTE25, BJDATA_PASTE24, BJDATA_PASTE23, BJDATA_PASTE22, BJDATA_PASTE21, BJDATA_PASTE20, BJDATA_PASTE19, BJDATA_PASTE18, BJDATA_PASTE17, BJDATA_PASTE16, BJDATA_PASTE15, BJDATA_PASTE14, BJDATA_PASTE13, BJDATA_PASTE12, BJDATA_PASTE11, BJDATA_PASTE10, BJDATA_PASTE9, BJDATA_PASTE8, BJDATA_PASTE7, BJDATA_PASTE6, BJDATA_PASTE5, BJDATA_PASTE4, BJDATA_PASTE3, BJDATA_PASTE2, BJDATA_PASTE1)(step, __VA_ARGS__))

/** Defines both directions as a hidden friend. Place inside the type. */
#define BJDATA_DEFINE_TYPE(Type, ...)                                                              \
    friend void bjdata_convert(auto &visitor,                                                      \
                               ::nonstd::bjdata::conversion_object_t<decltype(visitor), Type> value) { \
        BJDATA_FOR_EACH(BJDATA_MEMBER, __VA_ARGS__)                                                \
    }

/** Defines both directions as a free function. Place beside the type, in its namespace. */
#define BJDATA_DEFINE_TYPE_NON_INTRUSIVE(Type, ...)                                                \
    inline void bjdata_convert(auto &visitor,                                                      \
                               ::nonstd::bjdata::conversion_object_t<decltype(visitor), Type> value) { \
        BJDATA_FOR_EACH(BJDATA_MEMBER, __VA_ARGS__)                                                \
    }
