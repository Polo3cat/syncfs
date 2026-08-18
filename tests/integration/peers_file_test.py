import os
import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest

# Kept away from the 5000-5009 range the other integration tests bind.
listen_addr = "127.0.0.1:5110"
peer_addr = "127.0.0.1:5111"


@pytest.fixture(scope="function")
def tmp_dir() -> Generator[str, Any, Any]:
    with tempfile.TemporaryDirectory() as d:
        # The daemon runs as an unprivileged user and syncs its own cwd.
        os.chmod(d, 0o777)
        yield d


@pytest.fixture(scope="function")
def read_only_peers() -> Generator[str, Any, Any]:
    """A peers file the daemon may read but never write."""
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.write(f"tcp://{peer_addr}\n")
        f.flush()
        f.close()
        os.chmod(f.name, 0o444)
        yield f.name


def test_v32_read_only_peers_file_is_parsed(read_only_peers, tmp_dir):
    """A peers file without write permission must still yield its peers.

    Opening the file read-write would fail here and leave the peer list
    silently empty, so the daemon would publish to nobody.
    """
    if os.geteuid() != 0:
        pytest.skip("dropping privileges to read-only the peers file needs root")

    with subprocess.Popen(
        ["syncfs", read_only_peers, listen_addr],
        cwd=tmp_dir,
        stdout=subprocess.PIPE,
        text=True,
        user="nobody",
        group="nobody",
    ) as p:
        # The peer list is read before the sync loop starts.
        time.sleep(1)
        p.terminate()
        out, _ = p.communicate(timeout=10)

    assert f"Subscribed to tcp://{peer_addr}" in out


def run_until_it_exits(peers_file: str, cwd: str) -> subprocess.CompletedProcess:
    """The daemon is expected to give up before the sync loop, so this waits for
    it rather than stopping it."""
    return subprocess.run(
        ["syncfs", peers_file, listen_addr],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )


def test_v2_empty_peers_file_exits_failure(tmp_dir):
    """V2: a daemon with no peers has nothing to synchronize with and has to say
    so. The check was an assert, which NDEBUG removes: the release build bound
    its socket, subscribed to nobody and reported success (B5).

    Exit code one rather than the negative six an aborted assert leaves is what
    tells the two apart, since these tests run a build that still has asserts in
    it.
    """
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.close()
        finished = run_until_it_exits(f.name, tmp_dir)

    assert finished.returncode == 1, finished.stdout + finished.stderr
    assert "No peers" in finished.stdout, finished.stdout


def test_v27_missing_peers_file_exits_failure(tmp_dir):
    """A peers file that cannot be read is an error, not an empty peer list."""
    finished = run_until_it_exits(str(PosixPath(tmp_dir) / "not_here"), tmp_dir)

    assert finished.returncode == 1, finished.stdout + finished.stderr
    assert "Cannot read peers file" in finished.stdout, finished.stdout
