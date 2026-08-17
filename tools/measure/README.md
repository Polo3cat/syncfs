# tools/measure

Measurement harnesses, not tests. Nothing here asserts anything, nothing here
runs under ctest, and a run takes minutes. Each script exists so that a §R row
in `SPEC.md` can be reproduced rather than believed.

| script | produced | answers |
|---|---|---|
| `measure_peak.py` | §R.27 | peak concurrent peer connections and open descriptors, against N nodes × M files |
| `measure_digest.py` | §R.28 | bytes on the wire for one `digest`, against M paths |

§R.26 has no script: it is a read of libtorrent's own source, and the citations
in the row are the reproduction.

## Running them

Both need the `syncfs` binary on `PATH` inside the test image, which is what
the `-e PATH=` below is for. Build first (`make build`).

```sh
podman run --rm \
  -v "$(pwd)":/syncfs:rw,Z \
  -e PATH="/syncfs/.build/unixlike-clang-debug/src:/usr/bin:/bin" \
  -e PYTHONUNBUFFERED=1 \
  localhost/syncfs-env \
  python3 /syncfs/tools/measure/measure_peak.py
```

`PYTHONUNBUFFERED=1` is not decoration. Piping the output anywhere holds every
row until the process exits, and these run long enough that a silent twenty
minutes looks like a hang.

`measure_digest.py` additionally needs pyzmq, which the project does not depend
on:

```sh
podman run --rm \
  -v "$(pwd)":/syncfs:rw,Z \
  -e PATH="/syncfs/.build/unixlike-clang-debug/src:/usr/bin:/bin" \
  -e PYTHONUNBUFFERED=1 \
  localhost/syncfs-env \
  sh -c 'pip install -q pyzmq && python3 /syncfs/tools/measure/measure_digest.py'
```

Adding pyzmq to `pyproject.toml` would put a dependency in §C for the sake of a
tool that is not part of the build or the suite, so it is installed at the point
of use instead. That is a decision, not an oversight.

Ports: `measure_peak.py` binds from 6200 upwards, `measure_digest.py` uses 6100
and 6101. Both are clear of the ranges the integration and performance suites
use, so a measurement and a test run do not collide — though they will still
compete for CPU, and a loaded host makes a peak measurement meaningless.

## What is not here, and why

An earlier descriptor sweep over sixty four byte files is deliberately not kept.
It reported a descriptor count that plateaued while torrents grew eighty fold,
which reads like a ceiling; what it had actually measured was a steady state,
because each transfer finished between two samples and the peak was never
sampled at all. `measure_peak.py` supersedes it by using files big enough that
transfers overlap, and by reading libtorrent's own peer count rather than
inferring load from descriptors.

The lesson is worth more than the script: with these two, a run whose transfers
complete faster than the sampling interval reports a number that is real,
reproducible, and about nothing.

## Reading the output

`measure_peak.py` prints one row per case. The column to watch is
`peers/product` — peak connections over M × (N−1). §R.27 puts it at ≈0.70 for
N ≥ 5. The N=2 case reports ≈0.01 and is an artifact of the two second
statistics tick, not a finding; it is kept in the case list so that anyone
re-running this meets it with the explanation attached.

`measure_digest.py` prints the size of every part. `bytes/path` should come out
at `len(path) + 21` — the path, a NUL, nineteen digits of ticks, a NUL.
