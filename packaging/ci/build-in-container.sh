#!/usr/bin/env bash
# usage: build-in-container.sh <docker-platform> <image> <out-dir>      (env RUN_TESTS=0 to skip the smoke tests)
# e.g.   build-in-container.sh linux/arm/v7 debian:bookworm dist
#        build-in-container.sh linux/amd64  alpine:3.20     dist        (musl tarball)
#        build-in-container.sh linux/arm64  fedora:40       dist        (rpm)
# Foreign platforms need QEMU registered (docker/setup-qemu-action in CI, or
# `docker run --privileged --rm tonistiigi/binfmt --install all` locally).
set -euo pipefail
PLATFORM=$1; IMAGE=$2; OUT=$(mkdir -p "$3" && cd "$3" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
# the tree is mounted read-write on purpose (the build writes build/ and kingcobra into it); CI checkouts are throw-away
exec docker run --rm --platform "$PLATFORM" -e RUN_TESTS="${RUN_TESTS:-1}" -e OUT=/out \
    -v "$SRC":/src -v "$OUT":/out "$IMAGE" bash /src/packaging/ci/inner-build.sh
