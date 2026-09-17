#!/usr/bin/env python3
"""Lays the results of differently configured benchmark builds side by side.

    tools/bench-summary.py <results dir> [--markdown]

Each file in the directory is what one `serpent_bench_<configuration> --json` wrote. Every
library that is not serpent is the same code in every build, so it is shown once, from the
first configuration; serpent is shown once per configuration. Times are microseconds, and the
figure in brackets is how many times the fastest row of the group each one took.
"""

import json
import pathlib
import sys


def load(directory: pathlib.Path) -> dict[str, list[dict]]:
    runs = {}
    for path in sorted(directory.glob("*.json")):
        document = json.loads(path.read_text())
        runs[document["configuration"]] = document["results"]
    # "default" first, since it is the one the others are read against.
    return dict(sorted(runs.items(), key=lambda item: (item[0] != "default", item[0])))


def rows(runs: dict[str, list[dict]]) -> dict[str, dict[str, float]]:
    """group -> row label -> nanoseconds: serpent in each configuration, then everyone else."""
    ours: dict[str, dict[str, float]] = {}
    theirs: dict[str, dict[str, float]] = {}
    first = next(iter(runs))
    for configuration, results in runs.items():
        for entry in results:
            if entry["library"].startswith("serpent"):
                label = f"{entry['library']} [{configuration}]" if len(runs) > 1 else entry["library"]
                ours.setdefault(entry["group"], {})[label] = entry["ns"]
            elif configuration == first:
                theirs.setdefault(entry["group"], {})[entry["library"]] = entry["ns"]
    return {group: entries | theirs.get(group, {}) for group, entries in ours.items()}


def main() -> int:
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith("--")]
    markdown = "--markdown" in sys.argv
    if len(arguments) != 1:
        print(__doc__, file=sys.stderr)
        return 2

    runs = load(pathlib.Path(arguments[0]))
    if not runs:
        print("no results there; run the benchmarks with --json first", file=sys.stderr)
        return 1

    for group, entries in rows(runs).items():
        fastest = min(entries.values())
        if markdown:
            print(f"\n**{group}**\n\n| | µs | × fastest |\n|---|---:|---:|")
            for label, ns in entries.items():
                print(f"| {label} | {ns / 1000:,.1f} | {ns / fastest:.2f} |")
        else:
            print(f"\n{group}")
            for label, ns in entries.items():
                print(f"  {label:<32} {ns / 1000:>12,.1f} us   {ns / fastest:>6.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
