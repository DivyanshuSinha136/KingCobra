#!/usr/bin/env bash
# Release consistency check. Run it before you tag (CI runs it too, and the release workflow runs it with the tag):
#   tools/check-release.sh            # checks the tree
#   tools/check-release.sh v1.0.0     # ...and that the tag matches the version compiled into the program
# The single source of truth for the version is KC_VERSION in compiler/include/kc_common.h.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
TAG=${1:-}
fail=0
# chk "what is checked" command...   -> prints ok/FAIL and remembers failures
chk() { local what=$1; shift; if "$@" >/dev/null 2>&1; then echo "ok:   $what"; else echo "FAIL: $what"; fail=1; fi; }

VER=$(sed -n 's/.*define KC_VERSION "\([^"]*\)".*/\1/p' compiler/include/kc_common.h)
# shellcheck disable=SC2016  # the regex is evaluated by the inner bash, on purpose
chk "KC_VERSION '$VER' is MAJOR.MINOR.PATCH[-pre]" bash -c '[[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]' _ "$VER"
[[ $fail == 0 ]] || { echo "release checks FAILED"; exit 1; }

[[ -z "$TAG" ]] || chk "tag $TAG matches KC_VERSION v$VER" test "$TAG" = "v$VER"
chk "debian/changelog starts with $VER"            grep -q "^kingcobra ($VER)" debian/changelog
chk "docs/CHANGELOG.md has a '## $VER' section"    grep -qE "^## $VER( |$)" docs/CHANGELOG.md
chk "tests/smoke_test.sh expects 'KingCobra $VER'" grep -q "KingCobra $VER" tests/smoke_test.sh
chk "assets/banner.svg shows v$VER"                grep -q "v$VER" assets/banner.svg
chk "index.html shows v$VER"                       grep -q "v$VER" index.html
chk "no OWNER/REPO placeholders in install scripts" bash -c '! grep -qE "OWNER/REPO" install.sh install.ps1 2>/dev/null'

#if git rev-parse --git-dir >/dev/null 2>&1; then
    #chk "no build output or binaries are tracked by git" bash -c '! git ls-files | grep -E "\.(o|so|dll|dylib|exe|deb|rpm|whl|nupkg)$|(^|/)build/"'
#fi
if [[ $fail == 0 ]]; then echo "release checks passed"; else echo "release checks FAILED"; fi
exit $fail
