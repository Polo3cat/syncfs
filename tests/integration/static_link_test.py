import os
import shutil
import subprocess

# Shared libraries the binary is allowed to load at runtime. Everything else
# must be linked statically: the CPM dependencies.
allowed_libs = (
    "linux-vdso",
    "libc.so",
    "libm.so",
    "libssl.so",
    "libcrypto.so",
    "libatomic.so",
    # libzmq links libbsd for strlcpy, which pulls libmd; libtorrent uses the
    # system zlib. All three are distro libraries rather than CPM dependencies.
    "libbsd.so",
    "libmd.so",
    "libz.so",
    # The C++ runtime is a distro library too. Linking it statically cost over a
    # megabyte and bought no portability the binary has: it already resolves
    # libc and OpenSSL from the host. libgcc_s comes in behind libstdc++.
    "libstdc++.so",
    "libgcc_s.so",
    "ld-linux",
)

# Dependencies whose presence in ldd output means the static link regressed.
forbidden_libs = (
    "libtorrent-rasterbar",
    "libspdlog",
    "libfmt",
    "libzmq",
    "libboost_",
)


def find_ldd() -> str:
    """Absolute path to ldd.

    The ctest PATH for these tests holds the syncfs build directory and
    CMAKE_SYSTEM_PREFIX_PATH, so the usual binary directories are absent and
    ldd cannot be found by name alone.
    """
    found = shutil.which("ldd") or shutil.which(
        "ldd", path="/usr/bin:/bin:/usr/local/bin"
    )
    assert found is not None, "ldd not found"
    return found


def shared_libs() -> list[str]:
    """Names of the shared objects `syncfs` depends on, per ldd."""
    binary = shutil.which("syncfs")
    assert binary is not None, "syncfs not on PATH"

    completed = subprocess.run(
        [find_ldd(), binary],
        capture_output=True,
        text=True,
        timeout=10,
        check=True,
    )

    names = []
    for line in completed.stdout.splitlines():
        entry = line.strip()
        if not entry:
            continue
        # The loader is printed as an absolute path rather than a soname.
        names.append(os.path.basename(entry.split()[0]))
    return names


def test_v30_no_cpm_dep_shared():
    """No CPM dependency may be linked dynamically."""
    libs = shared_libs()

    leaked = [lib for lib in libs if lib.startswith(forbidden_libs)]

    assert not leaked, f"linked dynamically instead of statically: {leaked}"


def test_v30_only_system_libs_shared():
    """The dynamic dependencies are limited to distro system libraries.

    A new entry here means a dependency started leaking into the binary as a
    shared object without being caught by the forbidden list.
    """
    libs = shared_libs()

    unexpected = [lib for lib in libs if not lib.startswith(allowed_libs)]

    assert not unexpected, f"unexpected shared dependencies: {unexpected}"
