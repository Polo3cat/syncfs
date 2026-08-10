# Syncfs: synchronize files anywhere

Watch a directory for file changes. Spread those changes to nodes anywhere. Optimized for small and large files.

## Reach anywhere

If a [ZMQ](https://zeromq.org/) socket can be connected then it can synchronize files. Uses an efficient PUB-SUB pattern to announce file changes.

## Any file size

Uses [libtorrent](https://github.com/arvidn/libtorrent) for spreading files around the network. This is the most efficient way of distributing large files in a peer to peer network.
