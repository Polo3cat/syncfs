#!/usr/bin/env python3
"""Stand up an N-node Exasol cluster whose BucketFS is host-visible.

The image ships an Exasol Community Edition license capped at
`max_nodes_per_cluster = 1`, which caps the *database*. `init-sc --no-db`
leaves the database out entirely, and `bucketfsd` — a COS service — still runs
per node and still replicates. So an N-node BucketFS is measurable under the
bundled license, which is the whole reason this comparison can be run at all.

Two things make the measurement possible:

  * every node's `/exa` is a host bind mount, so the poller reads
    `/exa/data/bucketfs/bfsdefault/...` directly and never pays a `podman exec`
  * every node's PrivateNet lands on one podman network, because `init-sc`
    aborts with `IndexError: list index out of range` when no local interface
    matches the EXAConf subnet

`SyncPeriod` (default 30000 ms) and `HttpPort` (default 0, disabled) are both
set here. HTTP is enabled in place of HTTPS so that BucketFS is not charged for
TLS that syncfs's plaintext ZMQ and BitTorrent planes do not pay.

    exasol_cluster.py up --nodes 4 --root /path/to/root
    exasol_cluster.py down --root /path/to/root
"""

import argparse
import base64
import json
import re
import shutil
import subprocess
import sys
import time

from pathlib import Path

IMAGE = "docker.io/exasol/docker-db:2026.1.1"
NETWORK = "exanet"
SUBNET = "10.10.10.0/24"
FIRST_NODE_ID = 11
BUCKETFS = "bfsdefault"
BUCKET = "default"
CONTAINER_HTTP_PORT = 2580
FIRST_HOST_PORT = 2600


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def ensure_network() -> None:
    """The subnet must match EXAConf's PrivateNet or `init-sc` dies at startup.

    `get_local_ip` picks the interface inside the node's own subnet and indexes
    [0] on the result, so a container on podman's default 10.88.0.0/16 raises
    IndexError before any service starts.
    """
    existing = subprocess.run(
        ["podman", "network", "exists", NETWORK], capture_output=True
    )
    if existing.returncode == 0:
        return
    run(["podman", "network", "create", "--subnet", SUBNET, NETWORK])


def node_ids(count: int) -> list[int]:
    return [FIRST_NODE_ID + i for i in range(count)]


def node_ip(node_id: int) -> str:
    return f"10.10.10.{node_id}"


def host_port(node_id: int) -> int:
    return FIRST_HOST_PORT + (node_id - FIRST_NODE_ID)


def container(root: Path, node_id: int) -> str:
    return f"exa-{root.name}-n{node_id}"


def write_template(root: Path, count: int, sync_period: int) -> Path:
    """Generate one EXAConf for the whole cluster, then edit BucketFS on it.

    Every node must see the *same* EXAConf — the UUIDs, the SyncKey and the
    bucket passwords are generated, so a per-node `init-sc` would produce N
    mutually unintelligible clusters.
    """
    template = root / "_template"
    if template.exists():
        shutil.rmtree(template)
    (template / "etc").mkdir(parents=True)

    run(
        [
            "podman", "run", "--rm", "--privileged",
            "--network", NETWORK, "--ip", node_ip(FIRST_NODE_ID),
            "-v", f"{template}:/exa:Z",
            IMAGE,
            "init-sc", "--template",
            "--num-nodes", str(count),
            "--node-id", str(FIRST_NODE_ID),
            "--no-db", "--no-odirect",
        ]
    )

    exaconf = template / "etc" / "EXAConf"
    # exaconf recomputes EXAConf's Checksum on commit; a sed would leave it
    # stale and the config would be rejected at startup.
    run(
        [
            "podman", "run", "--rm", "--privileged",
            "-v", f"{template}:/exa:Z",
            "--entrypoint", "/opt/exasol/cos-8.66.7/bin/exaconf",
            IMAGE,
            "modify-bucketfs",
            "--name", BUCKETFS,
            "--http-port", str(CONTAINER_HTTP_PORT),
            "--sync-period", str(sync_period),
            "/exa/etc/EXAConf",
        ]
    )
    return exaconf


def read_passwords(exaconf: Path) -> dict[str, str]:
    """Bucket passwords are stored base64-encoded, and curl needs them decoded."""
    text = exaconf.read_text()
    out = {}
    for key, field in (("read", "ReadPasswd"), ("write", "WritePasswd")):
        match = re.search(rf"^\s*{field}\s*=\s*(\S+)\s*$", text, re.M)
        if not match:
            raise RuntimeError(f"no {field} in {exaconf}")
        out[key] = base64.b64decode(match.group(1)).decode()
    return out


def start_node(root: Path, node_id: int, exaconf: Path) -> None:
    node_root = root / f"n{node_id}"
    (node_root / "etc").mkdir(parents=True, exist_ok=True)
    shutil.copy(exaconf, node_root / "etc" / "EXAConf")

    subprocess.run(["podman", "rm", "-f", container(root, node_id)], capture_output=True)
    run(
        [
            "podman", "run", "-d",
            "--name", container(root, node_id),
            "--privileged",
            "--hostname", f"n{node_id}",
            "--network", NETWORK, "--ip", node_ip(node_id),
            "-v", f"{node_root}:/exa:Z",
            "-p", f"{host_port(node_id)}:{CONTAINER_HTTP_PORT}",
            IMAGE,
            "init-sc",
            "--num-nodes", "0",  # ignored: EXAConf is already initialized
            "--node-id", str(node_id),
            "--no-db", "--no-odirect",
        ]
    )


def wait_ready(root: Path, ids: list[int], deadline: float) -> None:
    """Ready == bucketfsd answers HTTP, not == the container is up.

    A container that has exited is reported at once rather than waited out:
    `init-sc` fails fast and loudly, and a silent 10-minute wait on a dead
    container is the least useful failure mode available.
    """
    end = time.time() + deadline
    pending = set(ids)
    while pending and time.time() < end:
        for node_id in sorted(pending):
            name = container(root, node_id)
            state = subprocess.run(
                ["podman", "inspect", "--format", "{{.State.Status}}", name],
                capture_output=True, text=True,
            ).stdout.strip()
            if state == "exited":
                logs = subprocess.run(
                    ["podman", "logs", "--tail", "20", name],
                    capture_output=True, text=True,
                )
                raise RuntimeError(f"{name} exited during startup:\n{logs.stdout}{logs.stderr}")
            probe = subprocess.run(
                ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                 "--max-time", "3",
                 f"http://localhost:{host_port(node_id)}/{BUCKET}/"],
                capture_output=True, text=True,
            )
            if probe.stdout.strip() in {"200", "401", "403", "404"}:
                pending.discard(node_id)
        if pending:
            time.sleep(2)

    if pending:
        raise RuntimeError(f"nodes not ready within {deadline}s: {sorted(pending)}")


def bucket_dir(root: Path, node_id: int) -> Path:
    """Where a synced file lands on this node.

    `.dest` is what rsync writes into on a receiving node; the plain bucket
    directory is the uploader's own view. Both are checked by the caller at G2.
    """
    return root / f"n{node_id}" / "data" / "bucketfs" / BUCKETFS / ".dest" / BUCKET


def up(root: Path, count: int, sync_period: int, deadline: float) -> dict:
    ensure_network()
    root.mkdir(parents=True, exist_ok=True)
    exaconf = write_template(root, count, sync_period)
    ids = node_ids(count)
    for node_id in ids:
        start_node(root, node_id, exaconf)
    wait_ready(root, ids, deadline)

    passwords = read_passwords(exaconf)
    return {
        "system": "bucketfs",
        "nodes": count,
        "sync_period_ms": sync_period,
        "read_passwd": passwords["read"],
        "write_passwd": passwords["write"],
        "node": {
            f"n{node_id}": {
                "container": container(root, node_id),
                "url": f"http://localhost:{host_port(node_id)}/{BUCKET}",
                "bucket_dir": str(bucket_dir(root, node_id)),
                "own_dir": str(
                    root / f"n{node_id}" / "data" / "bucketfs" / BUCKETFS / BUCKET
                ),
            }
            for node_id in ids
        },
    }


def down(root: Path, count: int) -> None:
    for node_id in node_ids(count):
        subprocess.run(["podman", "rm", "-f", container(root, node_id)], capture_output=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    up_cmd = sub.add_parser("up")
    up_cmd.add_argument("--nodes", type=int, required=True)
    up_cmd.add_argument("--root", type=Path, required=True)
    up_cmd.add_argument("--sync-period", type=int, default=30000)
    up_cmd.add_argument("--deadline", type=float, default=600.0)

    down_cmd = sub.add_parser("down")
    down_cmd.add_argument("--nodes", type=int, required=True)
    down_cmd.add_argument("--root", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "up":
        print(json.dumps(up(args.root, args.nodes, args.sync_period, args.deadline), indent=2))
    else:
        down(args.root, args.nodes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
