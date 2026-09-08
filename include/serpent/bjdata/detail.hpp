#pragma once

#include <serpent/error.hpp>
#include <serpent/bjdata/marker.hpp>
#include <nonstd/unaligned_ptr.hpp>

#include <limits>
#include <span>
#include <string_view>

#include <cstddef>
#include <cstdint>

namespace serpent::bjdata {

namespace detail {

template<typename T>
[[nodiscard]] inline T load(const std::byte *position) noexcept {
    return *nonstd::unaligned_little_ptr<const T> { position };
}

/** Reads a fixed-width integer payload, sign-extending the signed markers. */
[[nodiscard]] inline std::int64_t load_integer(marker kind, const std::byte *position) noexcept {
    switch (kind) {
    case marker::uint8: return load<std::uint8_t>(position);
    case marker::int8: return load<std::int8_t>(position);
    case marker::uint16: return load<std::uint16_t>(position);
    case marker::int16: return load<std::int16_t>(position);
    case marker::uint32: return load<std::uint32_t>(position);
    case marker::int32: return load<std::int32_t>(position);
    case marker::uint64: return static_cast<std::int64_t>(load<std::uint64_t>(position));
    case marker::int64: return load<std::int64_t>(position);
    case marker::byte: return load<std::uint8_t>(position);
    default: return 0;
    }
}

/**
 * @brief A bounds-checked forward cursor over the document, latching the first failure.
 *
 * Once failed it stays failed, so a parse can run to its natural end without checking after
 * every step, and the recorded offset is the one where things first went wrong.
 */
struct cursor {
    const std::byte *origin = nullptr;
    const std::byte *position = nullptr;
    const std::byte *limit = nullptr;

    errc failure = errc::ok;
    std::size_t failure_offset = 0;

    constexpr cursor() = default;
    constexpr cursor(const std::byte *origin, const std::byte *position, const std::byte *limit) noexcept
    : origin(origin)
    , position(position)
    , limit(limit) {}
    constexpr cursor(std::span<const std::byte> buffer, const std::byte *position) noexcept
    : origin(buffer.data())
    , position(position)
    , limit(buffer.data() + buffer.size()) {}

    [[nodiscard]] constexpr bool ok() const noexcept { return this->failure == errc::ok; }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return this->position < this->limit ? static_cast<std::size_t>(this->limit - this->position) : 0;
    }
    [[nodiscard]] constexpr bool available(std::uint64_t bytes) const noexcept {
        return this->ok() && bytes <= this->remaining();
    }
    [[nodiscard]] constexpr std::size_t offset_of(const std::byte *at) const noexcept {
        return static_cast<std::size_t>(at - this->origin);
    }

    constexpr void fail(errc code) noexcept { this->fail(code, this->position); }
    constexpr void fail(errc code, const std::byte *at) noexcept {
        if (!this->ok()) return;
        this->failure = code;
        this->failure_offset = this->offset_of(at);
    }

    /** Fails with unexpected_end when fewer than the requested bytes remain. */
    [[nodiscard]] constexpr bool need(std::uint64_t bytes) noexcept {
        if (!this->ok()) return false;
        if (bytes > this->remaining()) {
            this->fail(errc::unexpected_end);
            return false;
        }
        return true;
    }

    [[nodiscard]] constexpr std::byte peek() const noexcept { return *this->position; }
    [[nodiscard]] constexpr marker peek_marker() const noexcept { return to_marker(*this->position); }
    constexpr void advance(std::uint64_t bytes) noexcept { this->position += bytes; }

    [[nodiscard]] constexpr error to_error() const noexcept { return error { this->failure, this->failure_offset }; }
};

/** Reads a <marker><integer> pair, failing with marker_error if the marker is not integral. */
[[nodiscard]] inline std::int64_t read_marked_integer(cursor &source, errc marker_error) noexcept {
    const auto *at = source.position;
    if (!source.need(1)) return 0;

    const auto kind = to_marker(source.peek());
    if (!is_integer(kind)) {
        source.fail(marker_error, at);
        return 0;
    }
    source.advance(1);

    const auto width = payload_width(kind);
    if (!source.need(width)) return 0;

    const auto value = load_integer(kind, source.position);
    source.advance(width);
    return value;
}

/** Reads a length or count prefix. Negative values are a format error, not a wrap. */
[[nodiscard]] inline std::uint64_t read_length(cursor &source) noexcept {
    const auto *at = source.position;
    const auto value = read_marked_integer(source, errc::invalid_length);
    if (!source.ok()) return 0;
    if (value < 0) {
        source.fail(errc::negative_length, at);
        return 0;
    }
    return static_cast<std::uint64_t>(value);
}

/** Skips a bare object key: a length prefix with no S marker, then that many UTF-8 bytes. */
inline void skip_key(cursor &source) noexcept {
    const auto length = read_length(source);
    if (!source.need(length)) return;
    source.advance(length);
}

[[nodiscard]] inline std::string_view read_key(cursor &source) noexcept {
    const auto length = read_length(source);
    if (!source.need(length)) return {};
    const auto *begin = source.position;
    source.advance(length);
    return std::string_view { reinterpret_cast<const char *>(begin), static_cast<std::size_t>(length) };
}

struct counted_shape {
    std::uint64_t total = 0;
    std::uint32_t extents[max_dimensions] {};
    std::size_t rank = 0;
    bool column_major = false;
};

inline void read_dimension_list(cursor &source, counted_shape &shape) noexcept {
    const auto push = [&](std::int64_t value, const std::byte *at) {
        if (!source.ok()) return;
        if (value < 0) {
            source.fail(errc::invalid_dimensions, at);
            return;
        }
        if (shape.rank >= max_dimensions) {
            source.fail(errc::dimension_overflow, at);
            return;
        }
        shape.extents[shape.rank++] = static_cast<std::uint32_t>(value);
    };

    if (!source.need(1)) return;

    if (source.peek_marker() == marker::strong_type) {
        source.advance(1);
        const auto *at = source.position;
        if (!source.need(1)) return;

        const auto element = to_marker(source.peek());
        if (!is_integer(element)) {
            source.fail(errc::invalid_dimensions, at);
            return;
        }
        source.advance(1);

        if (!source.need(1)) return;
        if (source.peek_marker() != marker::count) {
            source.fail(errc::missing_count, source.position);
            return;
        }
        source.advance(1);

        const auto entries = read_length(source);
        const auto width = payload_width(element);
        for (std::uint64_t index = 0; index < entries && source.ok(); ++index) {
            const auto *entry = source.position;
            if (!source.need(width)) return;
            push(load_integer(element, source.position), entry);
            source.advance(width);
        }
    } else if (source.peek_marker() == marker::count) {
        source.advance(1);
        const auto entries = read_length(source);
        for (std::uint64_t index = 0; index < entries && source.ok(); ++index) {
            const auto *entry = source.position;
            push(read_marked_integer(source, errc::invalid_dimensions), entry);
        }
    } else {
        while (source.ok()) {
            if (!source.need(1)) return;
            if (source.peek_marker() == marker::array_end) {
                source.advance(1);
                break;
            }
            const auto *entry = source.position;
            push(read_marked_integer(source, errc::invalid_dimensions), entry);
        }
    }

    if (source.ok() && shape.rank == 0) source.fail(errc::invalid_dimensions);
}

/** Reads the value after '#': either a plain integer count or a dimension array. */
[[nodiscard]] inline counted_shape read_count(cursor &source) noexcept {
    counted_shape shape;
    if (!source.need(1)) return shape;

    if (source.peek_marker() != marker::array_begin) {
        shape.total = read_length(source);
        return shape;
    }
    source.advance(1);

    const auto *outer = source.position;
    if (source.available(1) && source.peek_marker() == marker::array_begin) {
        source.advance(1);
        shape.column_major = true;
    }

    read_dimension_list(source, shape);

    if (shape.column_major) {
        if (!source.need(1)) return shape;
        if (source.peek_marker() != marker::array_end) {
            source.fail(errc::invalid_dimensions, outer);
            return shape;
        }
        source.advance(1);
    }

    if (!source.ok()) return shape;

    shape.total = 1;
    for (std::size_t index = 0; index < shape.rank; ++index) {
        const auto extent = shape.extents[index];
        if (extent != 0 && shape.total > std::numeric_limits<std::uint64_t>::max() / extent) {
            source.fail(errc::dimension_overflow);
            return shape;
        }
        shape.total *= extent;
    }
    return shape;
}

/** A parsed container header: everything between the opening brace and the first element. */
/** What every container has. An object has only this: it cannot carry dimensions. */
struct container_prefix {
    marker element = marker::invalid; ///< strong type, or invalid when heterogeneous
    std::uint64_t count = 0;
    bool unbounded = false;
    const std::byte *body = nullptr;

    [[nodiscard]] constexpr bool typed() const noexcept { return this->element != marker::invalid; }
};

/** That, plus the shape an array may declare. */
struct header : container_prefix {
    std::uint32_t extents[max_dimensions] {};
    std::size_t rank = 0;
    bool column_major = false;
};

/**
 * Reads a container's header into whichever of the two shapes the caller asked for.
 *
 * Reading an object into the prefix alone is worth having: the full header carries an array of
 * extents that is zeroed on construction and copied on return, and an object never has any.
 */
template<typename Result>
[[nodiscard]] inline Result parse_header_as(cursor &source, bool object) noexcept {
    Result result;

    if (!source.need(1)) return result;

    const auto lead = source.peek_marker();
    if (lead == marker::strong_type) {
        source.advance(1);

        const auto *at = source.position;
        if (!source.need(1)) return result;
        result.element = to_marker(source.peek());
        if (!is_strong_type(result.element)) {
            source.fail(errc::invalid_strong_type, at);
            return result;
        }
        source.advance(1);

        if (!source.need(1)) return result;
        if (source.peek_marker() != marker::count) {
            source.fail(errc::missing_count, source.position);
            return result;
        }
        source.advance(1);
    } else if (lead == marker::count) {
        source.advance(1);
    } else {
        result.unbounded = true;
        result.body = source.position;
        return result;
    }

    const auto *at = source.position;
    const auto shape = read_count(source);
    if (!source.ok()) return result;

    if (object && shape.rank != 0) {
        source.fail(errc::object_dimension_count, at);
        return result;
    }

    result.count = shape.total;
    if constexpr (requires { result.rank; }) {
        result.rank = shape.rank;
        result.column_major = shape.column_major;
        for (std::size_t index = 0; index < shape.rank; ++index)
            result.extents[index] = shape.extents[index];
    }

    result.body = source.position;
    return result;
}

[[nodiscard]] inline header parse_header(cursor &source, bool object) noexcept {
    return parse_header_as<header>(source, object);
}

/** For an object, where the shape fields would only be zeroed and copied for nothing. */
[[nodiscard]] inline container_prefix parse_object_prefix(cursor &source) noexcept {
    return parse_header_as<container_prefix>(source, true);
}

void skip_value(cursor &source, marker kind, int depth) noexcept;

inline void skip_container(cursor &source, bool object, int depth) noexcept {
    const auto info = parse_header(source, object);
    if (!source.ok()) return;

    const auto skip_noops = [&] {
        while (source.available(1) && source.peek_marker() == marker::noop)
            source.advance(1);
    };

    const auto skip_element = [&] {
        if (!source.need(1)) {
            source.fail(errc::unterminated_container);
            return;
        }
        const auto *at = source.position;
        const auto kind = to_marker(source.peek());
        if (kind == marker::extension) {
            source.fail(errc::extension_unsupported, at);
            return;
        }
        if (!is_value(kind)) {
            source.fail(errc::unexpected_marker, at);
            return;
        }
        source.advance(1);
        skip_value(source, kind, depth + 1);
    };

    if (info.unbounded) {
        const auto terminator = object ? marker::object_end : marker::array_end;
        while (source.ok()) {
            skip_noops();
            if (!source.need(1)) {
                source.fail(errc::unterminated_container);
                return;
            }
            if (source.peek_marker() == terminator) {
                source.advance(1);
                return;
            }
            if (object) {
                skip_key(source);
                if (!source.ok()) return;
            }
            skip_element();
        }
        return;
    }

    if (info.typed() && !object) {
        const auto width = payload_width(info.element);
        if (width != 0 && info.count > std::numeric_limits<std::uint64_t>::max() / width) {
            source.fail(errc::dimension_overflow);
            return;
        }
        const auto total = info.count * width;
        if (!source.need(total)) return;
        source.advance(total);
        return;
    }

    for (std::uint64_t index = 0; index < info.count && source.ok(); ++index) {
        skip_noops();
        if (object) {
            skip_key(source);
            if (!source.ok()) return;
            if (info.typed()) {
                const auto width = payload_width(info.element);
                if (!source.need(width)) return;
                source.advance(width);
                continue;
            }
        }
        skip_element();
    }
}

inline void skip_value(cursor &source, marker kind, int depth) noexcept {
    if (depth > max_depth) {
        source.fail(errc::depth_exceeded);
        return;
    }

    const auto width = payload_width(kind);
    if (width != variable_width) {
        if (source.need(width)) source.advance(width);
        return;
    }

    switch (kind) {
    case marker::string:
    case marker::high_precision: {
        const auto length = read_length(source);
        if (!source.need(length)) return;
        source.advance(length);
        return;
    }
    case marker::array_begin: skip_container(source, false, depth); return;
    case marker::object_begin: skip_container(source, true, depth); return;
    case marker::extension: source.fail(errc::extension_unsupported); return;
    default: source.fail(errc::unexpected_marker); return;
    }
}

} // namespace detail

} // namespace serpent::bjdata
