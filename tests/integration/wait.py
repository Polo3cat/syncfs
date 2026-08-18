"""Waiting on convergence rather than on the clock.

A fixed sleep is wrong in both directions. It is too short on a loaded host,
which is a flake, and too long on an idle one, which is the suite spending
minutes waiting for something that already happened. The invariants name
deadlines, not durations: V18 puts ten nodes and a four byte change at one
second (measured 0.051 s) and V19 puts two nodes at five seconds. So the
deadline is the assertion and the polling interval only decides how much of it
is wasted after the fact.

A negative assertion cannot be polled: there is no moment at which "nothing
happened" becomes true, so the tests that assert an echo never travelled keep a
fixed window of their own.
"""

import time
from collections.abc import Callable
from pathlib import PosixPath

# V19: two nodes, one small change.
two_node_deadline = 5
# V18: ten nodes, one small change.
many_node_deadline = 1

# How long the poll leaves between attempts. The daemon's idle loop iteration is
# at most 100 ms (V26), so this is well inside it.
poll_interval = 0.05


def poll_until(
    predicate: Callable[[], bool],
    deadline: float,
    interval: float = poll_interval,
) -> bool:
    """True as soon as the predicate holds, False when the deadline passes."""
    end = time.monotonic() + deadline
    while True:
        if predicate():
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(interval)


def contents_match(file: PosixPath, expected_content: str) -> bool:
    """Whether the file is there and holds exactly that content.

    Reads through a missing file rather than raising: during a poll the path may
    not exist yet, or may be mid-write, and both are just "not yet".
    """
    try:
        return file.read_text() == expected_content
    except OSError:
        return False
