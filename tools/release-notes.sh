#!/usr/bin/env bash
# Print the docs/CHANGELOG.md section for a version (the text of the GitHub Release):  tools/release-notes.sh 1.0.0
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1#v}
notes=$(awk -v v="$VER" '
    $0 ~ "^## " v "( |$)" { on = 1; next }
    on && /^## /          { exit }
    on                    { print }
' docs/CHANGELOG.md)
[[ -n "${notes//[$'\t\r\n ']/}" ]] || { echo "No docs/CHANGELOG.md section for $VER" >&2; exit 1; }
printf '%s\n' "$notes"
