import signal
import subprocess
import tempfile
import time
from collections.abc import Generator
from typing import Any

import pytest

# Kept away from the 5000-5009 range the other integration tests bind.
listen_addr = "127.0.0.1:5120"
peer_addr = "127.0.0.1:5121"

# One idle loop iteration polls inotify for 50 ms and ZMQ for 100 ms (V26),
# so a stop request is picked up well inside a second.
shutdown_budget = 5


@pytest.fixture(scope="function")
def tmp_dir() -> Generator[str, Any, Any]:
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def peers() -> Generator[str, Any, Any]:
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.write(f"tcp://{peer_addr}\n")
        f.flush()
        f.close()
        yield f.name


@pytest.mark.parametrize("stop_signal", [signal.SIGTERM, signal.SIGINT])
def test_v33_stop_signal_exits_success(stop_signal, peers, tmp_dir):
    """SIGTERM and SIGINT must end the sync loop with EXIT_SUCCESS.

    As PID 1 of a container neither signal has a default disposition, so an
    unhandled one leaves the process only killable.
    """
    with subprocess.Popen(["syncfs", peers, listen_addr], cwd=tmp_dir) as p:
        # Let the sync loop start, so the signal lands in a poll.
        time.sleep(1)
        p.send_signal(stop_signal)
        assert p.wait(timeout=shutdown_budget) == 0
