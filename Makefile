.PHONY = install build config test build-image files dry-format

PODMAN_RUN = podman run -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/root/.cache/ccache:rw,Z localhost/syncfs-env
PODMAN_BUILD = podman build -f Containerfile -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/root/.cache/ccache:rw,Z

files:
	find src include tests -name *.h -o -name *.cpp > .build/clang-format-files

dry-format: files
	$(PODMAN_RUN) clang-format -Werror --dry-run --files=.build/clang-format-files

format: files
	$(PODMAN_RUN) clang-format -Werror --files=.build/clang-format-files -i

install: build
	$(PODMAN_RUN) cmake --install .build/unixlike-clang-debug

build: config dry-format
	$(PODMAN_RUN) cmake --build .build/unixlike-clang-debug

config:
	$(PODMAN_RUN) cmake --preset unixlike-clang-debug

test: install
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-debug

test-perfomance: install
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-debug -R syncfs-performance

build-image: Containerfile
	$(PODMAN_BUILD) -t syncfs-env
