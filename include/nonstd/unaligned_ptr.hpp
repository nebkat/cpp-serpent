#pragma once

#include <nonstd/unaligned.hpp>

#include <algorithm>
#include <compare>
#include <concepts>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>

#include <cstddef>

namespace nonstd {

/**
 * @brief A reference to a value of type T stored unaligned at a raw byte address.
 *
 * There is no object of type T at the address, so this never forms a T& - it loads and
 * stores by value through unaligned<T, Bits, E>. Assignment is const-qualified so that the
 * prvalue returned by unaligned_ptr::operator* can be written through.
 */
template<typename T, std::size_t Bits = sizeof(T) * 8, std::endian E = std::endian::native>
class unaligned_ref {
    static_assert(!std::is_const_v<T>, "unaligned_ref must not be cv-qualified; use a plain T for a const view");

    std::byte *pointer = nullptr;

public:
    using storage_type = unaligned<T, Bits, E>;
    using value_type = T;

    static constexpr std::size_t stride = storage_type::storage_bytes;

    constexpr explicit unaligned_ref(std::byte *pointer) noexcept : pointer(pointer) {}

    [[nodiscard]] constexpr T value() const noexcept {
        typename storage_type::storage_type storage {};
        std::copy_n(this->pointer, stride, storage.begin());
        return storage_type::from_storage(storage);
    }

    [[nodiscard]] constexpr operator T() const noexcept { return this->value(); }

    constexpr const unaligned_ref &operator=(T value) const noexcept {
        const auto storage = storage_type::to_storage(value);
        std::copy_n(storage.begin(), stride, this->pointer);
        return *this;
    }

    constexpr const unaligned_ref &operator=(const unaligned_ref &other) const noexcept {
        return *this = other.value();
    }

    [[nodiscard]] constexpr std::byte *data() const noexcept { return this->pointer; }

    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator+=(U y) const noexcept {
        return *this = static_cast<T>(this->value() + static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator-=(U y) const noexcept {
        return *this = static_cast<T>(this->value() - static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator*=(U y) const noexcept {
        return *this = static_cast<T>(this->value() * static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator/=(U y) const noexcept {
        return *this = static_cast<T>(this->value() / static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator%=(U y) const noexcept {
        return *this = static_cast<T>(this->value() % static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator&=(U y) const noexcept {
        return *this = static_cast<T>(this->value() & static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator|=(U y) const noexcept {
        return *this = static_cast<T>(this->value() | static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator^=(U y) const noexcept {
        return *this = static_cast<T>(this->value() ^ static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator<<=(U y) const noexcept {
        return *this = static_cast<T>(this->value() << static_cast<T>(y));
    }
    template<class U>
        requires std::convertible_to<U, T>
    constexpr const unaligned_ref &operator>>=(U y) const noexcept {
        return *this = static_cast<T>(this->value() >> static_cast<T>(y));
    }

    constexpr const unaligned_ref &operator++() const noexcept { return *this += static_cast<T>(1); }
    constexpr const unaligned_ref &operator--() const noexcept { return *this -= static_cast<T>(1); }
    [[nodiscard]] constexpr T operator++(int) const noexcept {
        const T old = this->value();
        ++(*this);
        return old;
    }
    [[nodiscard]] constexpr T operator--(int) const noexcept {
        const T old = this->value();
        --(*this);
        return old;
    }

    friend constexpr void swap(const unaligned_ref &a, const unaligned_ref &b) noexcept {
        const T temporary = a.value();
        a = b.value();
        b = temporary;
    }
};

/**
 * @brief A random access iterator over values of type T packed unaligned in a byte buffer.
 *
 * Constness is carried in T, mirroring T* and const T*: unaligned_ptr<const std::uint16_t>
 * dereferences to a prvalue, unaligned_ptr<std::uint16_t> to an assignable unaligned_ref.
 */
template<typename T, std::size_t Bits = sizeof(T) * 8, std::endian E = std::endian::native>
class unaligned_ptr {
public:
    using value_type = std::remove_const_t<T>;
    using storage_type = unaligned<value_type, Bits, E>;
    using byte_type = std::conditional_t<std::is_const_v<T>, const std::byte, std::byte>;
    using reference = std::conditional_t<std::is_const_v<T>, value_type, unaligned_ref<value_type, Bits, E>>;

    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::random_access_iterator_tag;
    using iterator_category = std::random_access_iterator_tag;

    static constexpr std::size_t stride = storage_type::storage_bytes;

private:
    byte_type *pointer = nullptr;

    struct arrow_proxy {
        value_type held;
        [[nodiscard]] constexpr const value_type *operator->() const noexcept { return std::addressof(this->held); }
    };

public:
    constexpr unaligned_ptr() = default;
    constexpr explicit unaligned_ptr(byte_type *pointer) noexcept : pointer(pointer) {}

    /**
     * Converting constructor from the mutable to the const form, mirroring T* -> const T*.
     * A constructor template so that it is never itself the copy constructor, which would
     * suppress the implicit one for a non-const T.
     */
    template<typename U>
        requires (std::is_const_v<T> && std::same_as<U, value_type>)
    constexpr unaligned_ptr(const unaligned_ptr<U, Bits, E> &other) noexcept : pointer(other.data()) {}

    [[nodiscard]] constexpr byte_type *data() const noexcept { return this->pointer; }

    [[nodiscard]] constexpr reference operator*() const noexcept {
        if constexpr (std::is_const_v<T>) {
            typename storage_type::storage_type storage {};
            std::copy_n(this->pointer, stride, storage.begin());
            return storage_type::from_storage(storage);
        } else {
            return reference { this->pointer };
        }
    }

    [[nodiscard]] constexpr reference operator[](difference_type index) const noexcept { return *(*this + index); }

    [[nodiscard]] constexpr arrow_proxy operator->() const noexcept { return arrow_proxy { **this }; }

    constexpr unaligned_ptr &operator++() noexcept {
        this->pointer += stride;
        return *this;
    }
    constexpr unaligned_ptr &operator--() noexcept {
        this->pointer -= stride;
        return *this;
    }
    constexpr unaligned_ptr operator++(int) noexcept {
        auto old = *this;
        ++(*this);
        return old;
    }
    constexpr unaligned_ptr operator--(int) noexcept {
        auto old = *this;
        --(*this);
        return old;
    }

    constexpr unaligned_ptr &operator+=(difference_type n) noexcept {
        this->pointer += n * static_cast<difference_type>(stride);
        return *this;
    }
    constexpr unaligned_ptr &operator-=(difference_type n) noexcept {
        this->pointer -= n * static_cast<difference_type>(stride);
        return *this;
    }

    [[nodiscard]] friend constexpr unaligned_ptr operator+(unaligned_ptr p, difference_type n) noexcept {
        return p += n;
    }
    [[nodiscard]] friend constexpr unaligned_ptr operator+(difference_type n, unaligned_ptr p) noexcept {
        return p += n;
    }
    [[nodiscard]] friend constexpr unaligned_ptr operator-(unaligned_ptr p, difference_type n) noexcept {
        return p -= n;
    }
    [[nodiscard]] friend constexpr difference_type operator-(const unaligned_ptr &a, const unaligned_ptr &b) noexcept {
        return (a.pointer - b.pointer) / static_cast<difference_type>(stride);
    }

    [[nodiscard]] friend constexpr bool operator==(const unaligned_ptr &a, const unaligned_ptr &b) noexcept {
        return a.pointer == b.pointer;
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(
            const unaligned_ptr &a, const unaligned_ptr &b) noexcept {
        return a.pointer <=> b.pointer;
    }

    [[nodiscard]] friend constexpr value_type iter_move(const unaligned_ptr &p) noexcept { return *p; }

    friend constexpr void iter_swap(const unaligned_ptr &a, const unaligned_ptr &b) noexcept
        requires (!std::is_const_v<T>)
    {
        const value_type temporary = *a;
        *a = static_cast<value_type>(*b);
        *b = temporary;
    }
};

/**
 * @brief A span of values of type T packed unaligned in a byte buffer.
 *
 * Not a contiguous_range - the elements exist only as byte patterns - so size(), empty(),
 * front(), back() and operator[] come from view_interface over the iterator pair.
 */
template<typename T, std::size_t Bits = sizeof(T) * 8, std::endian E = std::endian::native>
class unaligned_span : public std::ranges::view_interface<unaligned_span<T, Bits, E>> {
public:
    using pointer = unaligned_ptr<T, Bits, E>;
    using value_type = typename pointer::value_type;
    using byte_type = typename pointer::byte_type;
    using reference = typename pointer::reference;

    static constexpr std::size_t stride = pointer::stride;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    pointer origin {};
    std::size_t count = 0;

public:
    constexpr unaligned_span() = default;
    constexpr unaligned_span(pointer origin, std::size_t count) noexcept : origin(origin), count(count) {}
    constexpr unaligned_span(byte_type *data, std::size_t count) noexcept : origin(data), count(count) {}

    template<typename U>
        requires (std::is_const_v<T> && std::same_as<U, value_type>)
    constexpr unaligned_span(const unaligned_span<U, Bits, E> &other) noexcept
    : origin(other.begin())
    , count(other.size()) {}

    [[nodiscard]] constexpr pointer begin() const noexcept { return this->origin; }
    [[nodiscard]] constexpr pointer end() const noexcept {
        return this->origin + static_cast<std::ptrdiff_t>(this->count);
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return this->count; }
    [[nodiscard]] constexpr std::size_t size_bytes() const noexcept { return this->count * stride; }

    [[nodiscard]] constexpr std::span<byte_type> bytes() const noexcept {
        return std::span<byte_type> { this->origin.data(), this->size_bytes() };
    }

    [[nodiscard]] constexpr unaligned_span subspan(std::size_t offset, std::size_t length = npos) const noexcept {
        const auto clamped_offset = std::min(offset, this->count);
        const auto remaining = this->count - clamped_offset;
        return unaligned_span { this->origin + static_cast<std::ptrdiff_t>(clamped_offset),
            std::min(length, remaining) };
    }

    [[nodiscard]] constexpr unaligned_span first(std::size_t length) const noexcept { return this->subspan(0, length); }
    [[nodiscard]] constexpr unaligned_span last(std::size_t length) const noexcept {
        return this->subspan(this->count - std::min(length, this->count));
    }

    /** Truncates to whole elements; trailing bytes that cannot form one are ignored. */
    [[nodiscard]] static constexpr unaligned_span decode_TMP(std::span<byte_type> bytes) noexcept {
        return unaligned_span { bytes.data(), bytes.size() / stride };
    }

    /** Requires an exact multiple of the element width. */
    [[nodiscard]] static constexpr std::optional<unaligned_span> try_from_bytes(std::span<byte_type> bytes) noexcept {
        if (bytes.size() % stride != 0) return std::nullopt;
        return unaligned_span { bytes.data(), bytes.size() / stride };
    }
};

template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_little_ptr = unaligned_ptr<T, Bits, std::endian::little>;
template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_little_ref = unaligned_ref<T, Bits, std::endian::little>;
template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_little_span = unaligned_span<T, Bits, std::endian::little>;

template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_big_ptr = unaligned_ptr<T, Bits, std::endian::big>;
template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_big_ref = unaligned_ref<T, Bits, std::endian::big>;
template<typename T, std::size_t Bits = sizeof(T) * 8>
using unaligned_big_span = unaligned_span<T, Bits, std::endian::big>;

} // namespace nonstd
