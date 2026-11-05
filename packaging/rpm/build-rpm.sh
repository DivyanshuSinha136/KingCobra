#!/usr/bin/env bash
# Build the RPM for THIS machine's architecture:  packaging/rpm/build-rpm.sh [out-dir]   (default: dist)
#   -> <out-dir>/kingcobra-<version>-1.<arch>.rpm
# Needs rpmbuild (Fedora: dnf install rpm-build gcc make libX11-devel readline-devel).
# RPM_NODEPS=1 skips the BuildRequires check (for building on a non-RPM distro in CI/dev).
set -euo pipefail
OUT=$(mkdir -p "${1:-dist}" && cd "${1:-dist}" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
VER=$(sed -n 's/.*define KC_VERSION "\([^"]*\)".*/\1/p' "$SRC/compiler/include/kc_common.h")
[[ -n "$VER" ]] || { echo "cannot read KC_VERSION" >&2; exit 1; }
command -v rpmbuild >/dev/null || { echo "rpmbuild not found (install rpm-build)" >&2; exit 1; }

TOP=$(mktemp -d); trap 'rm -rf "$TOP"' EXIT
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,RPMS,SRPMS,BUILDROOT}
# source tarball straight from the tree (works with or without git); build outputs are left out
tar -czf "$TOP/SOURCES/kingcobra-$VER.tar.gz" \
    --exclude=./.git --exclude=./build --exclude=./dist --exclude=./site --exclude='./kcos/build' \
    --exclude='*.o' --exclude='*.d' --exclude='*.deb' --exclude='*.rpm' --exclude=./kingcobra \
    --transform "s,^\.,kingcobra-$VER," -C "$SRC" .
sed "s/@VERSION@/$VER/g" "$SRC/packaging/rpm/kingcobra.spec.in" > "$TOP/SPECS/kingcobra.spec"

args=(--define "_topdir $TOP" -bb)
[[ "${RPM_NODEPS:-0}" == 1 ]] && args+=(--nodeps)
rpmbuild "${args[@]}" "$TOP/SPECS/kingcobra.spec"
cp "$TOP"/RPMS/*/kingcobra-"$VER"-1.*.rpm "$OUT"/
ls -la "$OUT"
