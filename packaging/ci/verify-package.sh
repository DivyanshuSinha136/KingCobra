#!/usr/bin/env bash
# Install a built package in a CLEAN container the way a user would, run it, and uninstall it again.
#   verify-package.sh <docker-platform> <image> <file> [kind]     kind = deb | rpm | tar (default: from the extension)
# e.g. verify-package.sh linux/amd64 ubuntu:22.04 dist/kingcobra_1.0.0_amd64.deb
set -euo pipefail
PLATFORM=$1; IMAGE=$2; FILE=$3
KIND=${4:-}
if [[ -z "$KIND" ]]; then case "$FILE" in *.deb) KIND=deb ;; *.rpm) KIND=rpm ;; *) KIND=tar ;; esac; fi
FILE=$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")
exec docker run --rm --platform "$PLATFORM" -e KIND="$KIND" -e PKG="/pkg/$(basename "$FILE")" \
    -v "$(dirname "$FILE")":/pkg:ro "$IMAGE" sh -eu -c '
. /etc/os-release
echo "== verifying $PKG on $PRETTY_NAME ($(uname -m))"
case "$KIND" in
  deb) export DEBIAN_FRONTEND=noninteractive; apt-get update -qq; apt-get install -y -qq "$PKG" >/dev/null ;;
  rpm) if command -v dnf >/dev/null; then dnf install -y -q "$PKG"; else yum install -y -q "$PKG"; fi ;;
  tar) command -v apk >/dev/null && apk add --no-cache libx11 >/dev/null
       if command -v apt-get >/dev/null; then apt-get update -qq; apt-get install -y -qq libx11-6 >/dev/null; fi
       mkdir -p /opt/kc; tar -xzf "$PKG" -C /opt/kc; export PATH=/opt/kc/bin:$PATH ;;
esac
kingcobra version
kc version
cat > /tmp/hello.kc <<EOF2
fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }
fn main() { print("fib=", fib(20)); }
EOF2
out=$(kingcobra run /tmp/hello.kc); echo "$out"
case "$out" in *6765*) ;; *) echo "unexpected output" >&2; exit 1 ;; esac
case "$KIND" in
  deb) apt-get remove -y -qq kingcobra >/dev/null; ! command -v kingcobra ;;
  rpm) (dnf remove -y -q kingcobra || yum remove -y -q kingcobra) >/dev/null; ! command -v kingcobra ;;
esac
echo "== OK"
'
