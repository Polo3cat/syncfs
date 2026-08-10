# Syncfs: synchronize files anywhere

Watch a directory for file changes. Spread those changes to nodes anywhere. Optimized for small and large files.

## Reach anywhere

If a [ZMQ](https://zeromq.org/) socket can be connected then it can synchronize files. Uses an efficient PUB-SUB pattern to announce file changes.

## Any file size

Uses [libtorrent](https://github.com/arvidn/libtorrent) for spreading files around the network. This is the most efficient way of distributing large files in a peer to peer network.

# Building
Everything is self-contained. The only hard dependency is a Linux system with Podman. Builds with Podman, runs with Podman.

## It's a single Make target away
```bash
make run-image
```
builds a `syncfs` image.

# Running
Use the built image. Mount the directory you want to syncrhonize into `/data` and publish the ports.
## Simply
```bash
cat > peers-A <<EOF
tcp://hostB:5555
EOF

cat > peers-B <<EOF
tcp://hostA:5555
EOF

# Host A
podman run -v peers-A:/peers,Z -v host-A/directory:/data:rw,Z --userns=keep-id:uid=1000 syncfs /peers hostA:5555
# Host B
podman run -v peers-B:/peers,Z -v host-B/directory:/data:rw,Z --userns=keep-id:uid=1000 syncfs /peers hostB:5555
```
This syncrhonizes changes from directory `host-A/directory` on machine `Host A` to `host-B/directory` on `Host B`. These are rootless containers so you'll want to map the user owning the directories to the syncfs user inside the container.

## Locally

```bash
podman network create syncfs
podman run --network syncfs --hostname hostA  -v ./peers-A:/peers:Z -v ./hostA:/data:rw,Z --userns=keep-id:uid=1000 syncfs /peers hostA:5555
podman run --network syncfs --hostname hostB  -v ./peers-B:/peers:Z -v ./hostB:/data:rw,Z --userns=keep-id:uid=1000 syncfs /peers hostB:5555
```
