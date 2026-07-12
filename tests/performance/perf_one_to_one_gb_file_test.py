import hashlib
import os
from pprint import pprint
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


@pytest.fixture(scope="function", autouse=True)
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


expected_sync_delay = 10

def sha256(file):
    with open(file, "rb") as f:
        return hashlib.file_digest(f, hashlib.sha256).hexdigest()

def sync_one_to_one(tmp_dirs) -> bool:
    end = time.time() + expected_sync_delay
    while time.time() < end:
        exists = [(PosixPath(dir_) / "file").exists() for dir_ in tmp_dirs]
        if not all(exists):
            print("Files don't exist yet")
            pprint(exists)
            print("Sleeping for 2 seconds")
            time.sleep(2)
            continue

        sizes = [(PosixPath(dir_) / "file").stat().st_size for dir_ in tmp_dirs]
        hashes = [sha256(PosixPath(dir_) / "file") for dir_ in tmp_dirs]
        if all(s == sizes[0] for s in sizes) and all(h == hashes[0] for h in hashes):
            return True

        print("Size and hashes were not the same")
        print("sizes:")
        pprint(sizes)
        print("hashes:")
        pprint(hashes)
        print("Sleeping for 2 seconds")
        time.sleep(2)
    return False


def test_benchmark_sync_one_to_many(tmp_dirs, benchmark):
    file = PosixPath(tmp_dirs[0]) / "file"
    with file.open(mode="wb") as f:
        f.write(os.urandom(1024**3+7))

    assert benchmark.pedantic(sync_one_to_one, (tmp_dirs,), iterations=1, rounds=1)
