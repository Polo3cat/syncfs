#!/usr/bin/env python3
"""Read the matrix CSVs and print the table a §R row is written from.

Median, min and max per cell -- not mean. One 60 s outlier in five repeats
moves a mean past every other sample and hides both the typical case and the
outlier; the median plus the range shows each of them. Both existing
performance tests report a single sample and no distribution at all (§T.3).
"""

import argparse
import csv
import statistics
import sys

from pathlib import Path

METRICS = ("ingest_s", "propagation_s", "fanout_s")


def load(paths):
    rows = []
    for path in paths:
        with open(path, newline="") as f:
            rows.extend(csv.DictReader(f))
    return rows


def human(size: int) -> str:
    for unit, scale in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if size >= scale:
            value = size / scale
            return f"{value:g} {unit}"
    return f"{size} B"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--metric", choices=METRICS, default="propagation_s")
    args = parser.parse_args()

    rows = load(args.csv)
    cells = {}
    for row in rows:
        key = (row["system"], row["config"], int(row["nodes"]), int(row["size_bytes"]))
        cells.setdefault(key, []).append(row)

    print(f"{args.metric}, seconds -- median (min-max), n, and non-converged count\n")
    header = f"{'system':10} {'config':8} {'N':>3} {'size':>9} {'median':>9} {'min':>9} {'max':>9} {'n':>3} {'bad':>4}"
    print(header)
    print("-" * len(header))

    for key in sorted(cells, key=lambda k: (k[0], k[1], k[3], k[2])):
        system, config, nodes, size = key
        group = cells[key]
        values = [float(r[args.metric]) for r in group if r[args.metric]]
        bad = sum(1 for r in group if r["converged"] != "True")
        if not values:
            print(f"{system:10} {config:8} {nodes:>3} {human(size):>9} {'--':>9} {'--':>9} {'--':>9} {len(group):>3} {bad:>4}")
            continue
        print(
            f"{system:10} {config:8} {nodes:>3} {human(size):>9} "
            f"{statistics.median(values):>9.3f} {min(values):>9.3f} {max(values):>9.3f} "
            f"{len(group):>3} {bad:>4}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
