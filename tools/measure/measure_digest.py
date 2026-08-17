"""What one reconcile round puts on the wire. Produced R28.

A digest is addressed at one peer and the publisher filters on its subscription
trie (V58), so it never reaches the wire of a node that did not ask for it and
cannot be sniffed from the side. This stands up a peer instead: it publishes a
root hash that cannot match anybody's, which makes the node under measurement
draw it as the peer to ask, and subscribes to the digest addressed back at it.

Needs pyzmq, which the project does not depend on. See README.md: installing it
into the test image is a one liner, and adding it to pyproject.toml would be a
§C change rather than a tooling one.
"""

import subprocess
import sys
import tempfile
import time
from pathlib import PosixPath

import zmq

NODE_ADDR = "localhost:6100"
PROBE_ADDR = "localhost:6101"
NODE_ENDPOINT = f"tcp://{NODE_ADDR}"
PROBE_ENDPOINT = f"tcp://{PROBE_ADDR}"

# Thirty two bytes that cannot be the root hash of any tree: the node will see
# it differ from its own every round and address a digest here.
BOGUS_HASH = b"\x00" * 32


def addressed(verb: bytes, endpoint: str) -> bytes:
    """V58: the verb and the one endpoint it is for, each closed by a NUL."""
    return verb + b"\0" + endpoint.encode() + b"\0"


def measure(files: int, depth_one: bool, budget: float = 90.0) -> dict | None:
    with (
        tempfile.TemporaryDirectory() as scratch,
        tempfile.TemporaryDirectory() as tree,
    ):
        peers = PosixPath(scratch) / "peers"
        peers.write_text(PROBE_ENDPOINT)

        names = []
        for n in range(files):
            name = f"{n % 3}/f{n:04}" if depth_one else f"f{n:04}"
            path = PosixPath(tree) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("x" * 64)
            names.append(name)

        ctx = zmq.Context()
        sub = ctx.socket(zmq.SUB)
        sub.setsockopt(zmq.RCVHWM, 100000)
        sub.setsockopt(zmq.SUBSCRIBE, addressed(b"digest", PROBE_ENDPOINT))
        sub.connect(NODE_ENDPOINT)

        pub = ctx.socket(zmq.PUB)
        pub.setsockopt(zmq.SNDHWM, 100000)
        pub.bind(f"tcp://{PROBE_ADDR}")

        node = subprocess.Popen(
            ["syncfs", str(peers), NODE_ADDR],
            cwd=tree,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            # A publisher drops what it sends before its subscribers have
            # finished connecting, at both ends of this.
            time.sleep(3)
            deadline = time.monotonic() + budget
            while time.monotonic() < deadline:
                pub.send_multipart(
                    [b"state", BOGUS_HASH, PROBE_ENDPOINT.encode()]
                )
                if sub.poll(timeout=1000):
                    parts = sub.recv_multipart()
                    return {
                        "files": files,
                        "depth_one": depth_one,
                        "parts": [len(p) for p in parts],
                        "total": sum(len(p) for p in parts),
                        "held_bytes": len(parts[2]),
                        "path_chars": sum(len(n) + 2 for n in names),
                    }
            return None
        finally:
            node.terminate()
            try:
                node.wait(timeout=20)
            except subprocess.TimeoutExpired:
                node.kill()
            sub.close()
            pub.close()
            ctx.term()


def main() -> None:
    print(
        "files depth  part0 part1  part2(held) part3(tomb)  total  "
        "bytes/path"
    )
    for files, depth_one in [
        (0, False),
        (10, False),
        (100, False),
        (1000, False),
        (100, True),
        (1000, True),
    ]:
        r = measure(files, depth_one)
        if r is None:
            print(f"{files:5} {str(depth_one):>5}  no digest arrived")
            sys.stdout.flush()
            continue
        p = r["parts"]
        per = r["held_bytes"] / files if files else 0.0
        print(
            f"{r['files']:5} {str(r['depth_one']):>5}  {p[0]:5} {p[1]:5} "
            f"{p[2]:11} {p[3]:11}  {r['total']:6}  {per:10.1f}"
        )
        sys.stdout.flush()


if __name__ == "__main__":
    main()
