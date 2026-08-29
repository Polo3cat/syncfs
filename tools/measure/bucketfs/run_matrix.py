#!/usr/bin/env python3
"""Drive the syncfs / BucketFS comparison matrix and checkpoint every cell.

Three experiments, from `document.txt`'s own requirements:

  A  propagation  one file, one writer, every node polled to completion
                  (Scenarios 3.5, 3.12, 3.13, 3.14)
  B  small files  the UDF-load case named in the goals at document.txt:240
  C  ingest       what the writer waits for, which is where the two designs
                  differ most: a syncfs write returns as soon as the bytes are
                  on local disk, a BucketFS PUT returns once every node has them

Each cell reports three times, because one number would hide the asymmetry:

  ingest_s       t0 -> the writer node holds the complete file
  propagation_s  t0 -> the last node holds it; the user-visible number
  fanout_s       writer arrival -> last arrival; replication alone, and the
                 only term that compares the two mechanisms directly

A run that does not converge inside its deadline is written down as a
non-convergence, not dropped. SPEC.md R27 already holds one such case (5 nodes,
1000 files, 16 KiB, 300 s) that was never recorded as anything.

    run_matrix.py --experiment A --root /path/to/root --out results.csv
"""

import argparse
import csv
import http.client
import json
import os
import shutil
import subprocess
import sys
import time

from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import exasol_cluster
import poller
import syncfs_cluster

KIB = 1024
MIB = 1024 * KIB
GIB = 1024 * MIB

SIZES = [KIB, MIB, 100 * MIB, GIB, 4 * GIB]
SMALL_FILE = 4 * KIB

FIELDS = [
    "experiment", "system", "config", "nodes", "size_bytes", "file_count",
    "repeat", "t0", "ingest_s", "propagation_s", "fanout_s", "converged",
    "distinct_hashes", "nofile", "loadavg", "arrivals",
]

# A cell whose answer is within a few sampling intervals is not a measurement.
# tools/measure/README.md records what that mistake produced last time: "a
# number that is real, reproducible, and about nothing."
def interval_ms(size: int) -> int:
    return 50 if size <= MIB else 500


def deadline_for(size: int, nodes: int) -> float:
    if size >= GIB:
        return 1800.0
    return 900.0


def loadavg() -> float:
    return os.getloadavg()[0]


def wait_for_idle(threshold: float = 1.5, deadline: float = 120.0) -> float:
    """A loaded host makes a timing measurement meaningless.

    tools/measure/README.md says as much about peak measurement; it is at least
    as true of a convergence deadline.
    """
    end = time.time() + deadline
    while loadavg() > threshold and time.time() < end:
        time.sleep(10)
    return loadavg()


def make_payload(path: Path, size: int) -> None:
    """A fresh random payload per repeat, so no repeat reads what the page
    cache still holds from the last one. `.bin` is deliberate: BucketFS
    auto-decompresses archives (document.txt:220-224) and charging it for work
    syncfs does not do would not be a comparison."""
    with open(path, "wb") as f:
        remaining = size
        while remaining:
            chunk = min(remaining, 8 * MIB)
            f.write(os.urandom(chunk))
            remaining -= chunk


class Bucket:
    """One keep-alive HTTP connection to a BucketFS node.

    A fresh connection per file would charge BucketFS a TCP handshake ten
    thousand times in experiment B, which measures curl, not BucketFS.
    """

    def __init__(self, url: str, write_passwd: str):
        _, _, hostport, bucket = url.split("/", 3)
        self.host, _, port = hostport.partition(":")
        self.port = int(port)
        self.bucket = bucket
        self.auth = "w:" + write_passwd
        self.conn = http.client.HTTPConnection(self.host, self.port, timeout=3600)

    def put(self, name: str, path: Path, size: int) -> int:
        import base64
        headers = {
            "Content-Length": str(size),
            "Authorization": "Basic " + base64.b64encode(self.auth.encode()).decode(),
        }
        with open(path, "rb") as body:
            self.conn.request("PUT", f"/{self.bucket}/{name}", body=body, headers=headers)
            response = self.conn.getresponse()
            response.read()
        return response.status

    def delete(self, name: str) -> int:
        import base64
        headers = {
            "Authorization": "Basic " + base64.b64encode(self.auth.encode()).decode(),
        }
        self.conn.request("DELETE", f"/{self.bucket}/{name}", headers=headers)
        response = self.conn.getresponse()
        response.read()
        return response.status

    def close(self) -> None:
        self.conn.close()


def cluster_up(system: str, nodes: int, root: Path, sync_period: int) -> dict:
    if system == "syncfs":
        return syncfs_cluster.up(root, nodes, deadline=600.0)
    return exasol_cluster.up(root, nodes, sync_period, deadline=900.0)


def cluster_down(system: str, nodes: int, root: Path) -> None:
    if system == "syncfs":
        syncfs_cluster.down(root, nodes)
    else:
        exasol_cluster.down(root, nodes)
    purge(root)


def purge(root: Path) -> None:
    """Delete a cluster root, including files the host user cannot unlink.

    Under rootless podman a privileged container's uid 500 lands on a subuid,
    so BucketFS's own files come back EACCES to a plain rmtree. `podman
    unshare` enters the user namespace where those ids map. Leaving them
    behind is not cosmetic: a stale payload from a crashed run is still there
    at the next run's t0, and the cell reports an arrival time of ~0.
    """
    if not root.exists():
        return
    subprocess.run(["podman", "unshare", "rm", "-rf", str(root)], capture_output=True)
    shutil.rmtree(root, ignore_errors=True)


def target_paths(info: dict, name: str) -> dict[str, Path]:
    """Where each node's copy of `name` will land.

    For BucketFS the bucket directory and the `.dest` directory are hardlinks
    to one inode, so either answers; `.dest` is used because it is what a
    receiving node's rsync writes into.
    """
    if info["system"] == "syncfs":
        return {n: Path(v["data_dir"]) / name for n, v in info["node"].items()}
    return {n: Path(v["bucket_dir"]) / name for n, v in info["node"].items()}


def target_dirs(info: dict) -> dict[str, Path]:
    key = "data_dir" if info["system"] == "syncfs" else "bucket_dir"
    return {n: Path(v[key]) for n, v in info["node"].items()}


def writer_of(info: dict) -> str:
    return info.get("writer") or sorted(info["node"])[0]


def summarise(records: dict, writer: str, t0: float) -> dict:
    arrivals = {n: r.get("elapsed") for n, r in records.items()}
    hashes = {r["sha256"] for r in records.values() if r["sha256"]}
    converged = all(v is not None for v in arrivals.values()) and len(hashes) == 1
    ingest = arrivals.get(writer)
    last = max((v for v in arrivals.values() if v is not None), default=None)
    return {
        "ingest_s": None if ingest is None else round(ingest, 4),
        "propagation_s": None if last is None else round(last, 4),
        "fanout_s": None if (last is None or ingest is None) else round(last - ingest, 4),
        "converged": converged,
        "distinct_hashes": len(hashes),
        "arrivals": json.dumps({n: (None if v is None else round(v, 4)) for n, v in arrivals.items()}),
    }


def name_for(size: int, repeat: int) -> str:
    return f"probe-{size}-{repeat}.bin"


def clear_cell(info: dict, name: str) -> None:
    """Remove the cell's file through the service, on the writer node only.

    Both systems replicate the delete themselves -- that is Scenario 3.11 -- so
    the mesh is left consistent for the next cell rather than divergent. The
    host cannot unlink a BucketFS file anyway: it is owned by the container's
    uid, and a host-side unlink fails with EACCES.
    """
    writer = writer_of(info)
    if info["system"] == "syncfs":
        (Path(info["node"][writer]["data_dir"]) / name).unlink(missing_ok=True)
    else:
        bucket = Bucket(info["node"][writer]["url"], info["write_passwd"])
        try:
            bucket.delete(name)
        finally:
            bucket.close()

    gone = poller.watch_absent(
        target_paths(info, name), interval=0.2, deadline=180.0
    )
    if not gone:
        print(f"  warning: {name} still present somewhere after 180 s", flush=True)


def propagation_cell(info: dict, size: int, repeat: int, staging: Path) -> dict:
    name = name_for(size, repeat)
    payload = staging / name
    make_payload(payload, size)

    nodes = target_paths(info, name)
    writer = writer_of(info)
    load = wait_for_idle()

    t0 = time.time()
    if info["system"] == "syncfs":
        # A direct write into the sync root is what a user does, and it is the
        # same shape as a PUT: the file grows to full size in place.
        started = _spawn_copy(payload, nodes[writer])
    else:
        bucket = Bucket(info["node"][writer]["url"], info["write_passwd"])
        started = _spawn_put(bucket, name, payload, size)

    records = poller.watch(
        nodes, tree=False, size=size, count=1,
        interval=interval_ms(size) / 1000.0,
        deadline=deadline_for(size, info["nodes"]),
        t0=t0, out=os.devnull,
    )
    started()
    payload.unlink(missing_ok=True)

    row = summarise(records, writer, t0)
    row.update(loadavg=round(load, 2), nofile=info.get("nofile"))
    return row


def _spawn_copy(src: Path, dst: Path):
    """Start the write on a thread so the poller can sample it as it happens."""
    import threading
    error = []

    def body():
        try:
            shutil.copyfile(src, dst)
        except Exception as e:  # noqa: BLE001 - reported, not swallowed
            error.append(e)

    thread = threading.Thread(target=body, daemon=True)
    thread.start()

    def join():
        thread.join()
        if error:
            raise error[0]

    return join


def _spawn_put(bucket: Bucket, name: str, payload: Path, size: int):
    import threading
    error = []

    def body():
        try:
            status = bucket.put(name, payload, size)
            if status not in (200, 201, 204):
                error.append(RuntimeError(f"PUT returned {status}"))
        except Exception as e:  # noqa: BLE001
            error.append(e)

    thread = threading.Thread(target=body, daemon=True)
    thread.start()

    def join():
        thread.join()
        bucket.close()
        if error:
            raise error[0]

    return join


def append(out: Path, row: dict) -> None:
    exists = out.exists()
    with open(out, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        if not exists:
            writer.writeheader()
        writer.writerow({k: row.get(k) for k in FIELDS})


def done_cells(out: Path) -> set[tuple]:
    """Resume support: a matrix that takes hours must survive an interruption."""
    if not out.exists():
        return set()
    with open(out, newline="") as f:
        return {
            (r["experiment"], r["system"], r["config"], r["nodes"],
             r["size_bytes"], r["file_count"], r["repeat"])
            for r in csv.DictReader(f)
        }


def experiment_a(root: Path, out: Path, repeats: int, plan: list) -> None:
    done = done_cells(out)
    staging = root / "staging"
    staging.mkdir(parents=True, exist_ok=True)

    for system, config, nodes, sync_period in plan:
        wanted = [
            (size, repeat)
            for size in SIZES
            for repeat in range(repeats)
            if ("A", system, config, str(nodes), str(size), "1", str(repeat)) not in done
        ]
        if not wanted:
            continue

        cluster_root = root / f"{system}-{config}-{nodes}"
        info = cluster_up(system, nodes, cluster_root, sync_period)
        try:
            for size, repeat in wanted:
                cell = propagation_cell(info, size, repeat, staging)
                cell.update(
                    experiment="A", system=system, config=config, nodes=nodes,
                    size_bytes=size, file_count=1, repeat=repeat, t0=time.time(),
                )
                append(out, cell)
                print(
                    f"A {system:9} {config:8} N={nodes:<3} {size:>11} r{repeat} "
                    f"ingest={cell['ingest_s']} prop={cell['propagation_s']} "
                    f"fanout={cell['fanout_s']} converged={cell['converged']}",
                    flush=True,
                )
                clear_cell(info, name_for(size, repeat))
        finally:
            cluster_down(system, nodes, cluster_root)


def tree_cell(info: dict, count: int, repeat: int, staging: Path) -> dict:
    """Experiment B: `count` small files at once, the UDF-load case.

    The writes are driven from one long-lived process -- a keep-alive HTTP
    connection for BucketFS, plain writes for syncfs -- so neither system is
    charged for ten thousand process spawns.
    """
    prefix = f"many-{count}-{repeat}"
    payload = staging / f"{prefix}.unit"
    make_payload(payload, SMALL_FILE)
    total = SMALL_FILE * count

    writer = writer_of(info)
    dirs = target_dirs(info)
    subdirs = {node: path / prefix for node, path in dirs.items()}
    load = wait_for_idle()

    t0 = time.time()
    started = (
        _spawn_tree_copy(payload, subdirs[writer], count)
        if info["system"] == "syncfs"
        else _spawn_tree_put(
            Bucket(info["node"][writer]["url"], info["write_passwd"]),
            prefix, payload, count,
        )
    )

    records = poller.watch(
        subdirs, tree=True, size=total, count=count,
        interval=0.5, deadline=900.0, t0=t0, out=os.devnull,
    )
    started()
    payload.unlink(missing_ok=True)

    row = summarise(records, writer, t0)
    row.update(loadavg=round(load, 2), nofile=info.get("nofile"))
    return row


def _spawn_tree_copy(unit: Path, target: Path, count: int):
    import threading
    error = []

    def body():
        try:
            target.mkdir(parents=True, exist_ok=True)
            for i in range(count):
                shutil.copyfile(unit, target / f"f{i:05}.bin")
        except Exception as e:  # noqa: BLE001
            error.append(e)

    thread = threading.Thread(target=body, daemon=True)
    thread.start()

    def join():
        thread.join()
        if error:
            raise error[0]

    return join


def _spawn_tree_put(bucket: Bucket, prefix: str, unit: Path, count: int):
    import threading
    error = []

    def body():
        try:
            for i in range(count):
                status = bucket.put(f"{prefix}/f{i:05}.bin", unit, SMALL_FILE)
                if status not in (200, 201, 204):
                    error.append(RuntimeError(f"PUT {i} returned {status}"))
                    return
        except Exception as e:  # noqa: BLE001
            error.append(e)

    thread = threading.Thread(target=body, daemon=True)
    thread.start()

    def join():
        thread.join()
        bucket.close()
        if error:
            raise error[0]

    return join


def experiment_b(root: Path, out: Path, repeats: int, plan: list, counts: list) -> None:
    done = done_cells(out)
    staging = root / "staging"
    staging.mkdir(parents=True, exist_ok=True)

    for system, config, nodes, sync_period in plan:
        wanted = [
            (count, repeat)
            for count in counts
            for repeat in range(repeats)
            if ("B", system, config, str(nodes), str(SMALL_FILE), str(count), str(repeat)) not in done
        ]
        if not wanted:
            continue

        cluster_root = root / f"B-{system}-{config}-{nodes}"
        info = cluster_up(system, nodes, cluster_root, sync_period)
        try:
            for count, repeat in wanted:
                cell = tree_cell(info, count, repeat, staging)
                cell.update(
                    experiment="B", system=system, config=config, nodes=nodes,
                    size_bytes=SMALL_FILE, file_count=count, repeat=repeat,
                    t0=time.time(),
                )
                append(out, cell)
                print(
                    f"B {system:9} {config:8} N={nodes:<3} {count:>6} files r{repeat} "
                    f"ingest={cell['ingest_s']} prop={cell['propagation_s']} "
                    f"converged={cell['converged']}",
                    flush=True,
                )
                # A fresh cluster per cell: clearing ten thousand files through
                # the service would take longer than the cell it is cleaning up
                # after, and would leave the mesh churning into the next one.
                cluster_down(system, nodes, cluster_root)
                info = cluster_up(system, nodes, cluster_root, sync_period)
        finally:
            cluster_down(system, nodes, cluster_root)


def experiment_c(root: Path, out: Path, repeats: int) -> None:
    """The plain-file baseline both systems' ingest is measured against.

    Scenario 3.4 asks for write throughput "identical to a regular file". The
    per-cell `ingest_s` in experiments A and B is the other half; this is the
    reference it is compared to, on the same filesystem, same payload sizes.
    """
    done = done_cells(out)
    staging = root / "staging"
    staging.mkdir(parents=True, exist_ok=True)
    plain = root / "plain"
    plain.mkdir(parents=True, exist_ok=True)

    for size in SIZES:
        for repeat in range(repeats):
            if ("C", "plainfile", "baseline", "0", str(size), "1", str(repeat)) in done:
                continue
            payload = staging / f"plain-{size}-{repeat}.bin"
            make_payload(payload, size)
            load = wait_for_idle()
            target = plain / f"copy-{size}-{repeat}.bin"

            t0 = time.time()
            shutil.copyfile(payload, target)
            # copyfile returns when the last write() returns, which on ext4 is
            # before the data is on the device. syncfs's own definition of done
            # waits for a flush (V36, V44), so the baseline waits for one too.
            fd = os.open(target, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            elapsed = time.time() - t0

            payload.unlink(missing_ok=True)
            target.unlink(missing_ok=True)
            append(out, {
                "experiment": "C", "system": "plainfile", "config": "baseline",
                "nodes": 0, "size_bytes": size, "file_count": 1, "repeat": repeat,
                "t0": t0, "ingest_s": round(elapsed, 4),
                "propagation_s": round(elapsed, 4), "fanout_s": 0.0,
                "converged": True, "distinct_hashes": 1, "nofile": None,
                "loadavg": round(load, 2), "arrivals": "{}",
            })
            print(f"C plainfile  baseline N=0   {size:>11} r{repeat} write={elapsed:.4f}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment", choices=["A", "B", "C"], default="A")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--sizes", type=str, default=None,
                        help="comma-separated byte sizes, overriding the default ladder")
    parser.add_argument("--plan", type=Path, default=None,
                        help="JSON list of [system, config, nodes, sync_period_ms]")
    parser.add_argument("--counts", type=str, default="100,1000,10000",
                        help="experiment B: comma-separated file counts")
    args = parser.parse_args()

    if args.plan:
        plan = [tuple(entry) for entry in json.loads(args.plan.read_text())]
    else:
        plan = (
            [("syncfs", "default", n, 0) for n in (2, 4, 10, 20)]
            + [("bucketfs", "default", n, 30000) for n in (2, 4)]
            + [("bucketfs", "tuned", n, 1000) for n in (2, 4)]
        )

    if args.sizes:
        global SIZES
        SIZES = [int(s) for s in args.sizes.split(",")]

    args.root.mkdir(parents=True, exist_ok=True)
    if args.experiment == "A":
        experiment_a(args.root, args.out, args.repeats, plan)
    elif args.experiment == "B":
        counts = [int(c) for c in args.counts.split(",")]
        experiment_b(args.root, args.out, args.repeats, plan, counts)
    else:
        experiment_c(args.root, args.out, args.repeats)
    return 0


if __name__ == "__main__":
    sys.exit(main())
