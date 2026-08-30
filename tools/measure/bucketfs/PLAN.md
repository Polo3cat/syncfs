# syncfs vs BucketFS — benchmark plan and state of the work

Resume document. `README.md` next to this file says how the harness works;
this one says what the experiment is, what has been settled, and what is left.

Branch: `backprop-v70-wedged-torrent`, two commits ahead of `main`.

## Why

`document.txt` is the TFM. Its **Chapter 8, "Results", is empty** —
`syncfs-doc/main/chapter-results/results.tex` is 47 bytes, a `%!TEX root` line
and a chapter command. The whole project's motivation is one unmeasured
paragraph, `document.txt:228`:

> BucketFS has some limitations in terms of scalability. Big files take quite
> some time to be available on all nodes. This problem compounds as the number
> of nodes increases.

There is no BucketFS measurement anywhere in the repo. This work produces the
numbers that chapter needs.

The requirements the numbers answer, in the document's own terms (its text dump
strips periods, so `Scenario 31` is 3.1 and `1GiB` is 1 GiB):

| Scenario | Where | Claim |
|---|---|---|
| 3.5 | `document.txt:400-404` | 10 → 20 nodes costs **less than 2×** sync time. No test exists. |
| 3.12 | `:483-489` | bounded convergence time |
| 3.13 | `:490-495` | 10 KiB across 10 nodes in **≤ 5 s** |
| 3.14 | `:496-501` | 1 GiB across 10 nodes in **≤ 120 s** |
| goals | `:240` | "Many small files may need to be opened concurrently" — the UDF-load case |

## Decisions (settled with the author, do not relitigate)

- **Node matrix**: syncfs {2, 4, 10, 20}; BucketFS {2, 4}. The BucketFS points
  get a fitted trend extrapolated past 4, and the write-up must say plainly
  that it is an extrapolation. N=1 is dropped from propagation — replication to
  zero peers is not a measurement — and a syncfs mesh of 1 cannot even start,
  because an empty peer list is fatal (§I).
- **Sizes**: 1 KiB, 1 MiB, 100 MiB, 1 GiB, 4 GiB.
- **Metrics**: propagation to all nodes, many small files, writer-side ingest.
  Scenarios 3.1–3.4 (local read/write against a plain file) are **out of scope**
  for this run.
- **Repeats**: 5 for sub-GiB, 3 for the GiB tier. Report **median with min and
  max**, never the mean: both existing performance tests run
  `rounds=1, iterations=1` (§T.3) and report one sample with no distribution.
- **Non-convergence is a result**, recorded, not retried away. §R.27 already
  holds one such case (5 nodes, 1000 files, 16 KiB, 300 s) that was never
  written down as anything.

## What is already settled, and cost real time to learn

Do not rediscover any of this.

### Exasol runs multi-node under the Community Edition license

The image ships `max_nodes_per_cluster = 1`. That caps the **database**.
`init-sc --no-db` leaves the database out and `bucketfsd` still runs per node
and still replicates. Verified end to end at N=2. Without `--no-db` this
comparison cannot be run on the bundled license at all.

### BucketFS replicates synchronously with the PUT

Measured, and it reframes the comparison: the PUT does not return until every
node has the bytes. At N=2, 1 MiB: PUT 0.938 s, both nodes at 0.988 s.
`SyncPeriod` (default 30000 ms) is **not on the upload path** — it governs the
directory-scan path. The matrix therefore runs BucketFS in two configurations,
`default` (30 s) and `tuned` (1 s), so a result can distinguish "the shipped
configuration is slow" from "the replication mechanism is slow". If both
configurations come out identical, that is itself the finding and the `tuned`
arm can be dropped from later tiers.

### Arrival must be checked on allocated blocks, not size

libtorrent allocates sparse, so a receiving node's file reports its **full
size** the moment the torrent is added, before any bytes land. Proved directly:
at t=1.0 s a 200 MiB file read all 209,715,200 bytes with only 39,845,888
allocated; true arrival 2.4 s. A size-only check understated syncfs by 2.4× at
200 MiB and gets worse with size. `poller.complete()` gates on
`st_size == expected AND st_blocks * 512 >= expected`. This also caught itself:
a cell "arrived" on both nodes and then disagreed on the hash.

### Run on real disk

The session scratchpad is **tmpfs**. The first runs were measuring RAM. Use
`/var/tmp/syncfs-bench` — ext4 on the 916 G nvme, 389 G free.

### Other traps, all encoded in the harness already

- **Subnet must match EXAConf.** `init-sc` calls `get_local_ip` and indexes
  `[0]`, so a container on podman's default 10.88.0.0/16 dies with
  `IndexError: list index out of range` before any service starts.
  `exasol_cluster.py` pins a 10.10.10.0/24 network.
- **One EXAConf for the whole cluster.** UUIDs, `SyncKey` and bucket passwords
  are generated, so a per-node `init-sc` produces N mutually unintelligible
  clusters. One template is generated and copied.
- **Peers-file lines cap at 28 characters** (`include/discovery.h:10`) and an
  over-long line is fatal, not truncated (§B.5). The wire uses short network
  aliases (`tcp://sfs20:5555`); containers keep long unique names.
- **`rm -rf` cannot clear an Exasol root.** Rootless podman maps the
  container's uid 500 to a subuid, so BucketFS's files come back EACCES.
  Teardown goes through `podman unshare rm -rf`. Not cosmetic: a stale payload
  from a crashed run is on disk at the next run's t0 and the cell reports an
  arrival of ~0 s.
- **Delete through the service, on the writer only.** Unlinking on every node
  injects divergence that the *next* cell pays for. Each cell deletes on the
  writer and waits for it to propagate, which is Scenario 3.11 doing its job.
- **`--ulimit nofile` is set explicitly and recorded.** §C notes no ulimit is
  set in any container file and §R.26 that `connections_limit = -1` resolves to
  the soft `RLIMIT_NOFILE` verbatim. Measured 1048576 in both harnesses with
  26–56 fds actually open, so it is not currently binding — but leave it set,
  because at 20 nodes × 10 000 files the sizing rule `0.70 · M · (N−1)` wants
  ~1.3e5 connections.

### A syncfs defect was found and fixed mid-benchmark

`min_reconnect_time` defaulted to 60 s, so one failed connect made a peer
untouchable for a minute and ~1 write in 6 took 60 s instead of 0.1 s. Now 1 s;
worst case 1.25 s, 0 of 30 past §V.19. See §B.19, §V.70, §R.34 and
`.benchmarks/b19-min-reconnect-time.txt`.

**Consequence for the benchmark: any syncfs number taken before commit
`1c6f4c4` is invalid.** The partial `experiment-a-small.csv` from that period
has been deleted rather than kept, because `run_matrix.py` resumes by skipping
cells already present in the CSV and would have skipped exactly the poisoned
ones. Start the matrix from an empty CSV against the rebuilt
`localhost/syncfs` image.

## What is left to run

Nothing has been measured yet on the fixed binary. All three experiments are
outstanding.

Preconditions: on branch `backprop-v70-wedged-torrent`, `make run-image` has
been run since commit `1c6f4c4` (the current `localhost/syncfs` already has the
fix), no other containers running, host load average < 1.5.

### A — propagation (headline: Scenarios 3.5, 3.12, 3.13, 3.14)

One writer, one file, every node polled to completion. Staged so the cheap tier
lands first and the expensive one can be judged against it.

```sh
B=/var/tmp/syncfs-bench

# A1, sub-GiB tier, 5 repeats. ~2-4 h.
PYTHONUNBUFFERED=1 python3 tools/measure/bucketfs/run_matrix.py \
    --root $B/matrix --out .benchmarks/experiment-a-small.csv \
    --repeats 5 --sizes 1024,1048576,104857600

# A2, GiB tier, 3 repeats. Longer; 4 GiB x 20 nodes is 80 GiB peak on disk.
PYTHONUNBUFFERED=1 python3 tools/measure/bucketfs/run_matrix.py \
    --root $B/matrix --out .benchmarks/experiment-a-gib.csv \
    --repeats 3 --sizes 1073741824,4294967296
```

Both resume: re-running with the same `--out` skips cells already in the CSV.
`--plan` takes a path and process substitution works, as does `/dev/stdin`.

Answers 3.5 by comparing the N=10 and N=20 cells, and 3.13/3.14 by checking
1 KiB and 1 GiB at N=10 against the 5 s and 120 s budgets.

### B — many small files (the UDF-load goal)

```sh
PYTHONUNBUFFERED=1 python3 tools/measure/bucketfs/run_matrix.py \
    --experiment B --root $B/matrix --out .benchmarks/experiment-b.csv \
    --repeats 3 --counts 100,1000,10000 \
    --plan <(echo '[["syncfs","default",2,0],["syncfs","default",4,0],["syncfs","default",10,0],["bucketfs","default",2,30000],["bucketfs","default",4,30000]]')
```

Expect trouble on the syncfs side and record it rather than tuning around it:
`src/reconcile.cpp:224` paces repairs at 20 ms per gap, a 200 s floor at 10 000
files before bandwidth enters, and §R.27/§T.43 already record (5 nodes, 1000
files, 16 KiB) not converging in 300 s. A fresh cluster is brought up per cell,
because clearing ten thousand files through the service would cost more than
the cell it cleans up after.

### C — writer-side ingest

Cheap. The per-cell `ingest_s` column in A and B is the other half; this is the
plain-file baseline it is compared against, on the same filesystem.

```sh
PYTHONUNBUFFERED=1 python3 tools/measure/bucketfs/run_matrix.py \
    --experiment C --root $B/matrix --out .benchmarks/experiment-c-baseline.csv \
    --repeats 5
```

### Reading the results

```sh
python3 tools/measure/bucketfs/summarise.py .benchmarks/experiment-*.csv
python3 tools/measure/bucketfs/summarise.py --metric fanout_s .benchmarks/experiment-a-small.csv
```

Three times per cell, because one hides the asymmetry:

| | |
|---|---|
| `ingest_s` | t0 to the writer holding the complete file |
| `propagation_s` | t0 to the **last** node holding it — the user-visible number |
| `fanout_s` | writer arrival to last arrival — replication alone |

A syncfs write is local and returns immediately; a BucketFS PUT is the only way
in and returns once replication is done. `fanout_s` is the term that compares
the two mechanisms directly; `ingest_s` is where they differ by design.

## Indicative numbers, N=2, before the full matrix

Taken during harness validation. Directionally right, not the deliverable.

| 1 MiB, 2 nodes | ingest | all nodes |
|---|---|---|
| syncfs | 0.001 s | 0.100 s |
| BucketFS | 0.70 s | 1.00 s |

## What the write-up must not omit

- **Both systems run over a podman bridge on one host**, so neither is
  network-bound: these are CPU- and disk-bound runs. That favours neither, but
  it is not a cluster on 10 GbE and the numbers do not transfer unchanged. This
  belongs in the body, not a footnote. Running the Exasol half rootful would
  make `tc netem` shaping available for one confirmatory 1 GiB cell.
- The single host is also why the clock is not a problem: every container reads
  one kernel's `CLOCK_REALTIME`, so arrivals are directly comparable with no
  NTP anywhere.
- Payloads are raw random bytes named `.bin`. BucketFS auto-decompresses
  archives (`document.txt:220-224`); charging it for work syncfs does not do
  would not be a comparison.
- Report distributions, not medians alone. The §B.19 tail is fixed, but the
  discipline is what caught it.

## Open questions

- **Why the first peer connect fails at all** is unexplained (§T.67). One
  second only makes the retry cheap. This is the real defect under §B.19.
- **`all_settled` and the quiescence gate**: a torrent stuck `downloading`
  holds `all_settled` (`src/syncfs.cpp:494-499`) false, so the node reaches
  quiescence never and publishes `state` only at the 60 s ceiling. Mechanically
  true, was *not* the cause of §B.19, not chased, has no row of its own.
- Whether BucketFS `tuned` (SyncPeriod 1 s) differs measurably from `default`.
  If A1 shows no difference, drop the arm from A2 and B.
- Scenarios 3.7/3.8 (availability under partition and service death) are a
  separate experiment, not planned here.
- Chapter 8 itself. Harness and numbers first; the prose is a follow-on.
