import re
import signal
import subprocess
import tempfile
import time
from collections.abc import Generator
from datetime import datetime
from pathlib import PosixPath
from typing import Any

import pytest

# Kept away from the 5000-5009 range the other integration tests bind.
listen_addr = "127.0.0.1:5120"
peer_addr = "127.0.0.1:5121"

# The pair the teardown test needs, kept clear of both of the above.
writer_addr = "127.0.0.1:5122"
reader_addr = "127.0.0.1:5123"

# One idle loop iteration polls inotify for 50 ms and ZMQ for 100 ms (V26),
# so a stop request is picked up well inside a second.
shutdown_budget = 5

# Long enough that a stop which is merely slow is still measured. A budget used
# as a timeout reports "hung" and never "slow" (V54), so a stop of 4.9 seconds
# would pass in silence until load turned it into 10.1 and wedged the suite.
# Only a stop that never happens at all reaches this.
hung = 60

received_files = 12


log_time = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{6})")


def stamp(line: str) -> datetime | None:
    """The moment a log line was written, or nothing for a line that carries
    no timestamp of its own."""
    found = log_time.match(line)
    if found is None:
        return None
    return datetime.strptime(found.group(1), "%Y-%m-%d %H:%M:%S.%f")


def stopping_at(log: str) -> datetime | None:
    """When the daemon said it was stopping. It is the last line it writes, so
    everything after it is teardown and nothing else."""
    for line in log.splitlines():
        if "Stopping." in line:
            return stamp(line)
    return None


def start(peers: str, addr: str, cwd: str) -> subprocess.Popen:
    return subprocess.Popen(
        ["syncfs", peers, addr],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def stop_and_measure(p: subprocess.Popen, stop_signal) -> tuple[int, float, float]:
    """Signals the daemon and reports what the stop cost: the whole of it, and
    the part after it had nothing left to do but tear the session down."""
    p.send_signal(stop_signal)
    asked = time.monotonic()
    log = p.communicate(timeout=hung)[0]
    whole = time.monotonic() - asked
    ended = datetime.now()

    said = stopping_at(log)
    assert said is not None, (
        "the daemon exited without ever saying it was stopping, so there is no "
        f"teardown to measure. Log tail: {log.splitlines()[-5:]}"
    )
    return p.returncode, whole, (ended - said).total_seconds()


def peers_file(*addresses: str) -> str:
    """A peers file that outlives the call. It cannot live inside a node's own
    directory, since that directory is the thing being synchronized."""
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
        f.write("\n".join(f"tcp://{a}" for a in addresses))
        return f.name


@pytest.fixture(scope="function")
def tmp_dir() -> Generator[str, Any, Any]:
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def tmp_dir_b() -> Generator[str, Any, Any]:
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
    """SIGTERM and SIGINT must end the sync loop with EXIT_SUCCESS, inside a
    budget that is measured rather than inferred.

    As PID 1 of a container neither signal has a default disposition, so an
    unhandled one leaves the process only killable. And the stop is a number:
    "Stopping." is the last line the daemon writes, so the gap between it and
    the process going away is the session teardown and nothing else (V54).
    """
    with start(peers, listen_addr, tmp_dir) as p:
        # Let the sync loop start, so the signal lands in a poll.
        time.sleep(1)
        code, whole, teardown = stop_and_measure(p, stop_signal)

    assert code == 0
    assert teardown <= shutdown_budget, (
        f"the daemon took {teardown:.2f}s to tear its session down after "
        f"saying it had stopped, against a {shutdown_budget}s budget"
    )
    assert whole <= shutdown_budget, (
        f"the stop took {whole:.2f}s from the signal, against a "
        f"{shutdown_budget}s budget ({teardown:.2f}s of it teardown)"
    )


def test_v54_teardown_is_bounded_after_receiving(tmp_dir, tmp_dir_b):
    """A node that has taken files off the wire must stop inside the budget too.

    This is the case B15 measured at 2.72s while a node holding nothing stopped
    in 0.11s: every applied create forces a DHT announce, and the teardown used
    to wait out whatever was still in flight. A single idle node cannot reach
    this at all, which is why V33's own test never saw it.
    """
    writer = start(peers_file(reader_addr), writer_addr, tmp_dir)
    reader = start(peers_file(writer_addr), reader_addr, tmp_dir_b)
    try:
        # Long enough for both subscriptions to settle, since a publisher drops
        # whatever it sends before a subscriber has finished connecting.
        time.sleep(3)
        names = [f"f{n:02}" for n in range(received_files)]
        for name in names:
            (PosixPath(tmp_dir) / name).write_text(f"content of {name}\n" * 4)

        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if all((PosixPath(tmp_dir_b) / name).is_file() for name in names):
                break
            time.sleep(0.2)
        assert all((PosixPath(tmp_dir_b) / name).is_file() for name in names), (
            "the receiver never got the files, so its teardown says nothing"
        )

        code, whole, teardown = stop_and_measure(reader, signal.SIGTERM)
    finally:
        if writer.poll() is None:
            writer.terminate()
            writer.communicate(timeout=hung)
        if reader.poll() is None:
            reader.kill()

    assert code == 0
    assert teardown <= shutdown_budget, (
        f"a receiver holding {received_files} files took {teardown:.2f}s to "
        f"tear its session down, against a {shutdown_budget}s budget"
    )
    assert whole <= shutdown_budget, (
        f"the stop took {whole:.2f}s from the signal, against a "
        f"{shutdown_budget}s budget ({teardown:.2f}s of it teardown)"
    )
