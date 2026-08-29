#!/usr/bin/env python3
"""How often does a single small write miss its convergence deadline?

Written because the comparison matrix kept producing a ~60 s cell among
otherwise ~0.1 s cells, on an idle mesh, at every file size. 60 s is §C's
`state` ceiling, so those writes are not converging by announcement and not by
§V.41's "≤ 2 reconcile periods after quiescence" either -- they are waiting out
the ceiling, which is the path that exists for continuous write load and there
is none here.

The rate is the point. `perf_one_to_many_test.py` runs `rounds=1, iterations=1`
(§T.3 notes the absence of variance data), and a fault that appears in a
quarter of writes is invisible in a single sample three times out of four.

    measure_tail.py --nodes 2 --writes 20
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import poller
import syncfs_cluster

# SPEC.md V19: two nodes, one small change, converge <= 5 s.
V19_DEADLINE = 5.0
SIZE = 1024


def sweep(root: Path, nodes: int, writes: int, deletes: bool) -> list[float]:
    info = syncfs_cluster.up(root, nodes, deadline=600.0)
    dirs = {n: Path(v["data_dir"]) for n, v in info["node"].items()}
    writer = info["writer"]
    payload = root / "unit.bin"
    payload.write_bytes(os.urandom(SIZE))

    elapsed = []
    previous = None
    try:
        for i in range(writes):
            if deletes and previous:
                (dirs[writer] / previous).unlink(missing_ok=True)
                poller.watch_absent(
                    {n: d / previous for n, d in dirs.items()},
                    interval=0.2, deadline=120.0,
                )
            name = f"w{i:03}.bin"
            t0 = time.time()
            shutil.copyfile(payload, dirs[writer] / name)
            records = poller.watch(
                {n: d / name for n, d in dirs.items()},
                tree=False, size=SIZE, count=1,
                interval=0.05, deadline=120.0, t0=t0, out=os.devnull,
            )
            last = max(
                (r["elapsed"] for r in records.values() if r["arrived_at"]),
                default=None,
            )
            elapsed.append(last)
            previous = name
            flag = "" if (last is not None and last <= V19_DEADLINE) else "   <-- past V19"
            print(f"  write {i:>3}: {'--' if last is None else f'{last:7.3f} s'}{flag}", flush=True)
    finally:
        syncfs_cluster.down(root, nodes)
        subprocess.run(["podman", "unshare", "rm", "-rf", str(root)], capture_output=True)
    return elapsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/var/tmp/syncfs-tail"))
    parser.add_argument("--nodes", type=int, nargs="+", default=[2, 4])
    parser.add_argument("--writes", type=int, default=20)
    parser.add_argument("--deletes", action="store_true")
    args = parser.parse_args()

    for nodes in args.nodes:
        print(f"\n{nodes} nodes, {args.writes} writes of {SIZE} B"
              f"{', deleting the previous file each time' if args.deletes else ''}")
        values = sweep(args.root, nodes, args.writes, args.deletes)
        good = [v for v in values if v is not None and v <= V19_DEADLINE]
        slow = [v for v in values if v is None or v > V19_DEADLINE]
        print(f"  {len(slow)}/{len(values)} past V19's {V19_DEADLINE} s deadline")
        if good:
            print(f"  fast writes: median {sorted(good)[len(good)//2]:.3f} s")
        if slow:
            finite = [v for v in slow if v is not None]
            if finite:
                print(f"  slow writes: median {sorted(finite)[len(finite)//2]:.3f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
