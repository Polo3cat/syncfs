#!/usr/bin/env bash
# §V.70: every small write reaches the peer inside §V.19's five seconds.
#
# One write cannot fail this test honestly. §B.19 measured the fault at roughly
# one write in six, and both performance tests run `rounds=1, iterations=1`
# (§T.3), which is why a fault this size has been shipping unseen: a single
# sample reads green five times in six. Twenty sequential writes is what turns
# a rate into an assertion.
#
# Arrival is size AND allocated blocks. libtorrent allocates sparse, so a
# receiving node's file reports its full size the moment the torrent is added
# and before a byte of it has landed; size alone would call every write an
# instant success and this test would pass while measuring nothing.

set -uo pipefail

cd "$(dirname "$0")"

WRITES=${WRITES:-20}
SIZE=1024
DEADLINE=5

function cleanup() {
	podman-compose down
	rm -fr data-*
	rm -f peers-* payload.bin
}

if [[ ! -v DEBUG ]]; then
	trap cleanup EXIT
fi

source ./setup.sh

podman-compose up -d

# ZMQ PUB drops what it publishes before a subscriber has finished connecting,
# and no log line reports the subscription completing (§B.9).
echo "Waiting for every node to come up..."
for node in A B; do
	until podman logs "rsw-host${node}" 2>&1 | grep -q "Started libtorrent session"; do
		sleep 1
	done
done
sleep 5

head -c "$SIZE" /dev/urandom > payload.bin

slow=0
for i in $(seq 1 "$WRITES"); do
	name=$(printf 'w%03d.bin' "$i")
	start=$SECONDS
	cp payload.bin "data-A/$name"

	arrived=0
	while ((SECONDS - start <= DEADLINE)); do
		if [[ -f "data-B/$name" ]]; then
			size=$(stat -c %s "data-B/$name")
			blocks=$(stat -c %b "data-B/$name")
			if ((size == SIZE)) && ((blocks * 512 >= SIZE)); then
				arrived=1
				break
			fi
		fi
		sleep 0.05
	done

	if ((arrived == 1)); then
		echo "  write $i: arrived in $((SECONDS - start))s"
	else
		echo "  write $i: STILL MISSING after ${DEADLINE}s"
		slow=$((slow + 1))
	fi
done

echo
if ((slow > 0)); then
	echo "FAIL: $slow of $WRITES writes missed V19's ${DEADLINE}s deadline"
	exit 1
fi
echo "PASS: all $WRITES writes arrived within ${DEADLINE}s"
