import socket
import subprocess
import tempfile
from collections.abc import Generator
from typing import Any

import pytest

# Kept away from the 5000-5009 range the other integration tests bind.
listen_addr = "127.0.0.1:5100"
peer_addr = "127.0.0.1:5101"


@pytest.fixture(scope="function")
def tmp_dir():
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def peers() -> Generator[str, Any, Any]:
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.write(f"tcp://{peer_addr}\n")
        f.flush()
        f.close()
        yield f.name


@pytest.fixture(scope="function")
def occupied_port() -> Generator[None, Any, Any]:
    host, port = listen_addr.split(":")
    with socket.create_server((host, int(port))) as s:
        s.listen()
        yield


def run_until_it_exits(
    addr: str, peers_file: str, cwd: str
) -> subprocess.CompletedProcess:
    """An address the daemon must refuse before the sync loop, so this waits for
    it to give up rather than stopping it."""
    return subprocess.run(
        ["syncfs", peers_file, addr],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )


@pytest.mark.parametrize(
    "wildcard", ["0.0.0.0:5555", "[::]:5555", "*:5555", ":5555"]
)
def test_v64_wildcard_listen_address_exits_failure(wildcard, peers, tmp_dir):
    """V64: the listen address is this node's identity on the control plane and
    not only what it binds. state carries it and the digest subscription is
    built from the same string, so two nodes spelling it the same way each read
    the other's state as their own, draw no partner and never send a digest:
    the whole repair plane is gone while create and remove keep working, in no
    log line and with no crash. A wildcard guarantees that collision on every
    node at once, and it binds perfectly happily.
    """
    finished = run_until_it_exits(wildcard, peers, tmp_dir)

    assert finished.returncode == 1, finished.stdout + finished.stderr
    assert "listen address" in finished.stdout, finished.stdout


def test_v28_fatal_zmq_error_exits_failure(occupied_port, peers, tmp_dir):
    """A fatal zmq::error_t must leave main with EXIT_FAILURE.

    The listen address is already bound by another process, so the PUB
    socket bind raises zmq::error_t (EADDRINUSE).
    """
    completed = subprocess.run(
        ["syncfs", peers, listen_addr],
        cwd=tmp_dir,
        timeout=10,
        check=False,
    )

    assert completed.returncode == 1
