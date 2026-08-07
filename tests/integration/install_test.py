import os
import shutil
import subprocess
import tempfile
from pathlib import Path

# The install prefix must hold the syncfs binary and nothing else. Every CPM
# dependency is linked statically into it, so their headers, archives, pkg-config
# files and CMake package files have no reason to ship. syncfs is a program
# rather than a library, so it exports no CMake package of its own either.
expected_files = {"bin/syncfs"}


def find_cmake() -> str:
    """Absolute path to cmake.

    The ctest PATH for these tests holds the syncfs build directory and
    CMAKE_SYSTEM_PREFIX_PATH, so the usual binary directories are absent and
    cmake cannot be found by name alone.
    """
    found = shutil.which("cmake") or shutil.which(
        "cmake", path="/usr/bin:/bin:/usr/local/bin"
    )
    assert found is not None, "cmake not found"
    return found


def installed_files() -> set[str]:
    """Paths installed by `cmake --install`, relative to a throwaway prefix."""
    binary_dir = os.environ.get("SYNCFS_BINARY_DIR")
    assert binary_dir is not None, "SYNCFS_BINARY_DIR not set by ctest"

    with tempfile.TemporaryDirectory() as prefix:
        subprocess.run(
            [find_cmake(), "--install", binary_dir, "--prefix", prefix],
            capture_output=True,
            text=True,
            timeout=300,
            check=True,
        )

        root = Path(prefix)
        return {
            str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()
        }


def test_v31_only_syncfs_installed():
    """Installing must place the binary and no dependency artefact."""
    files = installed_files()
    unexpected = sorted(files - expected_files)

    # A regression here installs every dependency header, so print a sample
    # rather than thousands of paths.
    assert not unexpected, (
        f"{len(unexpected)} unexpected installed files, first 10: {unexpected[:10]}"
    )
    assert files == expected_files, f"missing from install: {expected_files - files}"
