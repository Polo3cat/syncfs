#!/usr/bin/env bash
# Scripted walkthrough of a ten-node syncfs mesh (compose.yaml).
#
#   make run-image      # once, from the repository root
#   demo/demo.sh
#
# Every step writes into one or more nodes' host directories and then waits
# until all ten directories agree, printing how long that took. The mesh is
# torn down on exit; set KEEP=1 to leave it running and poke at data/ by hand.
#
#   SIZE_MIB=256 demo/demo.sh     # size of the large file in step 2
#   TIMEOUT=120  demo/demo.sh     # per-step convergence deadline, seconds

set -euo pipefail

cd "$(dirname "$0")"

NODES=(node{01..10})
SIZE_MIB=${SIZE_MIB:-256}
TIMEOUT=${TIMEOUT:-120}

# `podman compose` hands off to an external provider. docker-compose needs the
# Podman API socket running; podman-compose drives podman directly, so prefer it
# when it is installed and the caller has not picked a provider.
if [[ ! -v PODMAN_COMPOSE_PROVIDER ]] && command -v podman-compose >/dev/null; then
	export PODMAN_COMPOSE_PROVIDER=podman-compose
fi
export PODMAN_COMPOSE_WARNING_LOGS=false

function compose() {
	podman compose "$@"
}

function cleanup() {
	if [[ -v KEEP ]]; then
		echo
		echo "Mesh left running. Directories: $(pwd)/data/node01..node10"
		echo "Stop it with: (cd $(pwd) && podman compose down && rm -rf data)"
		return
	fi
	echo
	echo "Tearing down..."
	compose down --timeout 5 >/dev/null 2>&1 || true
	# The daemons wrote as the mapped syncfs user, which is the invoking user
	# thanks to keep-id, so a plain rm is enough.
	rm -rf data
}

function step() {
	echo
	echo "== $*"
}

# One line per node: the digest of every (relative path, content) pair in its
# directory. Two directories hold the same tree exactly when the lines match.
function tree_digest() {
	(cd "data/$1" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum) | sha256sum | cut -c1-16
}

# Wait until every node's tree digest equals node01's, or TIMEOUT expires.
# Hashing content rather than listing names matters: libtorrent allocates
# sparse, so a receiving node shows the full-size file before a byte has landed.
function converge() {
	local start=$SECONDS want digest lagging
	while :; do
		want=$(tree_digest node01)
		lagging=()
		for n in "${NODES[@]:1}"; do
			digest=$(tree_digest "$n")
			[[ "$digest" == "$want" ]] || lagging+=("$n")
		done
		if ((${#lagging[@]} == 0)); then
			echo "   converged on all ${#NODES[@]} nodes in $((SECONDS - start))s (tree ${want})"
			return 0
		fi
		if ((SECONDS - start >= TIMEOUT)); then
			echo "   NOT converged after ${TIMEOUT}s; lagging: ${lagging[*]}"
			show_trees
			return 1
		fi
		sleep 0.5
	done
}

function show_trees() {
	for n in "${NODES[@]}"; do
		printf '   %s  %s  %3d files\n' "$n" "$(tree_digest "$n")" "$(find "data/$n" -type f | wc -l)"
	done
}

if ! podman image exists localhost/syncfs; then
	echo "Image localhost/syncfs not found. Build it first: make run-image" >&2
	exit 1
fi

trap cleanup EXIT

step "Starting ${#NODES[@]} nodes"
mkdir -p "${NODES[@]/#/data/}"
compose up -d >/dev/null

# ZMQ PUB drops what it publishes before a subscriber has finished connecting,
# and no log line reports a subscription completing, so wait for every session
# and then give the handshakes a grace period before writing anything.
echo "   waiting for every daemon to start its libtorrent session..."
for n in "${NODES[@]}"; do
	until podman logs "syncfs-$n" 2>&1 | grep -q "Started libtorrent session"; do
		sleep 1
	done
done
sleep 5
echo "   all up"

step "1. A small file written on node01 reaches the other nine"
echo "hello from node01 at $(date -Is)" > data/node01/hello.txt
converge
echo "   node07 reads: $(cat data/node07/hello.txt)"

step "2. A ${SIZE_MIB} MiB file written on node05 is fetched from the whole swarm"
head -c "$((SIZE_MIB * 1024 * 1024))" /dev/urandom > data/node05/big.bin
converge

step "3. Every node writes its own file at the same moment"
for n in "${NODES[@]}"; do
	echo "written on $n" > "data/$n/from-$n.txt" &
done
wait
converge
echo "   node03 now holds: $(cd data/node03 && ls from-*.txt | tr '\n' ' ')"

step "4. Nested directories are synchronized recursively"
mkdir -p data/node08/reports/2026/q3
for i in {1..20}; do
	head -c 4096 /dev/urandom > "data/node08/reports/2026/q3/r$i.dat"
done
converge
echo "   node02 has $(find data/node02/reports -type f | wc -l) files under reports/"

step "5. Overwriting a file on one node replaces it everywhere (last write wins)"
echo "rewritten on node09" > data/node09/hello.txt
converge
echo "   node01 reads: $(cat data/node01/hello.txt)"

step "6. A delete on node10 removes the file from every node"
rm data/node10/big.bin
converge
holders=$(for n in "${NODES[@]}"; do [[ -e "data/$n/big.bin" ]] && printf '%s ' "$n"; done || true)
echo "   big.bin present on: ${holders:-no node}"

step "7. A node that was down catches up when it restarts"
compose stop node04 >/dev/null 2>&1
echo "   node04 stopped; writing on node06 while it is away"
echo "written while node04 was down" > data/node06/missed.txt
rm data/node06/from-node02.txt
compose start node04 >/dev/null 2>&1
# node04 comes back with a tree that disagrees with everyone else's; the
# periodic state hash spots the gap and a digest exchange repairs it.
converge

step "Final state"
show_trees
