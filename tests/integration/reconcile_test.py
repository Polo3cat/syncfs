import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest

peer_a_addr = "localhost:5200"
peer_b_addr = "localhost:5201"

# Quiescence window plus one reconcile period, which is the earliest a node
# left alone may publish its root hash.
quiescent_state_delay = 10 + 5
# The ceiling, for a node that is never left alone.
state_ceiling = 60


@pytest.fixture(scope="function")
def tmp_dir_a():
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def tmp_dir_b():
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def peers_a() -> Generator[str, Any, Any]:
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.write(f"tcp://{peer_b_addr}")
        f.flush()
        f.close()
        yield f.name


@pytest.fixture(scope="function")
def peers_b() -> Generator[str, Any, Any]:
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.write(f"tcp://{peer_a_addr}")
        f.flush()
        f.close()
        yield f.name


class Node:
    """A syncfs process whose log is captured, so a test can assert on what
    the daemon decided to publish."""

    def __init__(self, process: subprocess.Popen):
        self.process = process
        self.log = ""

    def stop(self) -> str:
        if self.process.poll() is None:
            self.process.terminate()
        self.log, _ = self.process.communicate(timeout=10)
        return self.log

    def states(self) -> list[str]:
        return [line for line in self.log.splitlines() if "-> State" in line]

    def creates(self) -> list[str]:
        return [line for line in self.log.splitlines() if "-> Create" in line]


def start(peers: str, addr: str, cwd: str) -> Node:
    return Node(
        subprocess.Popen(
            ["syncfs", peers, addr],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    )


@pytest.fixture(scope="function")
def node_a(tmp_dir_a, peers_a) -> Generator[Node, Any, Any]:
    node = start(peers_a, peer_a_addr, tmp_dir_a)
    time.sleep(0.1)
    yield node
    node.stop()


def test_v47_quiet_node_publishes_its_root_hash(node_a, tmp_dir_a):
    """V47: with the directory quiet and every torrent settled, the root hash
    goes out once a period. Thirty two bytes is the whole idle cost of the
    repair, and nothing repairs anything until a peer has seen one."""
    (PosixPath(tmp_dir_a) / "file").write_text("1234")

    time.sleep(quiescent_state_delay + 5)
    node_a.stop()

    assert node_a.states(), "a quiet node never published its root hash"


def test_v47_ceiling_fires_under_continuous_write(node_a, tmp_dir_a):
    """V47: the gate holds the root hash back while the directory is being
    written to, and the ceiling publishes it anyway once a minute. Without the
    ceiling a workload writing every nine seconds keeps the node busy for ever
    and the repair never runs in exactly the regime that loses announcements;
    without the gate a receiver publishes the hash of a half filled tree."""
    written = 0
    deadline = time.monotonic() + state_ceiling + 10
    while time.monotonic() < deadline:
        (PosixPath(tmp_dir_a) / f"f{written:03}").write_text("x" * 16)
        written += 1
        time.sleep(2)

    node_a.stop()

    states = node_a.states()
    assert states, "the ceiling never fired, so a lost announcement stays lost"
    # Ungated, a node writing for seventy seconds would publish a dozen times.
    assert len(states) <= 2, (
        f"the gate is not gating: {len(states)} root hashes while writing"
    )
