import subprocess
import tempfile
import time

from collections.abc import Generator
from pathlib import PosixPath
from typing import Any

import pytest


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
    with tempfile.NamedTemporaryFile(mode="w") as f:
        f.write("tcp://localhost:5000")
        f.flush()
        yield f.name


@pytest.fixture(scope="function")
def peers_b() -> Generator[str, Any, Any]:
    with tempfile.NamedTemporaryFile(mode="w", delete_on_close=False) as f:
        f.close()
        yield f.name


@pytest.fixture(scope="function")
def syncfs_a(tmp_dir_a, peers_a):
    with subprocess.Popen(["syncfs", peers_a, "localhost:5001"], cwd=tmp_dir_a) as pa:
        time.sleep(0.1)
        yield
        pa.terminate()


@pytest.fixture(scope="function")
def syncfs_b(tmp_dir_b, peers_b):
    with subprocess.Popen(["syncfs", peers_b, "localhost:5000"], cwd=tmp_dir_b) as pb:
        time.sleep(0.1)
        yield
        pb.terminate()


def test_syncf(syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b):
    file_a = PosixPath(tmp_dir_a) / "file"
    with file_a.open(mode="w") as f:
        f.write("1234")

    time.sleep(0.5)
    file_b = PosixPath(tmp_dir_b) / "file"
    assert file_b.exists()
    with file_b.open(mode="r") as f:
        content = f.read()
        assert content == "1234"
