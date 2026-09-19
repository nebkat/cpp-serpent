#!/usr/bin/env python3
"""Rewrites the tables in docs/benchmarks.md from a benchmark run's results.

    tools/bench-docs.py <results dir>

Reads default.json and plain.json as `serpent_bench_<configuration> --json` wrote them. Only the
tables change; the prose around them is left as it is. Library names are the neutral ones the
page uses.
"""

import json
import pathlib
import re
import sys

NAMES = {"serpent": "serpent", "serpent (bounded)": "serpent, bounded", "serpent (for size)": "serpent, BJData for size",
         "glaze": "struct-mapping lib", "glaze (bounded)": "the same, bounded", "glaze (CBOR)": "the same, CBOR",
         "glaze (size build)": "the same, built for size", "nlohmann": "DOM lib"}
DOC_NAMES = {"serpent": "serpent", "serpent (indexed)": "serpent, over an index built first", "simdjson": "on-demand parser",
             "yyjson": "fast DOM A", "rapidjson": "fast DOM B", "nlohmann": "DOM lib"}
OPS = ["JSON encode", "JSON decode", "binary encode", "binary decode"]
KINDS = [("strings", "10k records of 5 strings"), ("booleans", "10k records of 5 booleans"),
         ("integers", "10k records of 5 integers"), ("reals", "10k records of 5 reals"),
         ("100 × 2000 numbers", "100 records of 2000 numbers")]
DOC_OPS = [("citm first key", "first key of citm"), ("citm last key", "last key of citm"), ("twitter sum ids", "sum ids, twitter"),
           ("citm count values", "count every value, citm"), ("canada sum coords", "sum coordinates, canada")]
RECORDS = "10k records"


def load(directory: pathlib.Path, configuration: str):
    results = json.loads((directory / f"{configuration}.json").read_text())["results"]
    return {(entry["workload"], entry["operation"], entry["library"]): entry["ns"] / 1000 for entry in results}


def cell(value: float, best: bool) -> str:
    text = f"{value:.2f}" if value < 10 else f"{value:,.0f}"
    return f"**{text}**" if best else text


def replace_table(page: str, header: str, body: str) -> str:
    start = page.index(header)
    end = page.index("\n\n", start)
    return page[:start] + header + "\n" + body.rstrip("\n") + page[end:]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    directory = pathlib.Path(sys.argv[1])
    default, plain = load(directory, "default"), load(directory, "plain")
    page_path = pathlib.Path(__file__).resolve().parent.parent / "docs" / "benchmarks.md"
    page = page_path.read_text()

    # Your own types: the mixed record, every library, every operation.
    rows = ""
    for library, name in NAMES.items():
        cells = []
        for op in OPS:
            value = default.get((RECORDS, op, library))
            if value is None:
                cells.append("")
                continue
            best = value == min(v for (w, o, l), v in default.items() if w == RECORDS and o == op)
            cells.append(cell(value, best))
        rows += f"| {name} | " + " | ".join(cells) + " |\n"
    page = replace_table(page, "| | JSON encode | JSON decode | binary encode | binary decode |\n|---|---:|---:|---:|---:|", rows)

    # By kind, as a ratio to the struct-mapping library.
    rows = ""
    for op in OPS:
        cells = []
        for _, workload in KINDS:
            ratio = default[(workload, op, "serpent")] / default[(workload, op, "glaze")]
            cells.append(f"**{ratio:.2f}**" if ratio < 1 else f"{ratio:.2f}")
        rows += f"| {op} | " + " | ".join(cells) + " |\n"
    page = replace_table(page, "| | strings | booleans | integers | reals | 100 × 2000 numbers |\n|---|---:|---:|---:|---:|---:|", rows)

    # Switches: default against plain.
    rows = ""
    for label, workload, op in [("JSON decode", RECORDS, "JSON decode"), ("JSON encode", RECORDS, "JSON encode"),
                                ("JSON encode, five reals", "10k records of 5 reals", "JSON encode"),
                                ("JSON encode, five integers", "10k records of 5 integers", "JSON encode"),
                                ("binary decode", RECORDS, "binary decode"), ("binary encode", RECORDS, "binary encode")]:
        rows += f"| {label} | {default[(workload, op, 'serpent')]:,.0f} | {plain[(workload, op, 'serpent')]:,.0f} |\n"
    page = replace_table(page, "| | default | plain |\n|---|---:|---:|", rows)

    # Documents.
    rows = ""
    for library, name in DOC_NAMES.items():
        cells = []
        for op, _ in DOC_OPS:
            value = default[("JSON documents", op, library)]
            best = value == min(default[("JSON documents", op, other)] for other in DOC_NAMES)
            cells.append(cell(value, best))
        rows += f"| {name} | " + " | ".join(cells) + " |\n"
    page = replace_table(page, "| | " + " | ".join(label for _, label in DOC_OPS) + " |\n|---|---:|---:|---:|---:|---:|", rows)

    page_path.write_text(page)
    return 0


if __name__ == "__main__":
    sys.exit(main())
