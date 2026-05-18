import subprocess
import tempfile
import time

from pathlib import PosixPath

import pytest


@pytest.fixture(scope="session")
def num_syncfs():
    return 10


@pytest.fixture(scope="function")
def tmp_dirs(num_syncfs: int):
    dirs = [tempfile.TemporaryDirectory(prefix=f"SYNCFS{i:02}") for i in range(num_syncfs)]
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


@pytest.fixture(scope="function", autouse=True)
def syncfs(peers, addrs, tmp_dirs):
    syncfss = [
        subprocess.Popen(["syncfs", peer, addr], cwd=tmp_dir)
        for peer, addr, tmp_dir in zip(peers, addrs, tmp_dirs)
    ]
    time.sleep(0.1)
    yield
    for p in syncfss:
        p.terminate()
    for p in syncfss:
        p.wait()

expected_sync_delay = 3


def test_one_syncf_sends_to_many_other(tmp_dirs):
    file_a = PosixPath(tmp_dirs[0]) / "file"
    with file_a.open(mode="w") as f:
        f.write("1234")

    time.sleep(expected_sync_delay)

    for dir_ in tmp_dirs:
        file = PosixPath(dir_) / "file"
        assert file.exists()
        with file.open(mode="r") as f:
            content = f.read()
            assert content == "1234"


# @pytest.fixture(scope="function")
# def file_a(tmp_dir_a) -> PosixPath:
#     f = PosixPath(tmp_dir_a) / "file"
#     with f.open(mode="w") as _f:
#         _f.write("1234")
#     return f


# @pytest.fixture(scope="function")
# def file_b(tmp_dir_b) -> PosixPath:
#     f = PosixPath(tmp_dir_b) / "file"
#     with f.open(mode="w") as _f:
#         _f.write("1234")
#     return f


# def test_one_syncf_deletes_another_file(
#     file_a, file_b, syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b
# ):
#     file_a.unlink()

#     time.sleep(expected_sync_delay)
#     assert not file_b.exists()


# def test_one_syncf_updates_another_file(
#     file_a, file_b, syncfs_a, syncfs_b, tmp_dir_a, tmp_dir_b
# ):
#     file_a.write_text("5678")

#     time.sleep(expected_sync_delay)
#     assert "5678" == file_b.read_text()
