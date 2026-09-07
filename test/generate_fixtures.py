#!/usr/bin/env python3
"""Generates cross-implementation fixtures from the dart-bjdata reference.

For each document this writes, into test/fixtures/:

  <name>.json    the source document
  <name>.bjd     dart-bjdata's draft 3 encoding of it
  <name>.blocks  dart-bjdata's block notation, whitespace stripped
  <name>.digest  a canonical rendering of what dart-bjdata decodes those bytes back to

bjdata_fixture_test.cpp then requires the C++ view to agree with both .blocks and .digest,
so the reader is checked against an independent implementation rather than against vectors
transcribed by hand.

Usage: generate_fixtures.py [path-to-bjdata-cli]
  The CLI comes from https://github.com/nebkat/dart-bjdata:
      dart compile exe bin/bjdata.dart -o /tmp/bjdatacli
"""

import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
FIXTURES = HERE / "fixtures"

# Draft 3 only: v1 of the C++ reader does not implement Structure-of-Arrays.
FLAGS = ["--draft3", "--no-soa"]

DOCUMENTS = {
    "null": None,
    "true": True,
    "false": False,
    "zero": 0,
    "uint8_max": 255,
    "uint16": 256,
    "int16_negative": -129,
    "uint32": 4294967295,
    "int64_negative": -9223372036854775808,
    "real_pi": 3.141592653589793,
    "real_half": 1.5,
    "real_tiny": 1e-20,
    "real_huge": 1e20,
    "string_empty": "",
    "string_hello": "hello",
    "string_utf8": "héllo wörld ✓",
    "string_long": "x" * 400,
    "array_empty": [],
    "array_small": [1, 2, 3],
    "array_negative": [-1, -2, -3],
    "array_mixed": [None, True, False, 1, "a"],
    "array_strings": ["a", "bb", "ccc"],
    "array_reals": [1.5, 2.5, -0.25],
    "array_wide": [100000, 200000, 300000],
    "array_uniform_long": [i % 200 for i in range(300)],
    "array_nested": [[1, 2, 3], [4, 5, 6]],
    "array_of_objects": [{"a": 1}, {"a": 2}, {"a": 3}],
    "object_empty": {},
    "object_flat": {"foo": 1, "bar": 2},
    "object_typed": {"n": None, "t": True, "f": False},
    "object_nested": {"a": [1, 2], "b": "x", "c": {"d": [{"e": 1}]}},
    "object_utf8_keys": {"ké": 1, "wörld": 2},
    "deep": [[[[[[[1]]]]]]],

    # Straddle the numeric-packing decision, which is where the writer is easiest to get
    # wrong: a generic array stores each value at its own width, a typed one pays the widest
    # throughout, and a tie must keep the generic form.
    "pack_boundary_4": [1, 2, 3, 4],
    "pack_boundary_5": [1, 2, 3, 4, 5],
    "pack_widened": [1, 2, 3, 1000000],
    "pack_tie": [52445, 43707, 13124, 4386],
    "pack_negatives": [-1, -2, -3, -4, -5, -6],
    "pack_halves": [index / 2 for index in range(100)],
    "pack_reals_exact": [0.1, 0.2, 0.3],
    "pack_mixed_numeric": [1, 2.5, 3],
    "pack_bools": [True, False, True, False, True, False],
    "pack_nulls": [None] * 6,
    "pack_wide_span": [0, 255, 256, 65535, 65536],
}


def digest(value) -> str:
    """A canonical, unambiguous rendering of a decoded document."""
    if value is None:
        return "Z"
    if value is True:
        return "T"
    if value is False:
        return "F"
    if isinstance(value, int):
        return f"i:{value}"
    if isinstance(value, float):
        return f"d:{value!r}"
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        return f"s{len(encoded)}:{value}"
    if isinstance(value, list):
        return "[" + ",".join(digest(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(f"{key}={digest(item)}" for key, item in value.items()) + "}"
    raise TypeError(f"cannot digest {type(value)}")


def main() -> int:
    cli = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/bjdatacli")
    if not cli.exists():
        print(f"bjdata CLI not found at {cli}; build it with:", file=sys.stderr)
        print("  git clone https://github.com/nebkat/dart-bjdata && cd dart-bjdata", file=sys.stderr)
        print("  dart compile exe bin/bjdata.dart -o /tmp/bjdatacli", file=sys.stderr)
        return 2

    FIXTURES.mkdir(parents=True, exist_ok=True)
    for stale in FIXTURES.glob("*"):
        stale.unlink()

    for name, document in DOCUMENTS.items():
        source = FIXTURES / f"{name}.json"
        encoded = FIXTURES / f"{name}.bjd"
        source.write_text(json.dumps(document), encoding="utf-8")

        subprocess.run([cli, "encode", source, encoded, *FLAGS], check=True)

        blocks = subprocess.run([cli, "block", source, *FLAGS],
                                check=True, capture_output=True, text=True).stdout
        # Strip the pretty-printer's indentation only; a block never spans a line, so
        # spaces inside a string body must survive.
        flattened = "".join(line.strip() for line in blocks.splitlines())
        (FIXTURES / f"{name}.blocks").write_text(flattened, encoding="utf-8")

        # Digest what the reference decodes the bytes back to, not what we fed in, so the
        # comparison is against a real decode of the exact bytes under test.
        decoded = subprocess.run([cli, "decode", encoded],
                                 check=True, capture_output=True, text=True).stdout
        (FIXTURES / f"{name}.digest").write_text(digest(json.loads(decoded)), encoding="utf-8")

        # dart-bjdata's own JSON rendering of those bytes, two-space indented. The C++ JSON
        # writer must reproduce it exactly, so JSON output is checked against the reference
        # rather than against expectations written by hand.
        (FIXTURES / f"{name}.json.expected").write_text(decoded.rstrip("\n"), encoding="utf-8")

        print(f"{name}: {encoded.stat().st_size} bytes")

    print(f"\n{len(DOCUMENTS)} fixtures written to {FIXTURES}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
