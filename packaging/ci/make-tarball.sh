#!/usr/bin/env bash
# Stage a relocatable tarball from the already-built tree:  make-tarball.sh <out-file.tar.gz> [--no-mime]
# Layout (unpack into a prefix such as /usr/local or ~/.local):  bin/ share/man share/doc/kingcobra [share/mime share/icons]
# --no-mime: leave out the freedesktop MIME type and icons (macOS has no such database).
set -euo pipefail
OUT=$1; MIME=install-mime; [[ "${2:-}" == --no-mime ]] && MIME=""
SRC=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
make -C "$SRC" install-bin install-man install-docs $MIME PREFIX=/usr/local DESTDIR="$STAGE" >/dev/null
tar -czf "$OUT" -C "$STAGE/usr/local" .
echo "wrote $OUT"; tar -tzf "$OUT" | head -n 8
