#!/bin/sh
# Builds every configuration of the benchmark and runs them all, then prints them side by side.
#
#   tools/bench.sh                     everything
#   tools/bench.sh --filter JSON       only measurements whose group or library contains JSON
#   tools/bench.sh --quick             fewer samples: checks that it runs, not numbers to quote
#
# CXX chooses the compiler; the reflected path needs GCC 16, which is what is looked for first.
# Both libraries are always built by the same compiler in the same process - one of them gains a
# good deal more from GCC 16 than the other, so numbers from two compilers do not compare.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${CXX:-}" ]; then
    CXX=$(command -v g++-16 || command -v c++)
fi
# One build directory per compiler, since CMake will not change compilers in one.
build="${BUILD_DIR:-$root/build-bench-$(basename "$CXX")}"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX" \
    -DSERPENT_BUILD_BENCHMARKS=ON -DSERPENT_TEST_SANITIZE=OFF > /dev/null
cmake --build "$build" -j --target serpent_benchmarks

rm -f "$build"/results/*.json
for program in "$build"/benchmark/serpent_bench_*; do
    [ -x "$program" ] || continue
    configuration=${program##*/serpent_bench_}
    "$program" --json "$build/results/$configuration.json" "$@"
done

echo
echo "================ every configuration, side by side ================"
python3 "$root/tools/bench-summary.py" "$build/results"
