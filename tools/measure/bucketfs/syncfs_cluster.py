#!/usr/bin/env python3
"""Stand up an N-node syncfs mesh whose sync roots are host-visible.

Generalises `tests/acceptance/large-directory-structure/setup.sh` from its
fixed five nodes to any N, and runs the shipping `localhost/syncfs` image
rather than bare processes, so that both systems in the comparison pay
container overhead.

Every node lands on one podman network and talks to its peers by container
name; nothing is published to the host, because nothing on the host needs to
reach a syncfs node. The sync roots are bind mounts, so the poller reads them
directly — the same way it reads BucketFS's bucket directories.

    syncfs_cluster.py up --nodes 10 --root /path/to/root
    syncfs_cluster.py down --nodes 10 --root /path/to/root
"""

import argparse
import json
import os
import subprocess
import sys
import time

from pathlib import Path

IMAGE = "localhost/syncfs"

# The shipping image is a dist build, so NDEBUG puts spdlog at info and the
# statistics table, "-> Create", "-> State" and "-> Digest to" are all compiled
# out (SPEC.md I). A trace needs them, so `debug=True` runs the debug binary
# out of the mounted build tree inside the build image instead. Same network,
# same bind mounts, same arguments -- only the binary and its log level differ.
DEBUG_IMAGE = "localhost/syncfs-env"
DEBUG_BINARY = "/syncfs/.build/unixlike-clang-debug/src/syncfs"

NETWORK = "syncfsnet"
SUBNET = "10.10.20.0/24"
ZMQ_PORT = 5555
FIRST_NODE_ID = 1

# SPEC.md C: no ulimit is set in Containerfile, Containerfile.run or
# compose.yaml, and R26: connections_limit -1 resolves to the soft
# RLIMIT_NOFILE *verbatim* rather than libtorrent's usual 80%-20. So the
# connection ceiling is whatever podman happens to hand the process. The sizing
# rule is 0.70 * files * (nodes - 1) connections; 20 nodes x 10000 files wants
# ~1.3e5. Set it explicitly so no cell is silently capped, and record it.
NOFILE = 1048576

# ZMQ PUB drops what it publishes before a subscriber has finished connecting,
# and no log line reports the subscription completing (SPEC.md B9). The
# acceptance test waits out the same window with the same constant.
SUBSCRIBE_GRACE = 5.0


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def ensure_network() -> None:
    if subprocess.run(["podman", "network", "exists", NETWORK], capture_output=True).returncode == 0:
        return
    run(["podman", "network", "create", "--subnet", SUBNET, NETWORK])


def node_ids(count: int) -> list[int]:
    return [FIRST_NODE_ID + i for i in range(count)]


def node_name(node_id: int) -> str:
    return f"sfs{node_id}"


def container(root: Path, node_id: int) -> str:
    """Podman container name: unique across the host, and never on the wire."""
    return f"syncfs-{root.name}-{node_name(node_id)}"


def start_node(root: Path, node_id: int, ids: list[int], debug: bool = False,
               source: Path | None = None) -> None:
    node_root = root / node_name(node_id)
    data = node_root / "data"
    data.mkdir(parents=True, exist_ok=True)

    # One address per line, every peer but this node. SPEC.md I: a peer list
    # that parses to nothing is fatal, so N=1 is not a runnable mesh.
    peers = node_root / "peers"
    # The peers file caps a line at len("tcp://255.255.255.255:65535") + 1 = 28
    # characters (include/discovery.h:10), and an over-long line is fatal, not
    # truncated (SPEC.md I, B5). So the wire uses the short network alias --
    # "tcp://sfs20:5555" is 16 -- while the container keeps its long unique name.
    peers.write_text(
        "".join(
            f"tcp://{node_name(other)}:{ZMQ_PORT}\n"
            for other in ids
            if other != node_id
        )
    )

    subprocess.run(["podman", "rm", "-f", container(root, node_id)], capture_output=True)
    args = [
        "podman", "run", "-d",
        "--name", container(root, node_id),
        "--hostname", node_name(node_id),
        "--network", f"{NETWORK}:alias={node_name(node_id)}",
        "--ulimit", f"nofile={NOFILE}:{NOFILE}",
        "--userns", f"keep-id:uid={os.getuid()}",
        "-v", f"{data}:/data:rw,Z",
        "-v", f"{peers}:/peers:ro,Z",
    ]
    if debug:
        args += [
            "-v", f"{source}:/syncfs:ro,Z",
            "--user", str(os.getuid()),
            "--workdir", "/data",
            "--entrypoint", DEBUG_BINARY,
            DEBUG_IMAGE,
        ]
    else:
        args += [IMAGE]
    args += [
        "/peers",
        # SPEC.md V64: a wildcard host is rejected -- this string is the
        # node's identity on the control plane, not just a bind address.
        f"{node_name(node_id)}:{ZMQ_PORT}",
    ]
    run(args)


def wait_ready(root: Path, ids: list[int], deadline: float) -> None:
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
            logs = subprocess.run(
                ["podman", "logs", name], capture_output=True, text=True
            )
            if "Started libtorrent session" in logs.stdout + logs.stderr:
                pending.discard(node_id)
        if pending:
            time.sleep(0.5)

    if pending:
        raise RuntimeError(f"nodes not ready within {deadline}s: {sorted(pending)}")

    time.sleep(SUBSCRIBE_GRACE)


def observed_nofile(root: Path, node_id: int) -> int | None:
    """What the runtime actually handed the process -- the real ceiling.

    Read rather than assumed: SPEC.md C is explicit that the limit is whatever
    the runtime gives, not anything syncfs states.
    """
    pid = subprocess.run(
        ["podman", "inspect", "--format", "{{.State.Pid}}", container(root, node_id)],
        capture_output=True, text=True,
    ).stdout.strip()
    try:
        for line in Path(f"/proc/{pid}/limits").read_text().splitlines():
            if line.startswith("Max open files"):
                return int(line.split()[3])
    except (OSError, ValueError, IndexError):
        return None
    return None


def up(root: Path, count: int, deadline: float, debug: bool = False,
       source: Path | None = None) -> dict:
    if count < 2:
        raise SystemExit("a syncfs mesh needs at least 2 nodes: an empty peer list is fatal")
    ensure_network()
    root.mkdir(parents=True, exist_ok=True)
    ids = node_ids(count)
    for node_id in ids:
        start_node(root, node_id, ids, debug=debug, source=source)
    wait_ready(root, ids, deadline)

    return {
        "system": "syncfs",
        "nodes": count,
        "nofile": observed_nofile(root, ids[0]),
        "node": {
            node_name(node_id): {
                "container": container(root, node_id),
                "data_dir": str(root / node_name(node_id) / "data"),
            }
            for node_id in ids
        },
        "writer": node_name(ids[0]),
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
    up_cmd.add_argument("--deadline", type=float, default=300.0)
    up_cmd.add_argument("--debug", action="store_true",
                        help="run the debug binary from --source's build tree")
    up_cmd.add_argument("--source", type=Path, default=Path.cwd())

    down_cmd = sub.add_parser("down")
    down_cmd.add_argument("--nodes", type=int, required=True)
    down_cmd.add_argument("--root", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "up":
        print(json.dumps(
            up(args.root, args.nodes, args.deadline,
               debug=args.debug, source=args.source),
            indent=2,
        ))
    else:
        down(args.root, args.nodes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
