# Other people's code

Everything under this directory was written by someone else and is kept here, under its own
licence, so that the library needs nothing fetched to build. Each is copied from a pinned upstream
commit by a script in `tools/`, which is also where the few mechanical differences from upstream
are described - a namespace and a macro prefix, so that a copy here cannot collide with another
copy in the same program. None is edited by hand. Each is used only when a switch in
`serpent/config.hpp` asks for it.

| Directory | What | Author and licence | Copied by | Used when |
|---|---|---|---|---|
| `zmij/` | [Żmij](https://github.com/vitaut/zmij): the shortest text of a real | Victor Zverovich; MIT, `zmij/LICENSE` | `tools/vendor-zmij.sh` | `SERPENT_USE_ZMIJ` |
| `glaze/` | [Glaze](https://github.com/stephenberry/glaze)'s integer formatting, `itoa.hpp` and `itoa_40kb.hpp` | Stephen Berry; MIT, `glaze/LICENSE`. `itoa_40kb.hpp` builds on ibireme's `itoa_yy.c` and RealTimeChris's Jsonifier, as its header says | `tools/vendor-glaze.sh` | `SERPENT_INTEGER_TABLE` 1 or 2 |
| `fast_float/` | [fast_float](https://github.com/fastfloat/fast_float): text to a real, exactly rounded | Daniel Lemire, João Paulo Magalhaes and the contributors its header names; Apache 2.0, MIT or Boost at your choice, `fast_float/LICENSE-*` | `tools/vendor-fast-float.sh` | `SERPENT_USE_FAST_FLOAT` |
