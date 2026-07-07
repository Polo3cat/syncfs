.PHONY = all build-container config build instal test

PODMAN_RUN = podman run -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/home/ubuntu/.cache/ccache:rw,Z localhost/syncfs-env
PODMAN_BUILD = podman build -f Containerfile -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/home/ubuntu/.cache/ccache:rw,Z

install: build
	$(PODMAN_RUN) cmake --install .build/unixlike-clang-debug

build-container: Containerfile
	$(PODMAN_BUILD) -t syncfs-env

config:
	$(PODMAN_RUN) cmake --preset unixlike-clang-debug

build: config
	$(PODMAN_RUN) cmake --build .build/unixlike-clang-debug

test: install
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-debug
