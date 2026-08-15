#!/usr/bin/env bash

function cleanup() {
	podman-compose down
	rm -fr data-*
	rm peers-*
	[[ -v TMP && -d "$TMP" ]] && rm -fr "$TMP"
}

if [[ ! -v DEBUG ]]; then
	trap cleanup EXIT
fi

source ./setup.sh

podman-compose up -d

# ZMQ publishers drop what they send before a subscriber has finished
# connecting, so nothing may be written until every daemon is up and every
# subscription has reached every publisher. The grace period covers the
# handshakes, which no log line reports.
echo "Waiting for every node to come up..."
for node in A B C D E; do
	until podman logs "syncfs-host${node}" 2>&1 | grep -q "Started libtorrent session"; do
		sleep 1
	done
done
sleep 5

echo "Generating random dirs and files..."

for i in {1..11}; do for j in {1..11}; do 
	mkdir -p "data-A/${i}/${j}"
	for k in {1..21}; do
		tail -c "$(bc <<< "$RANDOM % (100*1024^2)")" > "data-A/${i}/${j}/${k}.file" < /dev/random
done; done; done

echo "Done generating random dirs and files"

EXPECTED=$(find data-A -type f | wc -l)

# Poll instead of guessing: every peer is done once it holds as many files as
# the sender. The deadline is generous because each file is its own torrent and
# every one of them has to be announced, discovered and fetched.
DEADLINE=$((SECONDS + 900))
while ((SECONDS < DEADLINE)); do
	pending=0
	for dir in data-{B,C,D,E}; do
		[[ $(find "$dir" -type f | wc -l) -eq $EXPECTED ]] || pending=1
	done
	((pending == 0)) && break
	sleep 5
done

echo "Waited $((SECONDS)) seconds for $EXPECTED files to reach every peer"

TMP=$(mktemp -d --tmpdir test-XXXXX)

# The node directory is stripped so that the sums compare content and relative
# path only, which is what synchronization means here.
for dir in data-*; do
	(cd "$dir" && find . -type f -print | sort | xargs sha1sum) > "${TMP}/sum-$dir"
done

for dir in "${TMP}"/sum-data-{B,C,D,E}; do
	diff "${TMP}"/sum-data-A "$dir" || { echo "Incorrect sum for $dir" && exit 1; }
done
