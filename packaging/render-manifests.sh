#!/usr/bin/env bash
# Fill the Homebrew formula and the Scoop manifest with the real version and SHA-256 of the release files.
#   packaging/render-manifests.sh <dist-dir> <owner/repo> <out-dir>
# <dist-dir> holds kingcobra-<ver>-src.tar.gz and kingcobra-<ver>-windows-x64.zip (the release assets).
set -euo pipefail
DIST=$1; REPO=$2; OUT=$3
cd "$(dirname "$0")/.."
VER=$(sed -n 's/.*define KC_VERSION "\([^"]*\)".*/\1/p' compiler/include/kc_common.h)
sha() { sha256sum "$1" | cut -d' ' -f1; }
SRC=$(sha "$DIST/kingcobra-$VER-src.tar.gz")
WIN=$(sha "$DIST/kingcobra-$VER-windows-x64.zip")
mkdir -p "$OUT"
sed -e "s|@VERSION@|$VER|g" -e "s|@REPO@|$REPO|g" -e "s|@SRC_SHA256@|$SRC|g" packaging/homebrew/kingcobra.rb.in > "$OUT/kingcobra.rb"
sed -e "s|@VERSION@|$VER|g" -e "s|@REPO@|$REPO|g" -e "s|@WIN_SHA256@|$WIN|g" packaging/scoop/kingcobra.json.in > "$OUT/kingcobra.json"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$OUT/kingcobra.json"
echo "rendered $OUT/kingcobra.rb and $OUT/kingcobra.json for $VER"
