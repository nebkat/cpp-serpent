#!/bin/sh
# Refreshes the pieces of Glaze under include/serpent/external/glaze from a pinned upstream commit.
#
#   tools/vendor-glaze.sh                # the commit pinned below
#   tools/vendor-glaze.sh <commit>       # another one; update PINNED to match if it is kept
#
# What is taken is its integer formatting, in the two sizes it comes in - itoa.hpp, with 400 bytes
# of tables, and itoa_40kb.hpp, whose name is its table - and the inlining macro both use. They are
# Stephen Berry's, under the MIT licence that is copied beside them; itoa_40kb.hpp credits in its
# own header the work it builds on in turn - ibireme's itoa_yy.c and RealTimeChris's Jsonifier -
# and that header is kept as it is.
#
# The copy differs from upstream in three mechanical ways and no others, all so that it cannot
# collide with Glaze itself in a program that uses both:
#
#   namespace glz               ->  namespace serpent::external::glaze
#   GLZ_...  macros             ->  SERPENT_GLZ_...
#   #include "glaze/util/..."   ->  #include <serpent/external/glaze/...>
set -eu

PINNED=142d9ab6fa9272d779142055b9be482229f94bcb
COMMIT=${1:-$PINNED}
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/include/serpent/external/glaze"
BASE="https://raw.githubusercontent.com/stephenberry/glaze/$COMMIT"

mkdir -p "$DEST"
curl -fsSL "$BASE/LICENSE" -o "$DEST/LICENSE"
for FILE in inline.hpp itoa.hpp itoa_40kb.hpp; do
    {
        echo "// Vendored from https://github.com/stephenberry/glaze at $COMMIT"
        echo "// (include/glaze/util/$FILE) by tools/vendor-glaze.sh. Do not edit: change the script and run"
        echo "// it again. What differs from upstream, and why, is described there. The licence is beside"
        echo "// this file."
        echo "//"
        curl -fsSL "$BASE/include/glaze/util/$FILE" | sed \
            -e 's/^namespace glz$/namespace serpent::external::glaze/' \
            -e 's|^} // namespace glz$|} // namespace serpent::external::glaze|' \
            -e 's/\([^A-Za-z0-9_:]\)glz::/\1serpent::external::glaze::/g' \
            -e 's|#include "glaze/util/\([a-z_0-9]*\.hpp\)"|#include <serpent/external/glaze/\1>|' \
            -e 's/GLZ_/SERPENT_GLZ_/g'
    } > "$DEST/$FILE"
done

echo "vendored Glaze's integer formatting at $COMMIT"
