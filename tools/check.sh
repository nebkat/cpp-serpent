#!/bin/sh
# Builds a configuration and runs its tests - and never runs a test whose build failed, since
# ctest would happily run the binary from before and call it a pass.
#
#   tools/check.sh <build dir> [ctest arguments...]
set -eu
build=$1
shift
cmake --build "$build" -j
ctest --test-dir "$build" --output-on-failure "$@"
