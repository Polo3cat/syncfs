import re
import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest

from .wait import contents_match, poll_until, two_node_deadline

peer_a_addr = "localhost:5100"
peer_b_addr = "localhost:5101"

# An echo would follow the receiving node's own inotify event, one loop
# iteration behind the write libtorrent made (V26). "Nothing was published" has
# no moment at which it becomes true, so it cannot be polled for: this is the
# window an echo is given to show up in before the daemon is stopped and its log
# read.
echo_window = 2


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

    file_b = PosixPath(tmp_dir_b) / "file"
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"
    time.sleep(echo_window)

    node_a.stop()
    node_b.stop()

    assert len(node_a.creates()) == 1, "sender announced the file more than once"
    assert node_b.creates() == [], "receiver echoed the file it had just been sent"


def test_receiver_stamps_origin_mtime(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """V44: libtorrent writes the file with the receiver's clock, so without
    the origin time being put back the copy is newer than the original on
    every node it reaches and ordering has nothing left to order by."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    file_b = PosixPath(tmp_dir_b) / "file"
    # The stamp is put back after the write and before the snapshot that decides
    # the file is not worth republishing, so arrival alone is not the condition.
    assert poll_until(
        lambda: file_b.exists()
        and file_b.stat().st_mtime_ns == file_a.stat().st_mtime_ns,
        two_node_deadline,
    ), "the copy never carried the origin timestamp"

    assert file_b.stat().st_mtime_ns == file_a.stat().st_mtime_ns


def test_local_edit_after_receiving_is_published(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """Echo suppression must not swallow a genuine later edit of the same
    path on the receiving side."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    file_b = PosixPath(tmp_dir_b) / "file"
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"

    file_b.write_text("5678")
    edited_at = file_b.stat().st_mtime_ns
    assert poll_until(
        lambda: contents_match(file_a, "5678"), two_node_deadline
    ), "the edit made on the receiving side never travelled back"
    # An edit that lands while the origin stamp is still pending used to have its
    # timestamp rewritten backwards to the sender's, which then read as
    # libtorrent's own write and suppressed the edit for ever (B17).
    assert file_b.stat().st_mtime_ns >= edited_at, (
        "the origin stamp rewrote the local edit's timestamp backwards"
    )
    # The count below is an upper bound, so the second announcement it forbids
    # needs its window to appear in.
    time.sleep(echo_window)

    node_a.stop()
    node_b.stop()

    assert len(node_b.creates()) == 1, "receiver did not announce its own edit"


def test_v17_peer_and_dht_alerts_reach_the_log(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """R2, R3: the default alert mask hides the peer and DHT categories, and both
    of the swarm failures that cost the most were of exactly that shape - a
    seeder holding zero connections (B1) and two nodes whose active sets never
    named the same torrent (B10). Neither leaves a line behind under the default
    mask, so triage starts by rebuilding the daemon.

    A sync that moved bytes found its peer and answered a DHT lookup to do it, so
    both categories have something to say by the time the file has arrived.
    """
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    file_b = PosixPath(tmp_dir_b) / "file"
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"

    # Either side may be the one that dialled out, so both logs are read: what
    # is asserted is that these categories reach a log at all, not which node
    # happened to open the connection.
    log = node_a.stop() + "\n" + node_b.stop()

    # Matched on what a connect category alert says rather than on the word
    # "peer": the statistics table has a Peers column, so a bare match on the
    # word passes with the alert mask exactly as it was.
    assert [
        line
        for line in log.splitlines()
        if re.search(r"connection to peer|disconnecting", line, re.I)
    ], "no connect category alert in either log"
    assert [line for line in log.splitlines() if re.search(r"\bdht\b", line, re.I)], (
        "no DHT category alert in either log"
    )


def test_delete_during_transfer_does_not_kill_the_daemon(
    node_a, node_b, tmp_dir_a, tmp_dir_b
):
    """V62: a deletion can overtake the transfer of the file it deletes. The
    receiver has the bytes on disk and the flush alert for that torrent still
    queued when the remove drops the torrent (V38), and the handle the alert
    carries then names nothing: every call on it throws out of the sync loop and
    takes the process with it.

    The window is one loop iteration wide, so one create and delete pair enters
    it about half the time. Several pairs in a row make missing it unlikely,
    while a daemon that survives all of them is not surviving by luck.
    """
    file_a = PosixPath(tmp_dir_a) / "file"
    file_b = PosixPath(tmp_dir_b) / "file"

    for attempt in range(5):
        content = f"round {attempt}"
        file_a.write_text(content)
        assert poll_until(
            lambda: contents_match(file_b, content), two_node_deadline
        ), f"the file never arrived on attempt {attempt}"

        file_a.unlink()
        assert poll_until(
            lambda: not file_b.exists(), two_node_deadline
        ), f"the deletion never arrived on attempt {attempt}"

        # The throw does not land with the deletion. The flush of the torrent the
        # deletion dropped comes back afterwards, so the window opens once the
        # file is already gone.
        time.sleep(1)

        assert node_b.process.poll() is None, (
            f"receiver died on attempt {attempt}, on a deletion that overtook "
            "its own transfer"
        )
        assert node_a.process.poll() is None, f"sender died on attempt {attempt}"


def test_nested_file_does_not_kill_the_daemon(node_a, node_b, tmp_dir_a):
    """V39: a file in a subdirectory used to throw out of the sync loop and
    take the process down with it, because the hashes were read relative to
    the file's own parent rather than the sync root."""
    nested = PosixPath(tmp_dir_a) / "a" / "f.txt"
    nested.parent.mkdir()
    nested.write_text("1234")

    # A window for the throw this test exists to catch, not a wait for
    # convergence: what is asserted is that nothing happened to the process.
    time.sleep(echo_window)
    assert node_a.process.poll() is None, "sender died on a file in a subdirectory"

    node_a.stop()
    assert node_a.creates() != [], "sender never announced the nested file"


def test_nested_file_at_depth_one_syncs(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """V25: a v2-only torrent holding one file under one directory drops that
    directory on load, so the file used to arrive at the sync root instead of
    inside it. The announced path carries the directory the load lost."""
    nested = PosixPath(tmp_dir_a) / "a" / "f.txt"
    nested.parent.mkdir()
    nested.write_text("1234")

    assert poll_until(
        lambda: contents_match(PosixPath(tmp_dir_b) / "a" / "f.txt", "1234"),
        two_node_deadline,
    ), "the nested file never arrived where it was announced"
    assert not (PosixPath(tmp_dir_b) / "f.txt").exists(), (
        "the file lost its directory on the way"
    )


def test_nested_file_at_depth_two_syncs(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """At depth two the torrent carries the whole path itself, so this holds
    with or without the save path derived from the announced one."""
    nested = PosixPath(tmp_dir_a) / "a" / "b" / "f.txt"
    nested.parent.mkdir(parents=True)
    nested.write_text("1234")

    assert poll_until(
        lambda: contents_match(PosixPath(tmp_dir_b) / "a" / "b" / "f.txt", "1234"),
        two_node_deadline,
    ), "the nested file never arrived"


def test_receiver_does_not_republish_remove(node_a, node_b, tmp_dir_a, tmp_dir_b):
    """V37: the deletion a peer asked for must not travel back. Every node
    would answer every deletion, and an echo arriving after the path was
    recreated deletes the new file."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_a.write_text("1234")

    file_b = PosixPath(tmp_dir_b) / "file"
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"

    file_a.unlink()
    assert poll_until(
        lambda: not file_b.exists(), two_node_deadline
    ), "the deletion never arrived"
    time.sleep(echo_window)

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
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"

    file_a.unlink()
    assert poll_until(
        lambda: not file_b.exists(), two_node_deadline
    ), "the deletion never arrived"

    # Same path again, this time born on the receiving side.
    file_b.write_text("5678")
    assert poll_until(
        lambda: contents_match(file_a, "5678"), two_node_deadline
    ), "the recreated file never travelled back"

    file_b.unlink()
    assert poll_until(
        lambda: not file_a.exists(), two_node_deadline
    ), "the second deletion was suppressed with the first"
