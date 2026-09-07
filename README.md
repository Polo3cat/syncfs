# syncfs

Keep a directory identical across a set of machines. No central server, no
tracker, no cloud account — a static peer list and a daemon per node.

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![platform Linux](https://img.shields.io/badge/platform-Linux-lightgrey)
![license Unlicense](https://img.shields.io/badge/license-Unlicense-green)

syncfs watches its working directory with inotify, announces every change over
a [ZeroMQ](https://zeromq.org/) PUB/SUB mesh, and moves the bytes with
[libtorrent](https://github.com/arvidn/libtorrent) (BitTorrent v2). Small files
cost one announcement; a large file is pulled from every node that already has
a piece of it, so adding nodes adds bandwidth instead of consuming it.

- [How it works](#how-it-works)
- [Quick start](#quick-start)
- [Usage](#usage)
- [Building from source](#building-from-source)
- [Development](#development)
- [Design notes and limits](#design-notes-and-limits)
- [Security](#security)
- [Measurement](#measurement)
- [Project layout](#project-layout)
- [License](#license)

## How it works

```
        control plane (ZMQ PUB/SUB)              data plane (BitTorrent v2)
   create · remove · state · digest                    pieces, DHT + LSD + PEX

   node A ──────────┐                            node A ═══════════╗
                    ├── every node subscribes                      ║ swarm per
   node B ──────────┤   to every peer           node B ═══════════╬═ file
                    │                                             ║
   node C ──────────┘                            node C ═══════════╝
```

1. **Detect.** inotify reports a closed write, a delete, or a rename under the
   sync root.
2. **Announce.** The node publishes `create` (with a bencoded single-file
   torrent and the origin mtime) or `remove` (with the delete time).
3. **Transfer.** Receivers add the torrent and fetch it from the swarm. Peers
   are found through DHT, local service discovery and PEX — there is no tracker
   and no public DHT bootstrap node.
4. **Reconcile.** ZMQ PUB drops messages silently when a subscriber is slow or
   still connecting, so convergence never rests on delivery. While the tree is
   quiescent each node publishes a 32-byte root hash every 5 s; on a mismatch
   the lagging node asks one peer for a full `digest` and the answer is a
   broadcast `create`. A dropped announcement costs one repair round, not a
   permanent divergence.

Ordering is last-write-wins on the origin node's wall clock. Ties between two
`create`s go to the lower info hash; a `create` racing a `remove` keeps the
file, because a wrongly kept file is visible and re-deletable while a wrongly
deleted one is gone.

`SPEC.md` is the full specification: constraints, wire format, invariants and
the measurements behind them.

## Quick start

Everything is self-contained. The only hard requirement is a Linux host with
[Podman](https://podman.io/).

```bash
# Build the runtime image (compiles a release binary, ships only the binary).
make run-image
```

Two nodes on one Podman network, syncing `./nodeA` and `./nodeB`:

```bash
podman network create syncfs
mkdir -p nodeA nodeB
echo 'tcp://nodeB:5555' > peers-A
echo 'tcp://nodeA:5555' > peers-B

podman run -d --name nodeA \
    --network syncfs:alias=nodeA --hostname nodeA \
    --userns=keep-id:uid=1000 \
    -v ./nodeA:/data:rw,Z -v ./peers-A:/peers:ro,Z \
    syncfs /peers nodeA:5555

podman run -d --name nodeB \
    --network syncfs:alias=nodeB --hostname nodeB \
    --userns=keep-id:uid=1000 \
    -v ./nodeB:/data:rw,Z -v ./peers-B:/peers:ro,Z \
    syncfs /peers nodeB:5555
```

```bash
echo hello > nodeA/greeting
cat nodeB/greeting          # hello
```

The containers are rootless, so `--userns=keep-id` maps the user owning the
host directories onto the `syncfs` user inside the container. Without it the
daemon cannot write into `/data`.

### Across real hosts

Give each container the host's own network namespace, so the address it
publishes is one it can actually bind:

```bash
# On hostA
echo 'tcp://hostB:5555' > peers
podman run -d --network host --userns=keep-id:uid=1000 \
    -v ./data:/data:rw,Z -v ./peers:/peers:ro,Z \
    syncfs /peers hostA:5555
```

Open **TCP 5555** for the control plane and **TCP and UDP 7555** for the data
plane on every node — libtorrent listens 2000 above the ZMQ port.

## Usage

```
syncfs <peers file> <listen address>
```

syncfs synchronizes its **working directory**, recursively; regular
non-symlink files only. In the shipped image the working directory is `/data`.

**`<peers file>`** — one address per line, `tcp://host:port`, every peer except
this node. Blank lines are skipped. A missing or unreadable file, a list that
parses to nothing, and a line that does not fit are all fatal at startup: a
daemon that syncs with nobody is not a useful daemon.

> **Line length.** An address is capped at 27 characters, the length of
> `tcp://255.255.255.255:65535`. Long hostnames do not fit — use a short alias
> or an IP address.

**`<listen address>`** — `host:port`. This is the node's identity on the control
plane, not only a bind address: peers address their digests to the string
published here. So it must be

- reachable by every peer,
- spelled the same way everywhere and unique across the peer set,
- a real host — `0.0.0.0`, `::`, `*` and an empty host are rejected at startup,
  because two nodes sharing that spelling each read the other's state as their
  own and the repair plane disappears silently,
- port 1–63535, since libtorrent takes `port + 2000`.

**Ports.** ZMQ PUB on `port`/TCP; libtorrent on `port + 2000`, TCP and UDP.

**Logging** goes to stdout via spdlog: `info` and up for a release build,
`debug` for a debug build, which adds a torrent table every 2 s.

**Shutdown.** `SIGTERM` and `SIGINT` end the sync loop and exit 0.

## Building from source

The build runs inside a container image, so nothing but Podman is installed on
the host. Two images are involved:

| Image | Built by | Purpose |
|---|---|---|
| `localhost/syncfs-env` | `make build-image` | Fedora 44 + clang, ninja, ccache, lld, cppcheck, clang-tidy, Python. Used by every other target. |
| `localhost/syncfs` | `make run-image` | Runtime image. A release binary plus the handful of distro libraries it links against. |

```bash
make build-image      # once, and after Containerfile changes
make build            # configure, check formatting, compile (LEVEL=debug)
make build LEVEL=release
make install          # install to .install/, one file: bin/syncfs
```

Dependencies — fmt, spdlog, libzmq, cppzmq, Boost, libtorrent, googletest — are
fetched at configure time by [CPM](https://github.com/cpm-cmake/CPM.cmake) into
`.cpm-cache` at pinned versions and linked statically. Only the system
libraries stay dynamic — libc, libstdc++, OpenSSL, libatomic, libbsd, libmd and
zlib — which means the binary is not portable across distributions: the run
host must supply a matching libc and OpenSSL 3. `cmake --install` places
exactly one file.

Building without the Makefile works too, if you have clang and CMake ≥ 4.3:

```bash
cmake --preset unixlike-clang-debug     # or -release, or -dist for the shipping binary
cmake --build .build/unixlike-clang-debug
```

## Development

| Target | Does |
|---|---|
| `make test` | Everything under ctest |
| `make test-unit` | GoogleTest suites: `utils`, `source`, `monitor`, `protocol`, `files`, `sink`, `discovery` |
| `make test-integration` | pytest, real daemons in temporary directories |
| `make test-performance` | pytest-benchmark, one-to-many transfers |
| `make format` / `make dry-format` | clang-format over `src include tests`, `-Werror` |
| `make clean` | Remove `.build`, `.install`, `.cpm-cache`, `.venv` |

Debug builds run clang-tidy (`Checks: "*"` minus an exclusion list) and
cppcheck with warnings as errors; the `dist` preset drops both, along with the
sanitizers. Test sources are not exempt from the analyzers — vendored
dependencies are.

Integration and performance tests need the `syncfs` binary on `PATH` and are
Python: pytest, pytest-repeat, pytest-benchmark, Python ≥ 3.13, managed by
poetry inside the build image.

## Design notes and limits

- **Linux only.** inotify and `poll` are load-bearing.
- **No configuration file.** The reconcile period (5 s), quiescence window
  (10 s), `state` ceiling (60 s) and tombstone TTL (1 h) are compile-time
  constants.
- **Static peer set,** read once at startup. Changing it means a restart.
- **No conflict resolution,** no version history, no merge. Last write wins.
- **Tombstones live in memory,** so a node that was down longer than the TTL
  resurrects files deleted while it was away. Persisting them would need a
  state directory outside the sync root, which is a new argument and a new set
  of error paths.
- **Clock skew must stay well under the reconcile period.** Ordering rests on
  the origin node's wall clock; enough skew loses a genuine delete or a genuine
  create.
- **Connections, not descriptors, are the ceiling.** Peak concurrent
  connections run at roughly `0.70 × files × (peers − 1)` for five nodes or
  more, and libtorrent is configured to take `RLIMIT_NOFILE` as that budget
  verbatim. Size `--ulimit nofile=` accordingly for large trees. uTP
  multiplexes over one UDP socket, so measured descriptor counts stay small.

## Security

**The trust boundary is the peers file.** Both planes are plaintext and
unauthenticated: anyone who can reach a PUB port reads every announcement, and
anyone on the same LAN can read the whole tree over local service discovery. A
peer listed in the file can delete any path on any node, because deletes are
adopted from wire-supplied tombstones and nothing signs them.

Do not run syncfs across a boundary the peers file does not own. Confidentiality
and resistance to a malicious peer are out of scope.

## Measurement

`tools/measure` holds harnesses, not tests — nothing there asserts, nothing runs
under ctest, and a run takes minutes to hours. Each one exists so a research row
in `SPEC.md` can be reproduced rather than believed. See
[`tools/measure/README.md`](tools/measure/README.md), and
[`tools/measure/bucketfs/README.md`](tools/measure/bucketfs/README.md) for the
syncfs / BucketFS comparison. Raw results are in `.benchmarks`.

## Project layout

```
include/          Headers, one per module
src/
  syncfs.cpp      main, the sync loop, the libtorrent session
  monitor.cpp     inotify watches over the tree
  files.cpp       tree listing and diffing
  discovery.cpp   peers file parsing
  protocol.cpp    wire format, announcement encode and decode
  source.cpp      publishing: create, remove, state, digest, repair
  sink.cpp        receiving and dispatch
  reconcile.cpp   hash comparison, gap detection, partner choice
tests/
  unit/           GoogleTest
  integration/    pytest against real daemons
  performance/    pytest-benchmark
tools/measure/    Measurement harnesses
cmake/            Warnings, sanitizers, static analyzers, hardening
SPEC.md           The specification: constraints, invariants, research
```

## License

Public domain, under the [Unlicense](LICENSE).
