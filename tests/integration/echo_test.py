import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest

peer_a_addr = "localhost:5100"
peer_b_addr = "localhost:5101"

expected_sync_delay = 2


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
        self.process.terminate()
        self.log, _ = self.process.communicate(timeout=10)
        print(self.log)
        return self.log

    def creates(self) -> list[str]:
        return [line for line in self.log.splitlines() if "-> Create" in line]

    def removes(self) -> list[str]:
        return [line for line in self.log.splitlines() if "-> Remove" in line]


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
    if node.process.poll() is None:
        node.stop()


@pytest.fixture(scope="function")
def node_b(tmp_dir_b, peers_b) -> Generator[Node, Any, Any]:
    node = start(peers_b, peer_b_addr, tmp_dir_b)
    time.sleep(0.1)
    yield node
    if node.process.poll() is None:
        node.stop()


def test_receiver_does_not_republish(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """V36: the file libtorrent writes on the receiver must not travel back.
    Its content is identical, so the announcement is pure waste: a full
    re-hash inside the sync loop for an info hash every peer already has."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    time.sleep(expected_sync_delay)
    file_b = PosixPath(tmp_dir_b) / "file"
    assert file_b.exists()
    assert file_b.read_text() == "1234"

    node_a.stop()
    node_b.stop()

    assert len(node_a.creates()) == 1, "sender announced the file more than once"
    assert node_b.creates() == [], "receiver echoed the file it had just been sent"


def test_local_edit_after_receiving_is_published(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """Echo suppression must not swallow a genuine later edit of the same
    path on the receiving side."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    time.sleep(expected_sync_delay)
    file_b = PosixPath(tmp_dir_b) / "file"
    assert file_b.exists()

    file_b.write_text("5678")
    time.sleep(expected_sync_delay)
    assert file_a.read_text() == "5678"

    node_a.stop()
    node_b.stop()

    assert len(node_b.creates()) == 1, "receiver did not announce its own edit"


def test_receiver_does_not_republish_remove(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """V37: the deletion a peer asked for must not travel back. Every node
    would answer every deletion, and an echo arriving after the path was
    recreated deletes the new file."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    time.sleep(expected_sync_delay)
    file_b = PosixPath(tmp_dir_b) / "file"
    assert file_b.exists()

    file_a.unlink()
    time.sleep(expected_sync_delay)
    assert not file_b.exists()

    node_a.stop()
    node_b.stop()

    assert len(node_a.removes()) == 1, "sender announced the deletion more than once"
    assert node_b.removes() == [], "receiver echoed the deletion it was told to make"


def test_delete_after_suppressed_remove_is_published(
    node_a, node_b, tmp_dir_a, tmp_dir_b
):
    """The mark that suppresses one echo must be consumed by it, so the next
    genuine deletion of the same path still travels."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_b = PosixPath(tmp_dir_b) / "file"

    file_a.write_text("1234")
    time.sleep(expected_sync_delay)
    assert file_b.exists()

    file_a.unlink()
    time.sleep(expected_sync_delay)
    assert not file_b.exists()

    # Same path again, this time born on the receiving side.
    file_b.write_text("5678")
    time.sleep(expected_sync_delay)
    assert file_a.exists()

    file_b.unlink()
    time.sleep(expected_sync_delay)
    assert not file_a.exists()
