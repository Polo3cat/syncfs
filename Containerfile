FROM fedora:44 AS build

RUN dnf install -y --setopt=install_weak_deps=False \
	cmake \
	clang \
	lld \
	ninja \
	git \
	patch \
	python \
	kernel-devel \
	glibc-devel \
	libbsd-devel \
	&& dnf clean all

WORKDIR syncfs

RUN mkdir -p .build && \
	cd .build && \
	cmake .. --preset unixlike-clang-debug
	cmake --build
