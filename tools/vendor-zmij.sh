#!/bin/sh
# Refreshes the copy of Żmij under include/serpent/external/zmij from a pinned upstream commit.
#
#   tools/vendor-zmij.sh                 # the commit pinned below
#   tools/vendor-zmij.sh <commit>        # another one; update PINNED to match if it is kept
#
# Two files are taken, as upstream ships them: zmij.h, and zmij.cc, which is built once into a
# small library rather than ported into a header. Included as text it costs every translation
# unit about half a second; compiled, it costs that once.
#
# The copy differs from upstream in four mechanical ways and no others, all so that it cannot
# collide with another copy of Żmij in the same program - one a consumer brought, or one inside
# another library:
#
#   namespace zmij      ->  namespace serpent::external::zmij
#   zmij::name          ->  serpent::external::zmij::name
#   ZMIJ_...  macros    ->  SERPENT_ZMIJ_...
#   the include guard   ->  follows from the macro rename
set -eu

PINNED=a24bf7fce0f4fdee60c0153529b74bb537552b7b
COMMIT=${1:-$PINNED}
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/include/serpent/external/zmij"
BASE="https://raw.githubusercontent.com/vitaut/zmij/$COMMIT"

mkdir -p "$DEST"
curl -fsSL "$BASE/LICENSE" -o "$DEST/LICENSE"
for FILE in zmij.h zmij.cc; do
    {
        echo "// Vendored from https://github.com/vitaut/zmij at $COMMIT by tools/vendor-zmij.sh."
        echo "// Do not edit: change the script and run it again. What differs from upstream, and why, is"
        echo "// described there. The licence is beside this file."
        echo "//"
        curl -fsSL "$BASE/$FILE" | sed \
            -e 's/^namespace zmij {$/namespace serpent::external::zmij {/' \
            -e 's|^}  // namespace zmij$|}  // namespace serpent::external::zmij|' \
            -e 's/\([^A-Za-z0-9_:]\)zmij::/\1serpent::external::zmij::/g' \
            -e 's/ZMIJ_/SERPENT_ZMIJ_/g'
    } > "$DEST/$FILE"
done

echo "vendored zmij.h and zmij.cc at $COMMIT"
