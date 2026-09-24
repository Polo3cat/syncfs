# Ten-node demo

A syncfs mesh of ten containers on one Podman network, `node01` to `node10`.
Every node's sync root is a directory under `demo/data/`, so the convergence
can be watched, and driven, from the host.

```bash
make run-image        # builds localhost/syncfs
make demo             # or: demo/demo.sh
```

`demo.sh` brings the mesh up, waits for every daemon, and then walks through
seven steps, each followed by a wait until all ten directories hold the same
tree (compared by content hash, not by name or size):

1. a small file written on `node01` reaches the other nine;
2. a large random file written on `node05` is fetched from the swarm;
3. all ten nodes write a file at the same moment;
4. a nested directory of 20 files is created on `node08`;
5. a file is overwritten on `node09` — last write wins;
6. a file is deleted on `node10`;
7. `node04` is stopped, the tree changes behind its back, and it catches up
   through the reconcile plane once it restarts.

The mesh is torn down and `data/` removed on exit.

| Variable | Default | Effect |
|---|---|---|
| `KEEP` | unset | Leave the mesh running after the walkthrough |
| `SIZE_MIB` | `256` | Size of the file in step 2 |
| `TIMEOUT` | `120` | Seconds each step may take to converge |

## By hand

```bash
cd demo
mkdir -p data/node{01..10}
podman compose up -d

echo hello > data/node01/hello.txt
cat data/node10/hello.txt
podman logs -f syncfs-node03

podman compose down && rm -rf data
```

`podman compose` uses whichever provider is configured. docker-compose needs
the Podman API socket (`systemctl --user start podman.socket`); podman-compose
does not, and `demo.sh` selects it when it is installed.

`peers/` holds one static peers file per node, listing the other nine by
service name. The listen address each node publishes is its own service name,
which is what makes it reachable under the same spelling from every peer.
