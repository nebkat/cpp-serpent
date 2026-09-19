# Licence

MIT License

Copyright (c) 2026 Nebojša Cvetković

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Code by other people

`include/serpent/external/` holds copies of three libraries, each under its own licence and
each kept beside the licence it came with. All three are permissive and compatible with the
MIT terms above; none of them places any further condition on a program that uses serpent
beyond preserving the notices below.

| Directory | Project | Copyright | Licence |
|---|---|---|---|
| `zmij/` | [Żmij](https://github.com/vitaut/zmij) | Victor Zverovich | MIT — `include/serpent/external/zmij/LICENSE` |
| `glaze/` | [Glaze](https://github.com/stephenberry/glaze) | Stephen Berry | MIT — `include/serpent/external/glaze/LICENSE` |
| `fast_float/` | [fast_float](https://github.com/fastfloat/fast_float) | Daniel Lemire, João Paulo Magalhaes and contributors | Apache-2.0 **or** MIT **or** BSL-1.0, at your choice — `include/serpent/external/fast_float/LICENSE-*` |

`glaze/itoa_40kb.hpp` is itself derived work: it builds on ibireme's `itoa_yy.c` and on
RealTimeChris's Jsonifier, as the notice at the top of that file records. That notice travels
with the file and is not repeated here.

Each copy is taken from a pinned upstream commit by a script in `tools/`, which also records the
only changes made: a namespace and a macro prefix, so that a copy here cannot collide with
another copy of the same library in the same program. None is edited by hand.
