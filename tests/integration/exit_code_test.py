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
