import subprocess
import tempfile
import time
from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest

from .wait import contents_match, poll_until, two_node_deadline


@pytest.fixture(scope="function")
def tmp_dir_a():
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture(scope="function")
def tmp_dir_b():
    with tempfile.TemporaryDirectory() as d:
        yield d


peer_a_addr = "localhost:5000"
peer_b_addr = "localhost:5001"


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


@pytest.fixture(scope="function")
def syncfs_a(tmp_dir_a, peers_a):
    with subprocess.Popen(["syncfs", peers_a, peer_a_addr], cwd=tmp_dir_a) as pa:
        time.sleep(0.1)
        yield
        pa.terminate()
        if pa.stdout:
            print(pa.stdout.read())


@pytest.fixture(scope="function")
def syncfs_b(tmp_dir_b, peers_b):
    with subprocess.Popen(["syncfs", peers_b, peer_b_addr], cwd=tmp_dir_b) as pb:
        time.sleep(0.1)
        yield
        pb.terminate()
        if pb.stdout:
            print(pb.stdout.read())


def test_one_syncf_sends_to_another(syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b):
    """V19: two nodes and a four byte change converge inside five seconds."""
    file_a = PosixPath(tmp_dir_a) / "file"
    with file_a.open(mode="w") as f:
        f.write("1234")

    file_b = PosixPath(tmp_dir_b) / "file"
    assert poll_until(
        lambda: contents_match(file_b, "1234"), two_node_deadline
    ), "the file never arrived"


@pytest.fixture(scope="function")
def file_a(tmp_dir_a) -> PosixPath:
    f = PosixPath(tmp_dir_a) / "file"
    with f.open(mode="w") as _f:
        _f.write("1234")
    return f


@pytest.fixture(scope="function")
def file_b(tmp_dir_b) -> PosixPath:
    f = PosixPath(tmp_dir_b) / "file"
    with f.open(mode="w") as _f:
        _f.write("1234")
    return f


def test_one_syncf_deletes_another_file(
    file_a, file_b, syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b
):
    file_a.unlink()

    assert poll_until(
        lambda: not file_b.exists(), two_node_deadline
    ), "the deletion never arrived"


def test_one_syncf_updates_another_file(
    file_a, file_b, syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b
):
    file_a.write_text("5678")

    assert poll_until(
        lambda: contents_match(file_b, "5678"), two_node_deadline
    ), "the update never arrived"
