#!/usr/bin/env bash
# Regenerate the per-node data directories and peer lists for compose.yaml.
# Idempotent: safe to re-run. Existing data directories are left alone.
set -euo pipefail

cd "$(dirname "$0")"

NODES=(A B C D E)
PORT=5555

for n in "${NODES[@]}"; do
	mkdir -p "data-$n"
	: > "peers-$n"
	for m in "${NODES[@]}"; do
		[[ "$m" == "$n" ]] && continue
		echo "tcp://host$m:$PORT" >> "peers-$n"
	done
done

echo "created ${#NODES[@]} nodes: ${NODES[*]}"
