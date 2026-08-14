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

sleep 1
echo "Generating random dirs and files..."

for i in {1..11}; do for j in {1..11}; do 
	mkdir -p "data-A/${i}/${j}"
	for k in {1..21}; do
		tail -c "$(bc <<< "$RANDOM % (100*1024^2)")" > "data-A/${i}/${j}/${k}.file" < /dev/random
done; done; done

echo "Done generating random dirs and files"

sleep 60

TMP=$(mktemp -d --tmpdir test-XXXXX)

for dir in data-*; do
	find "$dir" -type f -print | sort | xargs sha1sum > "${TMP}/sum-$dir"
done

for dir in "${TMP}"/sum-data-{B,C,D,E}; do
	diff "${TMP}"/sum-data-A "$dir" || { echo "Incorrect sum for $dir" && exit 1; }
done
