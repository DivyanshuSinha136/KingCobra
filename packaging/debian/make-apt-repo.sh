#!/usr/bin/env bash
# Build a signed-or-unsigned APT repository from one or more kingcobra_*.deb files.
#
#   packaging/debian/make-apt-repo.sh <repo-dir> kingcobra_1.0.0_amd64.deb [more .deb ...]
#   GPG_KEY=<key id or email> packaging/debian/make-apt-repo.sh repo *.deb      # also signs
#
# Layout produced (host <repo-dir> on any static web server, GitHub Pages, S3, ...):
#   pool/main/k/kingcobra/*.deb
#   dists/stable/main/binary-<arch>/Packages{,.gz}
#   dists/stable/Release  (+ InRelease and Release.gpg when GPG_KEY is set)
#   kingcobra-archive-keyring.gpg   (when GPG_KEY is set; users put it in /etc/apt/keyrings/)
# Needs: dpkg-dev (dpkg-scanpackages), gzip, sha256sum, md5sum; gpg only for signing.
set -euo pipefail
[[ $# -ge 2 ]] || { sed -n '2,12p' "$0"; exit 2; }
REPO=$1; shift
SUITE=${SUITE:-stable}; COMP=main

mkdir -p "$REPO/pool/$COMP/k/kingcobra"
for d in "$@"; do cp -f "$d" "$REPO/pool/$COMP/k/kingcobra/"; done

archs=()
for d in "$REPO/pool/$COMP/k/kingcobra"/*.deb; do
    a=$(dpkg-deb -f "$d" Architecture); [[ " ${archs[*]:-} " == *" $a "* ]] || archs+=("$a")
done

cd "$REPO"
for a in "${archs[@]}"; do
    dir=dists/$SUITE/$COMP/binary-$a
    mkdir -p "$dir"
    dpkg-scanpackages --arch "$a" pool/ /dev/null > "$dir/Packages" 2>/dev/null
    gzip -9nk -f "$dir/Packages"
done

rel=dists/$SUITE/Release
{
    echo "Origin: KingCobra"
    echo "Label: KingCobra"
    echo "Suite: $SUITE"
    echo "Codename: $SUITE"
    echo "Date: $(LC_ALL=C date -Ru | sed 's/+0000/UTC/')"
    echo "Architectures: ${archs[*]}"
    echo "Components: $COMP"
    echo "Description: KingCobra programming language packages"
    for algo in MD5Sum:md5sum SHA256:sha256sum; do
        echo "${algo%%:*}:"
        (cd "dists/$SUITE" && find "$COMP" -type f | sort | while read -r f; do
            printf ' %s %16d %s\n' "$(${algo##*:} "$f" | cut -d' ' -f1)" "$(stat -c %s "$f")" "$f"
        done)
    done
} > "$rel"

if [[ -n "${GPG_KEY:-}" ]]; then
    gpg --batch --yes --default-key "$GPG_KEY" -abs -o "$rel.gpg" "$rel"
    gpg --batch --yes --default-key "$GPG_KEY" --clearsign -o "dists/$SUITE/InRelease" "$rel"
    gpg --export "$GPG_KEY" > kingcobra-archive-keyring.gpg
    echo "signed with $GPG_KEY; keyring: $REPO/kingcobra-archive-keyring.gpg"
else
    echo "NOTE: repository is UNSIGNED (set GPG_KEY to sign). Clients would need [trusted=yes]."
fi
echo "repository ready in $REPO for: ${archs[*]}"
