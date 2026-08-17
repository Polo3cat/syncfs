import re
import subprocess
import tempfile
import time
from collections.abc import Generator
from datetime import datetime
from pathlib import PosixPath
from typing import Any

import pytest

peer_a_addr = "localhost:5200"
peer_b_addr = "localhost:5201"
peer_c_addr = "localhost:5202"

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


log_time = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{6})")

# One row of the statistics table the daemon prints at debug every two seconds:
# a file name, a progress to two decimals, and the counters after it. The table
# is written as a single message with the rows embedded, so a row carries no log
# prefix of its own.
torrent_row = re.compile(r"^[^\t]*\t\t\d+\.\d\d\t\t\d+\t\t\d+\t\d+\t\S+$")


def stamp(line: str) -> datetime | None:
    """The moment a log line was written, or nothing for a line that carries
    no timestamp of its own."""
    found = log_time.match(line)
    if found is None:
        return None
    return datetime.strptime(found.group(1), "%Y-%m-%d %H:%M:%S.%f")


def last_stamp(lines: list[str]) -> datetime | None:
    stamps = [s for s in (stamp(line) for line in lines) if s is not None]
    return stamps[-1] if stamps else None


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

    def lines(self) -> list[str]:
        return self.log.splitlines()

    def states(self) -> list[str]:
        return [line for line in self.lines() if "-> State" in line]

    def digests(self) -> list[str]:
        return [line for line in self.lines() if "-> Digest" in line]

    def creates(self) -> list[str]:
        return [line for line in self.lines() if "-> Create" in line]

    def torrent_rows(self) -> list[str]:
        """The rows of the debug statistics table, one per torrent the session
        holds. The header carries no numbers, so only real torrents match, and
        a node holding none prints the header alone."""
        return [line for line in self.lines() if torrent_row.match(line)]


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


def wait_for(predicate, budget: float) -> bool:
    """Polls to a deadline. A control plane that drops messages is one nothing
    may block on, in a test any more than in the daemon."""
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.5)
    return predicate()


def test_v50_a_missed_remove_is_learned_from_a_digest(
    tmp_dir_a, tmp_dir_b, peers_a, peers_b
):
    """V50: a node that was away when a file was deleted has no way of
    learning it from the announcements, which are gone. It takes the deletion
    off a peer's digest instead. Without that it would hold the file for ever,
    mismatch the root hash every round, and ship a full digest every round to
    say so."""
    file_a = PosixPath(tmp_dir_a) / "file"
    file_b = PosixPath(tmp_dir_b) / "file"

    node_a = start(peers_a, peer_a_addr, tmp_dir_a)
    node_b = start(peers_b, peer_b_addr, tmp_dir_b)
    try:
        # A publisher drops whatever it sends before its subscribers have
        # finished connecting, and this test is about a deletion going missing,
        # not the creation before it.
        time.sleep(1)
        file_a.write_text("1234")
        assert wait_for(file_b.exists, 10), "the file never reached the peer"

        # Away for the deletion, and with the tombstones held in memory it
        # comes back knowing nothing about it.
        node_b.stop()
        file_a.unlink()
        time.sleep(1)
        node_b = start(peers_b, peer_b_addr, tmp_dir_b)

        converged = wait_for(
            lambda: not file_b.exists(), quiescent_state_delay + 30
        )

        # Both ends agree again, so there is nothing left to say: a full digest
        # every round for ever is exactly what adoption is here to stop.
        quiet_tail = 20
        time.sleep(quiet_tail)
    finally:
        node_a.stop()
        node_b.stop()

    assert converged, "the file a peer deleted while this node was away stayed"

    last_digest = last_stamp(node_b.digests())
    end = last_stamp(node_b.lines())
    assert last_digest is not None, "the two never compared themselves at all"
    assert end is not None
    assert (end - last_digest).total_seconds() >= quiet_tail / 2, (
        "digests never stopped: the root hash is not converging"
    )


# The control plane and the data plane are bounded separately on purpose. One
# deadline covering both is flaky by construction, and the fix for that
# flakiness is always to inflate the timeout until the announcement half stops
# being checked at all.
announcement_budget = 45
convergence_budget = 150

written_files = 12
deleted_files = 4


def peers_file(*addresses: str) -> str:
    """A peers file that outlives the call. It cannot live inside a node's own
    directory, since that directory is the thing being synchronized."""
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
        f.write("\n".join(f"tcp://{a}" for a in addresses))
        return f.name


@pytest.fixture(scope="function")
def tmp_dir_c():
    with tempfile.TemporaryDirectory() as d:
        yield d


def written() -> dict[str, str]:
    """The files a writer lays down: some at the sync root, some a directory
    deep, since a torrent for a file one directory down loses that directory
    on load and has to have it put back."""
    return {
        f"{n % 3}/f{n:02}" if n % 2 else f"f{n:02}": f"content of file {n}\n"
        for n in range(written_files)
    }


def test_v41_v52_late_joiner_is_repaired(tmp_dir_a, tmp_dir_b, tmp_dir_c):
    """V41 and V52: a node that starts after the writing is over receives no
    announcements at all, which is what every dropped announcement looks like
    from the inside. It compares itself against its peers and is handed back
    what it never heard, files and deletions alike."""
    peers_a = peers_file(peer_b_addr, peer_c_addr)
    peers_b = peers_file(peer_a_addr, peer_c_addr)
    peers_c = peers_file(peer_a_addr, peer_b_addr)

    contents = written()
    kept = dict(list(contents.items())[deleted_files:])
    dropped = dict(list(contents.items())[:deleted_files])

    node_a = start(peers_a, peer_a_addr, tmp_dir_a)
    node_b = start(peers_b, peer_b_addr, tmp_dir_b)
    node_c = None
    try:
        time.sleep(1)
        for name, text in contents.items():
            path = PosixPath(tmp_dir_a) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

        assert wait_for(
            lambda: all(
                (PosixPath(tmp_dir_b) / name).is_file() for name in contents
            ),
            60,
        ), "the two writers never agreed in the first place"

        for name in dropped:
            (PosixPath(tmp_dir_a) / name).unlink()
        assert wait_for(
            lambda: not any(
                (PosixPath(tmp_dir_b) / name).exists() for name in dropped
            ),
            30,
        ), "the deletions never reached the second writer"

        # Everything above happened before this node existed, so nothing of it
        # was ever addressed to it.
        node_c = start(peers_c, peer_c_addr, tmp_dir_c)

        converged = wait_for(
            lambda: all(
                (PosixPath(tmp_dir_c) / name).is_file()
                and (PosixPath(tmp_dir_c) / name).read_text() == text
                for name, text in kept.items()
            ),
            convergence_budget,
        )
        resurrected = [
            name for name in dropped if (PosixPath(tmp_dir_c) / name).exists()
        ]
    finally:
        node_a.stop()
        node_b.stop()
        if node_c is not None:
            node_c.stop()

    assert converged, "the late joiner never got the files it never heard about"
    assert not resurrected, (
        f"deleted files came back on the late joiner: {resurrected}"
    )

    # And the announcements themselves, on their own deadline. Every file the
    # late joiner holds was announced to it by a holder, since it heard none of
    # the originals.
    lines = node_c.lines()
    added = [line for line in lines if "Added nodes for" in line]
    assert len(added) >= len(kept), (
        f"only {len(added)} announcements reached the late joiner, "
        f"{len(kept)} were missing"
    )
    start_at = next(s for s in (stamp(line) for line in lines) if s is not None)
    repaired_by = last_stamp(added[: len(kept)])
    assert repaired_by is not None
    assert (repaired_by - start_at).total_seconds() <= announcement_budget, (
        "the announcements were repaired, but not inside the control plane's "
        "own deadline"
    )


# A block of its own. This test stops and starts nodes on the same addresses,
# so it cannot share ports with anything else in the module.
restart_a_addr = "localhost:5210"
restart_b_addr = "localhost:5211"
restart_c_addr = "localhost:5212"

restarted_files = 8

# Two ticks of the statistics table, so that a torrent added just before the
# stop has been printed at least once. Without it the assertion would be about
# when the daemon last spoke rather than about what it was holding.
stats_settle = 5


def restarted_written() -> dict[str, str]:
    """What the two writers lay down between them, some at the sync root and
    some a directory deep."""
    return {
        f"{n % 2}/g{n:02}" if n % 2 else f"g{n:02}": f"content of file {n}\n"
        for n in range(restarted_files)
    }


def write_all(where: str, contents: dict[str, str]) -> None:
    for name, text in contents.items():
        path = PosixPath(where) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def test_v55_v60_a_restarted_swarm_repairs_a_late_joiner(
    tmp_dir_a, tmp_dir_b, tmp_dir_c
):
    """V55 and V60: the announcement cache fills only where a create is
    applied, and a node coming back up applies none for the files it already
    has. So every holder in a swarm that has been restarted holds its whole
    tree and remembers not one announcement, and until one of them rebuilds an
    announcement off disk it can repair nobody: the gap re-arms every round and
    never closes.

    T50 cannot see this. Its writers create the files inside the test, so every
    holder's cache is warm by construction, which is exactly the case a restart
    is not.

    And the rebuilt announcement goes out broadcast rather than at the node
    that asked. A restarted node adds no torrent for what the listing already
    found, so it seeds nothing until an announcement for the path reaches it;
    a repair aimed only at the asker would leave the swarm one node wide.
    """
    peers_a = peers_file(restart_b_addr, restart_c_addr)
    peers_b = peers_file(restart_a_addr, restart_c_addr)
    peers_c = peers_file(restart_a_addr, restart_b_addr)

    contents = restarted_written()
    half = len(contents) // 2
    first = dict(list(contents.items())[:half])
    second = dict(list(contents.items())[half:])

    node_a = start(peers_a, restart_a_addr, tmp_dir_a)
    node_b = start(peers_b, restart_b_addr, tmp_dir_b)
    node_c = None
    try:
        # A publisher drops whatever it sends before its subscribers have
        # finished connecting, and the first exchange is not what is on trial.
        time.sleep(1)
        write_all(tmp_dir_a, first)
        write_all(tmp_dir_b, second)

        assert wait_for(
            lambda: all(
                (PosixPath(tmp_dir_a) / name).is_file()
                and (PosixPath(tmp_dir_b) / name).is_file()
                for name in contents
            ),
            60,
        ), "the two writers never agreed in the first place"

        # Down and back up on the same directories, which is what a container
        # restart is. Each comes back holding every file, with no torrent in
        # its session and no announcement in its cache.
        node_a.stop()
        node_b.stop()
        node_a = start(peers_a, restart_a_addr, tmp_dir_a)
        node_b = start(peers_b, restart_b_addr, tmp_dir_b)
        time.sleep(1)

        # None of this was ever addressed at this node, and no holder has an
        # announcement left to hand it.
        node_c = start(peers_c, restart_c_addr, tmp_dir_c)

        converged = wait_for(
            lambda: all(
                (PosixPath(tmp_dir_c) / name).is_file()
                and (PosixPath(tmp_dir_c) / name).read_text() == text
                for name, text in contents.items()
            ),
            convergence_budget,
        )
        time.sleep(stats_settle)
    finally:
        node_a.stop()
        node_b.stop()
        if node_c is not None:
            node_c.stop()

    assert converged, (
        "the late joiner never got the files: every holder had restarted, so "
        "not one of them had a cached announcement to repair it with"
    )

    # V60: broadcast, so both restarted holders load the rebuilt torrent and
    # seed again, not only the one the late joiner happened to ask.
    for name, node in (("A", node_a), ("B", node_b)):
        assert node.torrent_rows(), (
            f"restarted node {name} held no torrent at all, so it was seeding "
            "nothing and the late joiner pulled from a swarm one node wide"
        )
