#!/usr/bin/env bash
# Runs INSIDE a Linux container (see build-in-container.sh): build KingCobra on the container's CPU and libc, run
# the smoke tests, and produce the release files in $OUT. What is produced depends on the distribution:
#   Debian/Ubuntu (glibc) : kingcobra_<ver>_<arch>.deb  +  kingcobra-<ver>-linux-<arch>.tar.gz
#   Alpine (musl)         : kingcobra-<ver>-linux-musl-<arch>.tar.gz
#   Fedora / RHEL family  : kingcobra-<ver>-1.<rpmarch>.rpm
# plus smoke-<label>.log.   Env: OUT (default /out), RUN_TESTS=1|0
set -euo pipefail
OUT=${OUT:-/out}; RUN_TESTS=${RUN_TESTS:-1}
export DEBIAN_FRONTEND=noninteractive

# shellcheck disable=SC1091
. /etc/os-release
case "${ID}:${ID_LIKE:-}" in
    alpine:*)                      FAMILY=alpine ;;
    fedora:*|rhel:*|centos:*|rocky:*|almalinux:*|*:*fedora*|*:*rhel*) FAMILY=rpm ;;
    *)                             FAMILY=deb ;;
esac

case "$FAMILY" in
    deb)
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends build-essential dpkg-dev fakeroot libx11-dev libreadline-dev ca-certificates python3 >/dev/null ;;
    alpine)
        apk add --no-cache build-base bash coreutils findutils tar gzip libx11-dev readline-dev python3 >/dev/null ;;
    rpm)
        dnf install -y -q gcc make tar gzip findutils rpm-build libX11-devel readline-devel python3 >/dev/null ;;
esac

cd /src
mkdir -p "$OUT"
VER=$(sed -n 's/.*define KC_VERSION "\([^"]*\)".*/\1/p' compiler/include/kc_common.h)
case "$(uname -m)" in
    x86_64) ARCH=amd64 ;; aarch64) ARCH=arm64 ;; armv7l|armv8l) ARCH=armhf ;; riscv64) ARCH=riscv64 ;; *) ARCH=$(uname -m) ;;
esac
[[ "$FAMILY" == alpine ]] && LABEL=musl-$ARCH || LABEL=$ARCH
echo "== KingCobra $VER on $PRETTY_NAME / $(uname -m) / $(gcc -dumpmachine)"

# build from a clean tree and keep the compiler output so warnings can be reported
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" 2>&1 | tee "$OUT/build-$LABEL.log" | grep -E 'warning:|error:' || true
warnings=$(grep -c 'warning:' "$OUT/build-$LABEL.log" || true)
[[ "$warnings" == 0 ]] || echo "::warning::$warnings compiler warning(s) on $LABEL (see build-$LABEL.log)"
./kingcobra info

status=0
if [[ "$RUN_TESTS" == 1 ]]; then
    bash tests/smoke_test.sh >"$OUT/smoke-$LABEL.log" 2>&1 || status=$?
    tail -n 3 "$OUT/smoke-$LABEL.log"
    grep -E '^FAIL' "$OUT/smoke-$LABEL.log" | head -40 || true
fi

case "$FAMILY" in
    deb)
        # dependency check skipped (libx11/readline are installed above); -nc reuses the objects built above (matters under QEMU)
        dpkg-buildpackage -us -uc -b -d -nc
        mv "../kingcobra_${VER}_${ARCH}.deb" "$OUT"/
        bash packaging/ci/make-tarball.sh "$OUT/kingcobra-$VER-linux-$ARCH.tar.gz" ;;
    alpine)
        bash packaging/ci/make-tarball.sh "$OUT/kingcobra-$VER-linux-musl-$ARCH.tar.gz" ;;
    rpm)
        bash packaging/rpm/build-rpm.sh "$OUT" ;;
esac
ls -la "$OUT"
exit "$status"
