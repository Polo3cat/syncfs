"""What the node and file count ceiling actually costs. Produced R27.

Reads the concurrent peer count out of the daemon's own statistics table rather
than inferring it from anything: that count is what connections_limit gates,
and libtorrent is the only witness to it. Descriptors are counted alongside,
which is how R27 established that the two do not track each other.

The file sizes matter and are not arbitrary. An earlier version of this used
sixty four byte files and reported a descriptor count that plateaued while the
torrent count grew eighty fold, which reads exactly like a ceiling that is not
there: the transfers finished between two samples, so what it measured was a
steady state and never a peak. Files have to be big enough that transfers
overlap or this measures nothing.

The two node case is in here to be disregarded, and is left in on purpose. With
one peer it reports roughly a hundredth of the connections the five and ten node
cases do, because a single loopback transfer finishes inside the two second
statistics tick. It is an artifact of the sampling rate, not a property of the
daemon, and R27 records it as one.
"""

import os
import re
import resource
import subprocess
import sys
import tempfile
import time
from pathlib import PosixPath

BASE_PORT = 6200

# One row of the table the daemon prints at debug every two seconds:
# name, progress, total, seeds, peers, state, tab separated with the wide
# columns doubled.
ROW = re.compile(r"^[^\t]*\t\t\d+\.\d\d\t\t\d+\t\t(\d+)\t(\d+)\t(\S+)$")


def fd_count(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except (FileNotFoundError, PermissionError):
        return -1


def peers_file(directory: str, index: int, addrs: list[str]) -> str:
    path = PosixPath(directory) / f"peers{index:02}"
    path.write_text(
        "\n".join(f"tcp://{a}" for j, a in enumerate(addrs) if j != index)
    )
    return str(path)


def peak_peers(log: str) -> tuple[int, int]:
    """The most peer connections the session held at once, and the most
    torrents it listed at once. Each statistics table is one block, so the
    blocks are split on the header and summed within."""
    best_peers = 0
    best_torrents = 0
    for block in log.split("Name\t\tProgr"):
        total = 0
        rows = 0
        for line in block.splitlines():
            found = ROW.match(line)
            if found:
                total += int(found.group(2))
                rows += 1
        best_peers = max(best_peers, total)
        best_torrents = max(best_torrents, rows)
    return best_peers, best_torrents


def run(nodes: int, files: int, size: int, budget: float) -> dict:
    addrs = [f"localhost:{BASE_PORT + n}" for n in range(nodes)]
    with tempfile.TemporaryDirectory() as scratch:
        dirs = [tempfile.TemporaryDirectory() for _ in range(nodes)]
        logs = [
            open(PosixPath(scratch) / f"log{i:02}", "w+") for i in range(nodes)
        ]
        try:
            procs = [
                subprocess.Popen(
                    ["syncfs", peers_file(scratch, i, addrs), addrs[i]],
                    cwd=d.name,
                    stdout=logs[i],
                    stderr=subprocess.STDOUT,
                )
                for i, d in enumerate(dirs)
            ]
            time.sleep(3)
            dead = [i for i, p in enumerate(procs) if p.poll() is not None]
            if dead:
                raise RuntimeError(f"nodes {dead} exited before the run began")

            names = [f"f{n:04}" for n in range(files)]
            blob = b"x" * size
            for name in names:
                (PosixPath(dirs[0].name) / name).write_bytes(blob)

            peak_fd = 0
            converged = False
            deadline = time.monotonic() + budget
            while time.monotonic() < deadline:
                for p in procs:
                    peak_fd = max(peak_fd, fd_count(p.pid))
                if all(
                    (PosixPath(d.name) / n).is_file() for d in dirs for n in names
                ):
                    converged = True
                    settle = time.monotonic() + 4
                    while time.monotonic() < settle:
                        for p in procs:
                            peak_fd = max(peak_fd, fd_count(p.pid))
                        time.sleep(0.1)
                    break
                time.sleep(0.1)

            for p in procs:
                p.terminate()
            for p in procs:
                try:
                    p.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    p.kill()

            best_peers = 0
            best_torrents = 0
            for f in logs:
                f.flush()
                f.seek(0)
                pk, tk = peak_peers(f.read())
                best_peers = max(best_peers, pk)
                best_torrents = max(best_torrents, tk)

            return {
                "nodes": nodes,
                "files": files,
                "size": size,
                "converged": converged,
                "peak_fd": peak_fd,
                "peak_peers": best_peers,
                "peak_torrents": best_torrents,
                "product": files * (nodes - 1),
            }
        finally:
            for f in logs:
                f.close()
            for d in dirs:
                d.cleanup()


def main() -> None:
    soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    print(f"RLIMIT_NOFILE soft={soft}")
    print(
        "nodes files   size  product  peak_torrents  peak_peers  peak_fd  "
        "converged  peers/product"
    )
    cases = [
        (2, 200, 256 * 1024, 240),
        (5, 200, 256 * 1024, 300),
        (5, 1000, 16 * 1024, 300),
        (10, 200, 64 * 1024, 300),
    ]
    for nodes, files, size, budget in cases:
        r = run(nodes, files, size, budget)
        ratio = r["peak_peers"] / r["product"] if r["product"] else 0.0
        print(
            f"{r['nodes']:5} {r['files']:5} {r['size'] // 1024:5}K "
            f"{r['product']:8} {r['peak_torrents']:14} {r['peak_peers']:11} "
            f"{r['peak_fd']:8} {str(r['converged']):>10} {ratio:14.3f}"
        )
        sys.stdout.flush()


if __name__ == "__main__":
    main()
