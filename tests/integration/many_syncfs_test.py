import subprocess
import tempfile
import time
from pathlib import PosixPath

import pytest

from .wait import contents_match, many_node_deadline, poll_until


@pytest.fixture(scope="session")
def num_syncfs():
    return 10


@pytest.fixture(scope="function")
def tmp_dirs(num_syncfs: int):
    dirs = [
        tempfile.TemporaryDirectory(prefix=f"SYNCFS{i:02}") for i in range(num_syncfs)
    ]
    yield [d.name for d in dirs]
    for dir_ in dirs:
        dir_.cleanup()


@pytest.fixture(scope="function")
def addrs(num_syncfs):
    return [f"localhost:{5000 + n}" for n in range(num_syncfs)]


@pytest.fixture(scope="function")
def peers(num_syncfs, addrs):
    files = [
        tempfile.NamedTemporaryFile(mode="w", delete_on_close=True)
        for _ in range(num_syncfs)
    ]
    for i, f in enumerate(files):
        for addr in (a for j, a in enumerate(addrs) if i != j):
            f.write(f"tcp://{addr}\n")
        f.flush()

    yield [f.name for f in files]

    for f in files:
        f.close()


@pytest.fixture(scope="function")
def syncfs(peers, addrs, tmp_dirs):
    syncfss = [
        subprocess.Popen(["syncfs", peer, addr], cwd=tmp_dir)
        for peer, addr, tmp_dir in zip(peers, addrs, tmp_dirs)
    ]
    time.sleep(2)
    yield
    for p in syncfss:
        p.terminate()
    for p in syncfss:
        p.wait()


def test_one_syncf_sends_to_many_other(tmp_dirs, syncfs):
    """V18: ten nodes and a four byte change converge inside a second."""
    file_a = PosixPath(tmp_dirs[0]) / "file"
    with file_a.open(mode="w") as f:
        f.write("1234")

    assert poll_until(
        lambda: all(
            contents_match(PosixPath(dir_) / "file", "1234") for dir_ in tmp_dirs
        ),
        many_node_deadline,
    ), "the file did not reach every node"


@pytest.fixture(scope="function")
def files(tmp_dirs) -> list[PosixPath]:
    files = [PosixPath(d) / "file" for d in tmp_dirs]
    for file in files:
        with file.open(mode="w") as f:
            f.write("1234")
    return files


def test_one_syncf_deletes_many_other(files, syncfs):
    files[0].unlink()

    assert poll_until(
        lambda: not any(file.exists() for file in files),
        many_node_deadline,
    ), "the deletion did not reach every node"


def test_one_syncf_updates_many_other(files, syncfs):
    files[0].write_text("5678")

    assert poll_until(
        lambda: all(contents_match(file, "5678") for file in files),
        many_node_deadline,
    ), "the update did not reach every node"
