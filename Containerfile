FROM fedora:44 AS build

RUN dnf install -y \
	ccache \
	clang \
	cmake \
	cppcheck \
	git \
	glibc \
	glibc-devel \
	kernel-devel \
	libbsd-devel \
	lld \
	llvm \
	ninja \
	patch \
	python \
	python-devel \
	&& dnf clean all

WORKDIR syncfs

RUN cmake . --preset unixlike-clang-debug \
	&& cmake --build .build/unixlike-clang-debug
