.PHONY: config build build-test build-image install \
	files dry-format format \
	test test-unit test-integration test-perfomance \
	clean

PODMAN_RUN = podman run -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/root/.cache/ccache:rw,Z localhost/syncfs-env
PODMAN_BUILD = podman build -f Containerfile -v $$(pwd):/syncfs:rw,Z -v $$HOME/.cache/ccache:/root/.cache/ccache:rw,Z
LEVEL = debug

install: build
	$(PODMAN_RUN) cmake --install .build/unixlike-clang-$(LEVEL) --strip

build-test:
	$(PODMAN_RUN) cmake --build .build/unixlike-clang-$(LEVEL) -t poetry-install

files:
	find src include tests -name *.h -o -name *.cpp > .build/clang-format-files

dry-format: files
	$(PODMAN_RUN) clang-format -Werror --dry-run --files=.build/clang-format-files

format: files
	$(PODMAN_RUN) clang-format -Werror --files=.build/clang-format-files -i

build: config dry-format
	$(PODMAN_RUN) cmake --build .build/unixlike-clang-$(LEVEL)

config:
	mkdir -p .virtualenvs
	$(PODMAN_RUN) cmake --preset unixlike-clang-$(LEVEL)

test:
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-$(LEVEL)

test-integration:
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-$(LEVEL) -R syncfs-integration

test-perfomance:
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-$(LEVEL) -R syncfs-performance

test-unit:
	$(PODMAN_RUN) ctest --preset test-unixlike-clang-$(LEVEL) -R .*unit

build-image: Containerfile
	$(PODMAN_BUILD) -t syncfs-env

clean:
	rm -fr .venv
	rm -fr .build
	rm -fr .cpm-cache
