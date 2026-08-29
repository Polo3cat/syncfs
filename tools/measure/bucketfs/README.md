# tools/measure/bucketfs

The syncfs / BucketFS comparison. Same footing as the rest of `tools/measure`:
harnesses, not tests. Nothing here asserts, nothing runs under ctest, and a
full matrix takes hours. Each script exists so a §R row can be reproduced
rather than believed.

What it answers is the one claim the thesis rests on and never measured
(`document.txt:228`):

> BucketFS has some limitations in terms of scalability. Big files take quite
> some time to be available on all nodes. This problem compounds as the number
> of nodes increases.

| script | role |
|---|---|
| `exasol_cluster.py` | N-node Exasol whose BucketFS is host-visible |
| `syncfs_cluster.py` | N-node syncfs mesh, same shape |
| `poller.py` | the shared probe: when did this file reach this node |
| `run_matrix.py` | drives the matrix, one CSV row per cell, resumable |

## The license, and why an N-node BucketFS is measurable anyway

The image ships an Exasol Community Edition license:

```
max_db_mem_size_in_gb = 10
max_db_raw_data_size_in_gb = 10
max_num_clusters = 1
max_nodes_per_cluster = 1
```

That caps the **database**. `init-sc --no-db` leaves the database out, and
`bucketfsd` is a COS service that still runs per node and still replicates.
Verified: a 2-node cluster replicates a 1 MiB file end to end, and a GET on the
receiving node returns it in full. Without `--no-db` this comparison could not
be run on the bundled license at all.

## Three decisions that make it a comparison

**One probe, host-side, for both systems.** Every node's storage is a host bind
mount — syncfs's sync root, and Exasol's `/exa`, which puts the bucket at
`/exa/data/bucketfs/bfsdefault/{default,.dest/default}` (the two are hardlinks
to one inode). So nothing is sampled through `podman exec`, which costs
100-300 ms per call and would have been most of the answer at 1 KiB.

**Arrival is the moment the file reaches its expected size.** The hash is read
once, afterwards, and reported separately. Folding verification into the
arrival time is the defect §T.3 records in `perf_gb_file_one_to_many_test.py`,
which re-hashes ten gigabytes on every 2 s poll and reports the sum.

**Three times per cell, because one would hide the asymmetry:**

| | |
|---|---|
| `ingest_s` | t0 to the writer holding the complete file |
| `propagation_s` | t0 to the *last* node holding it — the user-visible number |
| `fanout_s` | writer arrival to last arrival — replication alone |

The two systems' t0 are different operations by construction: a syncfs write is
local and returns immediately, a BucketFS PUT is the only way in and returns
once replication is done. `fanout_s` is the term that compares the two
mechanisms directly; `ingest_s` is the term where they differ by design.

## Running it

```sh
python3 tools/measure/bucketfs/run_matrix.py \
    --root /var/tmp/matrix \
    --out .benchmarks/experiment-a.csv \
    --repeats 5
```

`--sizes` overrides the ladder (1 KiB, 1 MiB, 100 MiB, 1 GiB, 4 GiB) and
`--plan` overrides the node matrix, a JSON list of
`[system, config, nodes, sync_period_ms]`. The default plan is syncfs at
N ∈ {2, 4, 10, 20} and BucketFS at N ∈ {2, 4} in two configurations.

Re-running with the same `--out` skips cells already in the CSV, so an
interrupted matrix resumes rather than restarts.

`PYTHONUNBUFFERED=1` is not decoration: piping the output holds every row until
the process exits, and a silent two hours looks like a hang.

## Two configurations of BucketFS, deliberately

`SyncPeriod` defaults to 30000 ms and `HttpPort` to 0 (disabled, HTTPS only).
Both are set here:

* HTTP replaces HTTPS, so BucketFS is not charged for TLS that syncfs's
  plaintext ZMQ and BitTorrent planes never pay.
* the matrix runs `default` (30 s) *and* `tuned` (1 s), because otherwise a
  result cannot distinguish "the shipped configuration is slow" from "the
  replication mechanism is slow". Only the second would be a finding about
  BucketFS.

## Things that bite

**The subnet must match EXAConf.** `init-sc` calls `get_local_ip` and indexes
`[0]` into the interfaces inside the node's own subnet, so a container on
podman's default 10.88.0.0/16 dies with `IndexError: list index out of range`
before any service starts. `exasol_cluster.py` creates a 10.10.10.0/24 network
and pins each node's IP to match its `PrivateNet`.

**Every node needs the same EXAConf.** UUIDs, the `SyncKey` and the bucket
passwords are all generated, so running `init-sc` separately per node produces
N mutually unintelligible clusters. One template is generated and copied.

**Peers-file lines cap at 28 characters** — `len("tcp://255.255.255.255:65535") + 1`
(`include/discovery.h:10`) — and an over-long line is fatal, not truncated
(§B.5). Container names are long and unique; the wire uses a short network
alias (`tcp://sfs20:5555`).

**`rm -rf` cannot clear an Exasol root.** Under rootless podman a privileged
container's uid 500 lands on a subuid, so BucketFS's own files return EACCES to
the host user. Teardown goes through `podman unshare rm -rf`. This is not
cosmetic: a stale payload left by a crashed run is still on disk at the next
run's t0, and the cell then reports an arrival of ~0 s.

**Delete through the service, on the writer only.** Clearing a cell by
unlinking on every node injects exactly the divergence syncfs then has to
reconcile, and the *next* cell pays for it at the 60 s `state` ceiling. Each
cell deletes on the writer and waits for the delete to propagate, which is
Scenario 3.11 doing its job.

**`--ulimit nofile` is set explicitly, and recorded.** §C notes no ulimit is set
in any container file, and §R.26 that `connections_limit = -1` resolves to the
soft `RLIMIT_NOFILE` *verbatim*. The sizing rule is `0.70 · M · (N−1)`
connections, so 20 nodes × 10 000 files wants ~1.3e5. Left to the runtime's
default, a cell would be capped silently and the number would be about the
default, not about syncfs.

## Reading the output

`converged` false is a result, not a failed run. §R.27 already holds one such
case — 5 nodes, 1000 files, 16 KiB, 300 s — that was never written down as
anything, and a matrix that drops non-convergence would lose the most
interesting cells it produces.

Report the **median** with min and max. Both existing performance tests run
`rounds=1, iterations=1` (§T.3), which reports one sample and no distribution;
at these sizes the distribution is the finding.

## What this does not measure

Both systems run over a podman bridge on one host, so neither is network-bound:
these are CPU- and disk-bound runs. That favours neither, but it is not a
cluster on 10 GbE, and the numbers do not transfer unchanged. The single host
is also why the clock is not a problem — every container reads one kernel's
`CLOCK_REALTIME`, so arrivals are directly comparable with no NTP anywhere.

Payloads are raw random bytes named `.bin`. BucketFS auto-decompresses archives
(`document.txt:220-224`); charging it for work syncfs does not do would not be
a comparison.

Scenarios 3.1-3.4 (local open, read and write against a plain file) are not
here. They are argued in Chapter 6 to hold by construction, and measuring them
is separate work.
