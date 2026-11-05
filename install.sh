#!/bin/sh
# KingCobra installer - detects your operating system and CPU and installs the matching build.
#
#   curl -fsSL https://github.com/DivyanshuSinha136/kingcobra/releases/latest/download/install.sh | sh
#   sh install.sh --help
#
# What you get, by machine:
#   Linux  amd64 / arm64 / armhf / riscv64 (glibc)   .deb via apt  |  .rpm via dnf/zypper  |  .tar.gz
#   Linux  amd64 / arm64 (musl: Alpine, Void-musl)   .tar.gz
#   macOS  Apple Silicon (arm64) / Intel (amd64)     .tar.gz
#   Raspberry Pi 2-5, Orange Pi, BeagleBone, VisionFive, Milk-V ... (ARM / RISC-V single-board computers)
#                                                    same as Linux above; Pi Zero / Pi 1 (ARMv6) are compiled locally
#   Android (Termux), other CPUs, BSDs, ...          compiled ON YOUR MACHINE from the source release
# Bare-metal microcontrollers (Arduino, ESP32, RP2040, STM32) have no operating system or C library, so KingCobra
# cannot run on them; use a Linux microcomputer (or an MCU board that runs Linux).
# If a prebuilt package is missing for your machine the installer falls back to compiling from source
# (needs a C11 compiler and make). Every download is verified against the release's SHA256SUMS.
#
# Environment:
#   KC_VERSION=1.0.0       install this version instead of the latest release
#   KC_METHOD=auto|deb|rpm|tar|src   force the package type (default: auto)
#   KC_PREFIX=/some/dir    tarball / source install prefix (default: /usr/local as root, else ~/.local)
#   KC_REPO=owner/repo     GitHub repository to download from
#   KC_BASE_URL=...        alternative release base URL (mirrors, testing)
#   KC_DRY_RUN=1           only print what would be done
#   KC_INSTALL_DEPS=1      install the compiler / make / X11 / readline headers first when compiling (needs root)
#   KC_JOBS=N              parallel compile jobs (default: CPU count, limited by RAM on small boards)
#   KC_TMPDIR=/some/dir    where downloads and the compile happen (default: disk-backed /var/tmp for source builds)
#   KC_MACHINE=armv6l      pretend `uname -m` is this; KC_BOARD="name" pretends the board model (testing)
#   KC_VERIFY_ATTESTATION=1  also check the GitHub build-provenance attestation (needs the `gh` CLI)
set -eu

REPO=${KC_REPO:-DivyanshuSinha136/kingcobra}
BASE=${KC_BASE_URL:-https://github.com/$REPO/releases}

say()  { printf '%s\n' "$*"; }
warn() { printf 'install.sh: %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() {
    sed -n '2,33p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || true
    say "Options:  --uninstall   remove a KingCobra installed by this script (deb/rpm/tarball/source)"
    say "          --help        this text"
}

# ---- machine detection ------------------------------------------------------------------------
# name of a single-board computer (Raspberry Pi, Orange Pi, BeagleBone, ...) from the device tree, if there is one
detect_board() {
    b=""
    for f in /proc/device-tree/model /sys/firmware/devicetree/base/model; do
        if [ -r "$f" ]; then b=$(tr -d '\000' < "$f" 2>/dev/null); break; fi
    done
    if [ -z "$b" ] && [ -r /proc/cpuinfo ]; then b=$(sed -n 's/^Model[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n1); fi
    printf '%s' "$b"
}

# bare-metal microcontroller toolchains cannot host KingCobra: say so clearly instead of failing obscurely
mcu_hint() {
    case "$(uname -s 2>/dev/null)" in
        Linux|Darwin|FreeBSD|OpenBSD|NetBSD|MINGW*|MSYS*|CYGWIN*) return 0 ;;
    esac
    die "KingCobra needs an operating system with a C library and cannot run on bare-metal microcontrollers (Arduino, ESP32, RP2040, STM32). Use a Linux microcomputer such as a Raspberry Pi, or a board that runs Linux."
}

detect_machine() {
    os=$(uname -s)
    machine=${KC_MACHINE:-$(uname -m)}
    libc=glibc
    board=${KC_BOARD:-$(detect_board)}
    case "$os" in
        Linux)
            if [ "$(uname -o 2>/dev/null)" = Android ]; then libc=bionic          # Termux: no prebuilt, compile locally
            elif command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl; then libc=musl
            elif ls /lib/ld-musl-* >/dev/null 2>&1; then libc=musl; fi
            ;;
        Darwin)
            libc=""
            # a shell running under Rosetta reports x86_64 on an Apple Silicon Mac: install the native build
            if [ "$machine" = x86_64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
                machine=arm64
            fi
            ;;
        MINGW*|MSYS*|CYGWIN*)
            die "this looks like Windows. Use PowerShell:  irm https://github.com/$REPO/releases/latest/download/install.ps1 | iex   (or: choco install kingcobra / scoop install kingcobra)"
            ;;
    esac
    bits=$(getconf LONG_BIT 2>/dev/null || echo 64)   # userland word size (a 32-bit OS can run on a 64-bit kernel)
    case "$machine" in
        x86_64|amd64)   arch=amd64 ;;
        aarch64|arm64)  arch=arm64 ;;
        armv7l|armv8l)  arch=armhf ;;
        riscv64)        arch=riscv64 ;;
        *)              arch=$machine ;;
    esac
    if [ "$arch" = arm64 ] && [ "$bits" = 32 ]; then arch=armhf; fi
    if [ "$arch" = amd64 ] && [ "$bits" = 32 ]; then arch=i386; fi
    # ARMv6 and older (Raspberry Pi Zero / 1): the armhf packages need ARMv7, so these boards compile locally
    case "$machine" in armv6*|armv5*) arch=armv6 ;; esac
}

# does the release carry a prebuilt tarball for this OS / libc / CPU?
has_prebuilt() {
    case "$os/$libc/$arch" in
        Linux/glibc/amd64|Linux/glibc/arm64|Linux/glibc/armhf|Linux/glibc/riscv64) return 0 ;;
        Linux/musl/amd64|Linux/musl/arm64) return 0 ;;
        Darwin/*/amd64|Darwin/*/arm64) return 0 ;;
    esac
    return 1
}

is_rpm_family() {
    [ -f /etc/redhat-release ] || [ -f /etc/fedora-release ] && return 0
    [ -f /etc/os-release ] && grep -qiE '^(ID|ID_LIKE)=.*(fedora|rhel|centos|suse|amzn)' /etc/os-release
}

# ---- tools ------------------------------------------------------------------------------------
setup_tools() {
    if command -v curl >/dev/null 2>&1; then
        fetch()   { curl -fsSL --retry 3 -o "$2" "$1"; }
        resolve() { curl -fsSLI -o /dev/null -w '%{url_effective}' "$1"; }
    elif command -v wget >/dev/null 2>&1; then
        fetch()   { wget -q -O "$2" "$1"; }
        resolve() { wget -q --max-redirect=5 --spider -S "$1" 2>&1 | sed -n 's/^ *[Ll]ocation: *//p' | tail -n1 | tr -d '\r'; }
    else
        die "need curl or wget"
    fi
    if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum "$1" | cut -d' ' -f1; }
    elif command -v shasum >/dev/null 2>&1;   then sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
    elif command -v openssl >/dev/null 2>&1;  then sha256() { openssl dgst -sha256 "$1" | sed 's/.*= //'; }
    else die "need sha256sum, shasum or openssl to verify the download"; fi

    SUDO=""
    if [ "$(id -u)" != 0 ] && command -v sudo >/dev/null 2>&1; then SUDO=sudo; fi
}

can_root() { [ "$(id -u)" = 0 ] || [ -n "$SUDO" ]; }

default_prefix() {
    if [ -n "${KC_PREFIX:-}" ]; then prefix=$KC_PREFIX
    elif [ "$(id -u)" = 0 ]; then prefix=/usr/local
    else prefix=$HOME/.local; fi
}

# ---- choose how to install --------------------------------------------------------------------
choose_method() {
    method=${KC_METHOD:-auto}
    case "$method" in auto|deb|rpm|tar|src) ;; *) die "KC_METHOD must be auto, deb, rpm, tar or src" ;; esac
    forced=1; [ "$method" = auto ] && forced=0
    if [ "$method" = auto ]; then
        if [ "$os" = Linux ] && [ "$libc" = glibc ] && has_prebuilt && can_root; then
            if [ -f /etc/debian_version ] && command -v apt-get >/dev/null 2>&1; then method=deb
            elif is_rpm_family && { [ "$arch" = amd64 ] || [ "$arch" = arm64 ]; } && { command -v dnf >/dev/null 2>&1 || command -v zypper >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; }; then method=rpm
            else method=tar; fi
        elif has_prebuilt; then method=tar
        else method=src; fi
    fi
    if [ "$method" = deb ] || [ "$method" = rpm ]; then
        [ "$os" = Linux ] || die "KC_METHOD=$method is for Linux"
        can_root || die "KC_METHOD=$method needs root (run as root or install sudo); use KC_METHOD=tar for a per-user install"
    fi
    if [ "$method" != src ] && ! has_prebuilt; then
        [ "$forced" = 0 ] || die "no prebuilt KingCobra for $os/$arch ($libc); use KC_METHOD=src to compile it on this machine"
        method=src
    fi
}

rpm_arch() {
    case "$arch" in amd64) echo x86_64 ;; arm64) echo aarch64 ;; armhf) echo armv7hl ;; riscv64) echo riscv64 ;; *) echo "$arch" ;; esac
}

asset_name() {
    case "$method" in
        deb) echo "kingcobra_${ver}_${arch}.deb" ;;
        rpm) echo "kingcobra-${ver}-1.$(rpm_arch).rpm" ;;
        src) echo "kingcobra-${ver}-src.tar.gz" ;;
        tar)
            case "$os" in
                Darwin) echo "kingcobra-${ver}-macos-${arch}.tar.gz" ;;
                *)      if [ "$libc" = musl ]; then echo "kingcobra-${ver}-linux-musl-${arch}.tar.gz"
                        else echo "kingcobra-${ver}-linux-${arch}.tar.gz"; fi ;;
            esac ;;
    esac
}

resolve_version() {
    if [ -n "${KC_VERSION:-}" ]; then
        ver=${KC_VERSION#v}
    else
        url=$(resolve "$BASE/latest") || die "cannot reach $BASE/latest"
        ver=${url##*/}; ver=${ver#v}
        { [ -n "$ver" ] && [ "$ver" != latest ]; } || die "could not determine the latest version (set KC_VERSION)"
    fi
}

# ---- download + verify ------------------------------------------------------------------------
download_verified() {   # $1 = file name; result: $tmp/$1 verified against SHA256SUMS. Returns 2 if the asset is missing.
    f=$1
    fetch "$dl/$f" "$tmp/$f" || return 2
    fetch "$dl/SHA256SUMS" "$tmp/SUMS" || die "download failed: $dl/SHA256SUMS"
    want=$(awk -v f="$f" '$2==f || $2=="*"f {print $1}' "$tmp/SUMS")
    [ -n "$want" ] || die "$f is not listed in SHA256SUMS"
    got=$(sha256 "$tmp/$f")
    [ "$got" = "$want" ] || die "checksum mismatch for $f (expected $want, got $got) - not installing"
    say "Checksum OK ($f)"
    if [ "${KC_VERIFY_ATTESTATION:-0}" = 1 ]; then
        command -v gh >/dev/null 2>&1 || die "KC_VERIFY_ATTESTATION=1 needs the GitHub CLI (gh)"
        gh attestation verify "$tmp/$f" --repo "$REPO" >/dev/null || die "build provenance attestation check FAILED for $f"
        say "Build provenance attestation OK"
    fi
}

# ---- small-computer helpers -------------------------------------------------------------------
# Work directory. A compile on a Raspberry Pi would fill a RAM-backed /tmp, so source builds prefer disk (/var/tmp).
make_tmp() {
    base=${KC_TMPDIR:-${TMPDIR:-/tmp}}
    if [ -z "${KC_TMPDIR:-}" ] && [ "$method" = src ] && [ -d /var/tmp ] && [ -w /var/tmp ]; then base=/var/tmp; fi
    tmp=$(mktemp -d "$base/kingcobra.XXXXXX") || die "cannot create a work directory under $base (set KC_TMPDIR)"
    if [ "$method" = src ]; then
        free_kb=$(df -Pk "$tmp" 2>/dev/null | awk 'NR==2 {print $4}')
        if [ -n "$free_kb" ] && [ "$free_kb" -lt 400000 ]; then
            warn "only $((free_kb / 1024)) MB free in $base; compiling needs about 400 MB (use KC_TMPDIR=/path/on/a/bigger/disk)"
        fi
    fi
}

mem_mb() { sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\) kB.*/\1/p' /proc/meminfo 2>/dev/null | awk '{printf "%d", $1 / 1024}'; }

# parallel jobs: the CPU count, but at most one job per 512 MB of RAM so a 512 MB board does not run out of memory
pick_jobs() {
    if [ -n "${KC_JOBS:-}" ]; then jobs=$KC_JOBS; return; fi
    jobs=$( (getconf _NPROCESSORS_ONLN || nproc || sysctl -n hw.ncpu) 2>/dev/null | head -n1 )
    jobs=${jobs:-1}
    m=$(mem_mb)
    if [ -n "$m" ] && [ "$m" -gt 0 ]; then
        cap=$((m / 512)); [ "$cap" -ge 1 ] || cap=1
        if [ "$jobs" -gt "$cap" ]; then jobs=$cap; fi
        if [ "$m" -lt 512 ]; then
            warn "only $m MB of RAM: if the compile is killed, add swap (sudo fallocate -l 1G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile)"
        fi
    fi
}

# KC_INSTALL_DEPS=1: install a compiler, make and the optional X11 / readline headers with the system package manager
install_build_deps() {
    can_root || die "KC_INSTALL_DEPS=1 needs root (run as root or install sudo)"
    say "Installing build dependencies..."
    if   command -v apt-get >/dev/null 2>&1; then $SUDO apt-get update -qq; $SUDO apt-get install -y build-essential libx11-dev libreadline-dev || $SUDO apt-get install -y build-essential
    elif command -v dnf >/dev/null 2>&1;     then $SUDO dnf install -y gcc make libX11-devel readline-devel
    elif command -v yum >/dev/null 2>&1;     then $SUDO yum install -y gcc make libX11-devel readline-devel
    elif command -v zypper >/dev/null 2>&1;  then $SUDO zypper --non-interactive install gcc make libX11-devel readline-devel
    elif command -v apk >/dev/null 2>&1;     then $SUDO apk add --no-cache build-base libx11-dev readline-dev
    elif command -v pacman >/dev/null 2>&1;  then $SUDO pacman -Sy --noconfirm --needed base-devel libx11 readline
    elif command -v pkg >/dev/null 2>&1 && [ "$(uname -o 2>/dev/null)" = Android ]; then pkg install -y clang make
    else die "do not know this system's package manager; install a C compiler and make yourself"; fi
}

# ---- install back ends ------------------------------------------------------------------------
install_deb() { $SUDO apt-get install -y "$tmp/$file"; bin=$(command -v kingcobra || echo /usr/bin/kingcobra); }

install_rpm() {
    if   command -v dnf >/dev/null 2>&1;    then $SUDO dnf install -y "$tmp/$file"
    elif command -v zypper >/dev/null 2>&1; then $SUDO zypper --non-interactive --no-gpg-checks install "$tmp/$file"
    elif command -v yum >/dev/null 2>&1;    then $SUDO yum install -y "$tmp/$file"
    else $SUDO rpm -Uvh "$tmp/$file"; fi
    bin=$(command -v kingcobra || echo /usr/bin/kingcobra)
}

install_tar() {
    default_prefix
    mkdir -p "$prefix"
    tar -xzf "$tmp/$file" -C "$prefix"
    bin=$prefix/bin/kingcobra
}

install_src() {
    default_prefix
    if [ "${KC_INSTALL_DEPS:-0}" = 1 ]; then install_build_deps; fi
    cc=""
    for c in "${CC:-}" cc gcc clang; do
        if [ -n "$c" ] && command -v "$c" >/dev/null 2>&1; then cc=$c; break; fi
    done
    [ -n "$cc" ] || die "building from source needs a C compiler. Debian/Ubuntu: apt install build-essential | Fedora: dnf install gcc make | Alpine: apk add build-base | macOS: xcode-select --install"
    command -v make >/dev/null 2>&1 || die "building from source needs make"
    pick_jobs
    slow=""; case "$machine" in armv6*|armv5*) slow=" (an ARMv6 board such as the Pi Zero/1 needs 20-40 minutes)" ;; esac
    say "Compiling KingCobra $ver on this machine ($os/$machine${board:+, $board}, $cc, $jobs jobs) - this takes a minute or two$slow..."
    tar -xzf "$tmp/$file" -C "$tmp"
    src=$tmp/kingcobra-$ver
    [ -d "$src" ] || die "unexpected source archive layout"
    if ! make -C "$src" -j"$jobs" CC="$cc" >"$tmp/build.log" 2>&1; then
        tail -n 30 "$tmp/build.log" >&2
        die "the build failed (log above). Please report it with the output of: uname -a; $cc --version"
    fi
    targets="install-bin install-docs"; [ "$os" = Linux ] && targets="$targets install-mime"
    # shellcheck disable=SC2086   # $targets is a deliberate word list
    if ! make -C "$src" $targets PREFIX="$prefix" CC="$cc" >>"$tmp/build.log" 2>&1; then
        tail -n 20 "$tmp/build.log" >&2
        die "install step failed (log above)"
    fi
    bin=$prefix/bin/kingcobra
    # the Makefile links X11 / readline only when their headers exist; tell the user what a headless build lacks
    if [ "$os" = Linux ] && "$bin" info 2>/dev/null | grep -q 'headless'; then
        say "NOTE: built without the std.gui window backend (no libX11 headers). To add it: install libx11-dev, then re-run."
    fi
    if [ "$os" = Linux ] && "$bin" info 2>/dev/null | grep -q 'plain stdin'; then
        say "NOTE: built without readline (REPL history and Tab completion). To add it: install libreadline-dev, then re-run."
    fi
}

do_install() {
    mcu_hint
    detect_machine
    setup_tools
    resolve_version
    choose_method
    say "Detected: $os $machine ($arch${libc:+, $libc})${board:+ on $board}  ->  KingCobra $ver via $method"
    dl="$BASE/download/v$ver"
    file=$(asset_name)
    say "Package:  $file"
    if [ "${KC_DRY_RUN:-0}" = 1 ]; then say "dry run: would download $dl/$file, verify it against SHA256SUMS and install it"; exit 0; fi

    make_tmp
    trap 'rm -rf "$tmp"' EXIT INT TERM
    rc=0; download_verified "$file" || rc=$?
    if [ "$rc" = 2 ]; then
        if [ "$forced" = 0 ] && [ "$method" != src ]; then
            warn "no prebuilt $file in release v$ver - compiling from source on this machine instead"
            method=src; file=$(asset_name)
            download_verified "$file" || die "download failed: $dl/$file"
        else
            die "download failed: $dl/$file"
        fi
    fi

    case "$method" in deb) install_deb ;; rpm) install_rpm ;; tar) install_tar ;; src) install_src ;; esac

    if "$bin" version >/dev/null 2>&1; then
        say ""; "$bin" info | sed -n '1,5p'
        case "$method" in
            tar|src) case ":$PATH:" in *":$prefix/bin:"*) ;; *) say ""; say "NOTE: add $prefix/bin to your PATH:   export PATH=\"$prefix/bin:\$PATH\"" ;; esac ;;
        esac
        say ""; say "Installed. Try:  kingcobra help"
    else
        warn "installed to $bin, but it did not start."
        if command -v ldd >/dev/null 2>&1; then ldd "$bin" 2>&1 | grep 'not found' >&2 || true; fi
        warn "The prebuilt tarball needs the system library libX11 (Debian/Ubuntu: apt install libx11-6 | Fedora: dnf install libX11 | Alpine: apk add libx11)."
        warn "Or compile for this machine instead:  KC_METHOD=src sh install.sh"
        exit 1
    fi
}

# ---- uninstall --------------------------------------------------------------------------------
do_uninstall() {
    SUDO=""; if [ "$(id -u)" != 0 ] && command -v sudo >/dev/null 2>&1; then SUDO=sudo; fi
    if command -v dpkg >/dev/null 2>&1 && dpkg -s kingcobra >/dev/null 2>&1; then
        can_root || die "removing the .deb needs root"
        $SUDO apt-get remove -y kingcobra; say "Removed the kingcobra .deb"; return 0
    fi
    if command -v rpm >/dev/null 2>&1 && rpm -q kingcobra >/dev/null 2>&1; then
        can_root || die "removing the .rpm needs root"
        if command -v dnf >/dev/null 2>&1; then $SUDO dnf remove -y kingcobra; else $SUDO rpm -e kingcobra; fi
        say "Removed the kingcobra rpm"; return 0
    fi
    default_prefix
    [ -e "$prefix/bin/kingcobra" ] || die "no KingCobra found under $prefix (set KC_PREFIX if you installed elsewhere)"
    rm -f "$prefix/bin/kingcobra" "$prefix/bin/kc" "$prefix/share/mime/packages/kingcobra.xml"
    rm -rf "$prefix/share/doc/kingcobra"
    for s in 16x16 24x24 32x32 48x48 64x64 128x128 256x256 512x512; do
        rm -f "$prefix/share/icons/hicolor/$s/mimetypes/text-x-kingcobra.png" "$prefix/share/icons/hicolor/$s/apps/kingcobra.png"
    done
    if command -v update-mime-database >/dev/null 2>&1; then update-mime-database "$prefix/share/mime" >/dev/null 2>&1 || true; fi
    say "Removed KingCobra from $prefix"
}

main() {
    case "${1:-}" in
        --uninstall|uninstall) do_uninstall ;;
        -h|--help|help)        usage ;;
        "")                    do_install ;;
        *)                     usage >&2; exit 2 ;;
    esac
}

# everything lives in functions and main runs last: a truncated `curl | sh` download can never execute half a script
main "$@"
