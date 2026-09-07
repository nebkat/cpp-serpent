# unaligned

Reading and writing values that are not aligned for their type.

There is no `T` at an address inside a byte buffer, only bytes that mean one. `unaligned_ptr`
never forms a `T *` or a `T &`: it is a proxy pointer that loads and stores by value through
`unaligned<T, Bits, Endian>`.

```cpp
#include <nonstd/unaligned_ptr.hpp>

nonstd::unaligned_little_span<std::uint16_t> values { buffer + 1, 8 };   // misaligned, fine
std::iota(values.begin(), values.end(), 0);
std::ranges::sort(values);
```

It models `std::random_access_iterator` — C++20 iterator concepts permit proxy references
above `input_iterator` — so `<algorithm>` and `<ranges>` apply. Constness lives in `T`,
mirroring `T *` and `const T *`. Odd widths work too: `unaligned_little_ptr<std::int32_t, 24>`
reads a sign-extended 24-bit field, and endianness is a template parameter, so a
little-endian field is read correctly on a big-endian host.

`unaligned_span` derives from `std::ranges::view_interface`, which supplies `size()`,
`empty()`, `front()`, `back()` and `operator[]`, and makes
`std::ranges::to<std::vector<std::uint16_t>>(span)` work when a copy *is* wanted.

Header only, C++23. `cmake --build` and `ctest` run the tests.
