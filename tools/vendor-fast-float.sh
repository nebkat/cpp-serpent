#!/bin/sh
# Refreshes the copy of fast_float under include/serpent/external/fast_float from a pinned
# upstream release.
#
#   tools/vendor-fast-float.sh               # the release pinned below
#   tools/vendor-fast-float.sh <tag>         # another one; update PINNED to match if it is kept
#
# What is taken is the single header upstream publishes with each release, which names its
# authors and carries its licence notice at the top, and the three licence files it is offered
# under - Apache 2.0, MIT or Boost, at the user's choice.
#
# The copy differs from upstream in three mechanical ways and no others, all so that it cannot
# collide with another copy of fast_float in the same program - one a consumer brought, or one
# inside another library:
#
#   namespace fast_float    ->  namespace serpent::external::fast_float
#   fast_float::name        ->  serpent::external::fast_float::name
#   FASTFLOAT_...  macros   ->  SERPENT_FASTFLOAT_...
set -eu

PINNED=v8.3.0
TAG=${1:-$PINNED}
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/include/serpent/external/fast_float"

mkdir -p "$DEST"
for LICENCE in LICENSE-APACHE LICENSE-BOOST LICENSE-MIT; do
    curl -fsSL "https://raw.githubusercontent.com/fastfloat/fast_float/$TAG/$LICENCE" -o "$DEST/$LICENCE"
done
{
    echo "// Vendored from https://github.com/fastfloat/fast_float release $TAG by"
    echo "// tools/vendor-fast-float.sh. Do not edit: change the script and run it again. What differs"
    echo "// from upstream, and why, is described there. The licences are beside this file."
    echo "//"
    curl -fsSL "https://github.com/fastfloat/fast_float/releases/download/$TAG/fast_float.h" | sed \
        -e 's/^namespace fast_float {$/namespace serpent::external::fast_float {/' \
        -e 's|^} // namespace fast_float$|} // namespace serpent::external::fast_float|' \
        -e 's/\([^A-Za-z0-9_:]\)fast_float::/\1serpent::external::fast_float::/g' \
        -e 's/FASTFLOAT_/SERPENT_FASTFLOAT_/g'
} > "$DEST/fast_float.h"

echo "vendored fast_float $TAG"
