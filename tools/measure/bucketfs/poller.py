#!/usr/bin/env python3
"""Watch the same file appear on every node of a cluster, and say when.

One process watches every node, so every node is sampled against one clock and
one interval. Both syncfs and BucketFS expose their per-node storage as a host
directory, so nothing here enters a container: a `podman exec` per sample costs
100-300 ms, which at 1 KiB is most of the answer.

Arrival is the moment the file reaches its expected *size*. The hash is read
once, after, and reported separately. Folding verification into the arrival
time is the defect SPEC.md T3 records in the 1 GiB test, which re-hashes ten
gigabytes on every poll and reports the sum.

    poller.py --interval-ms 50 --deadline 900 --size 1024 \
        --out arrivals.jsonl n1=/path/one/f.bin n2=/path/two/f.bin

Tree mode watches a directory instead, and arrives when the directory holds
`--count` files whose sizes sum to `--size`:

    poller.py --tree --count 1000 --size 4096000 ... n1=/path/one n2=/path/two
"""

import argparse
import hashlib
import json
import os
import sys
import time

from pathlib import Path
from stat import S_ISREG


def file_size(path: Path) -> int:
    """Size of `path`, or -1 if it is not there yet."""
    try:
        return path.stat().st_size
    except (FileNotFoundError, NotADirectoryError, PermissionError):
        return -1


def complete(path: Path, size: int) -> bool:
    """Has `path` actually got `size` bytes in it?

    st_size alone is not the answer, and getting this wrong quietly ruins the
    measurement. libtorrent allocates sparse, so a receiving node's file
    reports its **full** size the moment the torrent is added and before a
    single byte has landed; a size-only check then reports an arrival one poll
    after the announcement, at every file size, and the run looks fast because
    it is measuring the announcement rather than the transfer. It was caught
    by a cell that "arrived" on both nodes and then disagreed on the hash.

    st_blocks counts blocks actually allocated, so a hole is visible: a full
    file always satisfies `blocks * 512 >= size` (the tail block rounds up), a
    sparse one does not. It costs the same single stat, needs nothing
    system-specific, and on ext4's delayed allocation it additionally waits for
    the writeback that syncfs itself waits for before stamping (V36, V44).
    """
    try:
        stat = path.stat()
    except (FileNotFoundError, NotADirectoryError, PermissionError):
        return False
    return stat.st_size == size and stat.st_blocks * 512 >= size


def tree_size(path: Path) -> tuple[int, int]:
    """(file count, total bytes) under `path`, counting regular files only.

    Mirrors `files::list` (src/files.cpp), which is what syncfs itself counts:
    regular non-symlink files, recursively.
    """
    count = 0
    total = 0
    try:
        for dirpath, _, names in os.walk(path):
            for name in names:
                entry = Path(dirpath) / name
                try:
                    stat = entry.stat(follow_symlinks=False)
                except OSError:
                    continue
                if not S_ISREG(stat.st_mode):
                    continue
                # Same sparse trap as `complete`: an allocated-short file is
                # not a file that has arrived.
                if stat.st_blocks * 512 < stat.st_size:
                    continue
                count += 1
                total += stat.st_size
    except OSError:
        pass
    return count, total


def sha256(path: Path) -> str:
    with open(path, "rb") as f:
        return hashlib.file_digest(f, hashlib.sha256).hexdigest()


def tree_sha256(path: Path) -> str:
    """One hash over the whole tree: relative path, NUL, content hash, NUL.

    Same shape as reconcile's canonical form (SPEC.md V46) and the same thing
    `tests/acceptance/large-directory-structure/test.sh` compares by hand, so a
    mismatch here is comparable to a mismatch there.
    """
    digest = hashlib.sha256()
    for entry in sorted(p for p in path.rglob("*") if p.is_file()):
        digest.update(str(entry.relative_to(path)).encode())
        digest.update(b"\0")
        digest.update(sha256(entry).encode())
        digest.update(b"\0")
    return digest.hexdigest()


def watch(nodes, *, tree, size, count, interval, deadline, t0, out):
    """Poll until every node arrives or the deadline passes.

    Returns one record per node. A node that never arrives gets
    `arrived_at: None` and is reported as such — a run that does not converge
    is a result, and SPEC.md R27 already holds one (5 nodes, 1000 files, 16 KiB,
    300 s) that was never written down as anything else.
    """
    pending = dict(nodes)
    records = {}
    end = t0 + deadline

    while pending and time.time() < end:
        for name, path in list(pending.items()):
            if tree:
                found, total = tree_size(path)
                arrived = found == count and total == size
            else:
                arrived = complete(path, size)

            if arrived:
                records[name] = {"node": name, "arrived_at": time.time()}
                del pending[name]

        if pending:
            time.sleep(interval)

    for name in pending:
        records[name] = {"node": name, "arrived_at": None}

    # Hashing is deliberately outside the loop above: it is verification, not
    # latency, and at 4 GiB x 20 nodes it would dominate the sampling interval.
    for name, path in nodes.items():
        if records[name]["arrived_at"] is None:
            records[name]["sha256"] = None
            continue
        records[name]["sha256"] = tree_sha256(path) if tree else sha256(path)
        records[name]["verified_at"] = time.time()

    with open(out, "w") as f:
        for name in nodes:
            record = records[name]
            record["t0"] = t0
            if record["arrived_at"] is not None:
                record["elapsed"] = record["arrived_at"] - t0
            f.write(json.dumps(record) + "\n")

    return records


def watch_absent(nodes, *, interval, deadline) -> bool:
    """Poll until the path is gone from every node, or the deadline passes.

    A cell must not leave the mesh divergent for the next one. Deleting on
    every node at once would inject exactly the divergence syncfs then has to
    reconcile, and the next cell would pay for it at the `state` ceiling
    (SPEC.md C, 60 s). So a cell deletes through the service on the writer only
    and waits here for the delete to land.
    """
    end = time.time() + deadline
    pending = dict(nodes)
    while pending and time.time() < end:
        for name, path in list(pending.items()):
            if file_size(path) == -1:
                del pending[name]
        if pending:
            time.sleep(interval)
    return not pending


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interval-ms", type=int, default=50)
    parser.add_argument("--deadline", type=float, default=900.0)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--tree", action="store_true")
    parser.add_argument("--t0", type=float, default=None)
    parser.add_argument("--out", required=True)
    parser.add_argument("targets", nargs="+", metavar="NAME=PATH")
    args = parser.parse_args()

    nodes = {}
    for target in args.targets:
        name, _, path = target.partition("=")
        if not path:
            parser.error(f"expected NAME=PATH, got {target!r}")
        nodes[name] = Path(path)

    records = watch(
        nodes,
        tree=args.tree,
        size=args.size,
        count=args.count,
        interval=args.interval_ms / 1000.0,
        deadline=args.deadline,
        t0=args.t0 if args.t0 is not None else time.time(),
        out=args.out,
    )

    missing = [n for n, r in records.items() if r["arrived_at"] is None]
    hashes = {r["sha256"] for r in records.values() if r["sha256"]}

    for name in nodes:
        record = records[name]
        elapsed = record.get("elapsed")
        print(f"{name:10} {'-- did not arrive' if elapsed is None else f'{elapsed:8.3f} s'}")

    if missing:
        print(f"did not arrive on {len(missing)}/{len(nodes)}: {', '.join(sorted(missing))}")
        return 1
    if len(hashes) != 1:
        print(f"contents differ across nodes: {len(hashes)} distinct hashes")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
