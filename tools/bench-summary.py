#!/usr/bin/env python3
"""Lays the results of differently configured benchmark builds side by side.

    tools/bench-summary.py <results dir> [--markdown]

Each file in the directory is what one `serpent_bench_<configuration> --json` wrote. One table per
workload: a row per library, a column per operation. Every library that is not serpent is the same
code in every build, so it is shown once; serpent is shown once per configuration. Times are
microseconds; the figure beside each is how many times the first row's it is.
"""

import json
import pathlib
import sys

ROW_ORDER = ["serpent", "serpent (for size)", "serpent (indexed)", "glaze", "glaze (CBOR)", "glaze (size build)",
             "simdjson", "yyjson", "rapidjson", "nlohmann"]


def load(directory: pathlib.Path) -> dict[str, list[dict]]:
    runs = {}
    for path in sorted(directory.glob("*.json")):
        document = json.loads(path.read_text())
        runs[document["configuration"]] = document["results"]
    return dict(sorted(runs.items(), key=lambda item: (item[0] != "default", item[0])))


def tables(runs: dict[str, list[dict]]):
    """workload -> (columns, {row label: {operation: ns}})"""
    out: dict[str, tuple[list[str], dict[str, dict[str, float]]]] = {}
    first = next(iter(runs))
    for configuration, results in runs.items():
        for entry in results:
            ours = entry["library"].startswith("serpent")
            if not ours and configuration != first:
                continue
            label = f"{entry['library']} [{configuration}]" if ours and len(runs) > 1 else entry["library"]
            columns, rows = out.setdefault(entry["workload"], ([], {}))
            if entry["operation"] not in columns:
                columns.append(entry["operation"])
            rows.setdefault(label, {})[entry["operation"]] = entry["ns"]
    return out


def base_name(label: str) -> str:
    return label.split(" [")[0]


def row_key(label: str):
    base = base_name(label)
    return (ROW_ORDER.index(base) if base in ROW_ORDER else len(ROW_ORDER), label)


def cell(ns: float) -> str:
    us = ns / 1000
    return f"{us:.2f}" if us < 10 else f"{us:,.0f}"


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

    for workload, (columns, rows) in tables(runs).items():
        labels = sorted(rows, key=row_key)
        first = rows[labels[0]]
        if markdown:
            print(f"\n**{workload}** (µs, and times the first row)\n")
            print("| | " + " | ".join(columns) + " |")
            print("|---|" + "---:|" * len(columns))
        else:
            print(f"\n{workload}  (us, and times the first row)")
            print("  " + " " * 30 + "".join(f" {column:>19}" for column in columns))
        for label in labels:
            cells = []
            for column in columns:
                ns = rows[label].get(column)
                if ns is None:
                    cells.append("-")
                elif label == labels[0] or first.get(column) is None:
                    cells.append(cell(ns))
                else:
                    ratio = ns / first[column]
                    cells.append(f"{cell(ns)} ({'>999' if ratio >= 1000 else f'{ratio:.2f}'}x)")
            if markdown:
                print(f"| {label} | " + " | ".join(cells) + " |")
            else:
                print(f"  {label:<30}" + "".join(f" {text:>19}" for text in cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
