#pragma once

#include <nonstd/bjdata/detail.hpp>
#include <nonstd/bjdata/marker.hpp>
#include <nonstd/bjdata/real_format.hpp>
#include <nonstd/bjdata/view.hpp>

#include <charconv>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include <cstddef>

namespace nonstd::bjdata {

namespace detail {

inline void write_block(std::string &out, std::string_view text) {
    out += '[';
    out += text;
    out += ']';
}

inline void write_block(std::string &out, marker value) {
    out += '[';
    out += static_cast<char>(value);
    out += ']';
}

/** Emits a <marker><integer> pair as two blocks and returns its value. */
inline std::int64_t notate_marked_integer(std::string &out, cursor &source) {
    if (!source.need(1)) return 0;

    const auto kind = to_marker(source.peek());
    if (!is_integer(kind)) {
        source.fail(errc::invalid_length);
        return 0;
    }
    source.advance(1);
    write_block(out, kind);

    const auto width = payload_width(kind);
    if (!source.need(width)) return 0;

    const auto value = load_integer(kind, source.position);
    source.advance(width);
    write_block(out, kind == marker::uint64
            ? std::format("{}", load<std::uint64_t>(source.position - width))
            : std::format("{}", value));
    return value;
}

inline void notate_string_body(std::string &out, cursor &source) {
    const auto length = notate_marked_integer(out, source);
    if (length < 0) {
        source.fail(errc::negative_length);
        return;
    }
    if (!source.need(static_cast<std::uint64_t>(length))) return;
    write_block(out, std::string_view { reinterpret_cast<const char *>(source.position), static_cast<std::size_t>(length) });
    source.advance(static_cast<std::uint64_t>(length));
}

void notate_value(std::string &out, cursor &source, marker kind, int depth);

/** Emits the count after '#', which may be a plain integer or a dimension array. */
inline std::uint64_t notate_count(std::string &out, cursor &source) {
    if (!source.need(1)) return 0;

    if (source.peek_marker() != marker::array_begin) {
        const auto value = notate_marked_integer(out, source);
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }

    // Re-parse the dimension list for its product, then emit it token by token.
    auto measured = source;
    const auto shape = read_count(measured);

    source.advance(1);
    write_block(out, marker::array_begin);
    bool column_major = false;
    if (source.available(1) && source.peek_marker() == marker::array_begin) {
        source.advance(1);
        write_block(out, marker::array_begin);
        column_major = true;
    }

    if (source.available(1) && source.peek_marker() == marker::strong_type) {
        source.advance(1);
        write_block(out, marker::strong_type);
        if (!source.need(1)) return 0;
        const auto element = to_marker(source.peek());
        source.advance(1);
        write_block(out, element);
        if (!source.need(1)) return 0;
        source.advance(1);
        write_block(out, marker::count);
        const auto entries = notate_marked_integer(out, source);
        const auto width = payload_width(element);
        for (std::int64_t index = 0; index < entries && source.ok(); ++index) {
            if (!source.need(width)) return 0;
            write_block(out, std::format("{}", load_integer(element, source.position)));
            source.advance(width);
        }
    } else if (source.available(1) && source.peek_marker() == marker::count) {
        source.advance(1);
        write_block(out, marker::count);
        const auto entries = notate_marked_integer(out, source);
        for (std::int64_t index = 0; index < entries && source.ok(); ++index) notate_marked_integer(out, source);
    } else {
        while (source.ok() && source.available(1) && source.peek_marker() != marker::array_end) {
            notate_marked_integer(out, source);
        }
        if (source.need(1)) {
            source.advance(1);
            write_block(out, marker::array_end);
        }
    }

    if (column_major && source.need(1)) {
        source.advance(1);
        write_block(out, marker::array_end);
    }
    return shape.total;
}

inline void notate_container(std::string &out, cursor &source, bool object, int depth) {
    marker element = marker::invalid;
    std::uint64_t count = 0;
    bool unbounded = false;

    if (!source.need(1)) return;

    if (source.peek_marker() == marker::strong_type) {
        source.advance(1);
        write_block(out, marker::strong_type);
        if (!source.need(1)) return;
        element = to_marker(source.peek());
        source.advance(1);
        write_block(out, element);
        if (!source.need(1)) return;
        source.advance(1);
        write_block(out, marker::count);
        count = notate_count(out, source);
    } else if (source.peek_marker() == marker::count) {
        source.advance(1);
        write_block(out, marker::count);
        count = notate_count(out, source);
    } else {
        unbounded = true;
    }

    const auto emit_typed_element = [&] {
        const auto width = payload_width(element);
        if (!source.need(width)) return;
        if (is_float(element)) {
            const auto value = element == marker::float16 ? decode_float16(load<std::uint16_t>(source.position))
                    : element == marker::float32          ? load<float>(source.position)
                                                          : static_cast<float>(load<double>(source.position));
            write_block(out, format_real(value));
        } else if (element == marker::character) {
            write_block(out, std::string_view { reinterpret_cast<const char *>(source.position), 1 });
        } else {
            write_block(out, std::format("{}", load_integer(element, source.position)));
        }
        source.advance(width);
    };

    const auto emit_element = [&] {
        if (!source.need(1)) return;
        const auto kind = to_marker(source.peek());
        source.advance(1);
        write_block(out, kind);
        notate_value(out, source, kind, depth + 1);
    };

    if (unbounded) {
        const auto terminator = object ? marker::object_end : marker::array_end;
        while (source.ok() && source.available(1)) {
            if (source.peek_marker() == terminator) {
                source.advance(1);
                write_block(out, terminator);
                return;
            }
            if (source.peek_marker() == marker::noop) {
                source.advance(1);
                write_block(out, marker::noop);
                continue;
            }
            if (object) notate_string_body(out, source);
            emit_element();
        }
        source.fail(errc::unterminated_container);
        return;
    }

    if (element != marker::invalid && !object) {
        for (std::uint64_t index = 0; index < count && source.ok(); ++index) emit_typed_element();
        return;
    }

    for (std::uint64_t index = 0; index < count && source.ok(); ++index) {
        while (source.available(1) && source.peek_marker() == marker::noop) {
            source.advance(1);
            write_block(out, marker::noop);
        }
        if (object) {
            notate_string_body(out, source);
            if (element != marker::invalid) {
                emit_typed_element();
                continue;
            }
        }
        emit_element();
    }
}

inline void notate_value(std::string &out, cursor &source, marker kind, int depth) {
    if (depth > max_depth) {
        source.fail(errc::depth_exceeded);
        return;
    }

    switch (kind) {
        case marker::null:
        case marker::boolean_true:
        case marker::boolean_false:
        case marker::noop:
            return;

        case marker::string:
        case marker::high_precision:
            notate_string_body(out, source);
            return;

        case marker::character: {
            if (!source.need(1)) return;
            write_block(out, std::string_view { reinterpret_cast<const char *>(source.position), 1 });
            source.advance(1);
            return;
        }

        case marker::float16:
        case marker::float32:
        case marker::float64: {
            const auto width = payload_width(kind);
            if (!source.need(width)) return;
            const double value = kind == marker::float16 ? decode_float16(load<std::uint16_t>(source.position))
                    : kind == marker::float32            ? load<float>(source.position)
                                                         : load<double>(source.position);
            write_block(out, format_real(value));
            source.advance(width);
            return;
        }

        case marker::array_begin:
            notate_container(out, source, false, depth);
            return;
        case marker::object_begin:
            notate_container(out, source, true, depth);
            return;

        default: {
            const auto width = payload_width(kind);
            if (width == variable_width) {
                source.fail(errc::unexpected_marker);
                return;
            }
            if (!source.need(width)) return;
            if (kind == marker::uint64) {
                write_block(out, std::format("{}", load<std::uint64_t>(source.position)));
            } else {
                write_block(out, std::format("{}", load_integer(kind, source.position)));
            }
            source.advance(width);
            return;
        }
    }
}

}// namespace detail

/**
 * @brief Renders a document as dart-bjdata's block notation, e.g. [S][U][5][hello].
 *
 * A token-level trace of the byte stream rather than of the decoded value, so it catches
 * grammar drift that comparing decoded values would miss.
 */
[[nodiscard]] inline std::string block_notation(std::span<const std::byte> buffer) {
    std::string out;
    detail::cursor source { buffer, buffer.data() };

    if (!source.need(1)) return out;
    const auto root = to_marker(source.peek());
    source.advance(1);
    detail::write_block(out, root);
    detail::notate_value(out, source, root, 0);
    return out;
}

[[nodiscard]] inline std::string block_notation(const view &value) {
    return block_notation(value.buffer());
}

}// namespace nonstd::bjdata
