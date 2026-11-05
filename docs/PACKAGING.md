# Packaging and releases

KingCobra is released as one GitHub Release per version, built by GitHub Actions on every kind of device it supports.
`install.sh` / `install.ps1` pick the right file for the user's machine. This page is for maintainers; users only need
the install section of the README.

## What a release contains

| File | For |
|---|---|
| `kingcobra_<ver>_<arch>.deb` | Debian / Ubuntu on amd64, arm64, armhf, riscv64 (apt) |
| `kingcobra-<ver>-1.<arch>.rpm` | Fedora / RHEL / Rocky / Alma / openSUSE on x86_64, aarch64 (dnf, zypper) |
| `kingcobra-<ver>-linux-<arch>.tar.gz` | any glibc Linux (amd64, arm64, armhf, riscv64), unpack into a prefix |
| `kingcobra-<ver>-linux-musl-<arch>.tar.gz` | Alpine and other musl Linux (amd64, arm64) |
| `kingcobra-<ver>-macos-<arch>.tar.gz` | macOS Apple Silicon (arm64) and Intel (amd64) |
| `kingcobra-<ver>-windows-x64.zip`, `kingcobra.<ver>.nupkg` | Windows x64 (zip, Chocolatey) |
| `kingcobra-<ver>-src.tar.gz` | everything else: compiled on the user's machine (also used by Homebrew) |
| `kingcobra.rb`, `kingcobra.json` | Homebrew formula and Scoop manifest, rendered with the real hashes |
| `install.sh`, `install.ps1` | the installers |
| `SHA256SUMS` | checksums of every file above (the installers verify against it) |

Every binary file also has a GitHub build-provenance attestation: `gh attestation verify <file> --repo DivyanshuSinha136/kingcobra`.

## How users install

    curl -fsSL https://github.com/DivyanshuSinha136/kingcobra/releases/latest/download/install.sh | sh     # Linux, macOS
    irm https://github.com/DivyanshuSinha136/kingcobra/releases/latest/download/install.ps1 | iex            # Windows

`install.sh` detects the OS (Linux glibc / musl, macOS incl. a shell under Rosetta), the CPU (`x86_64` -> amd64,
`aarch64`/`arm64` -> arm64, `armv7l`/`armv8l` or a 32-bit userland on a 64-bit ARM kernel -> armhf, `riscv64`), and the
package manager, then downloads that file, **verifies it against `SHA256SUMS`**, installs it, runs `kingcobra version`,
and prints what is missing if it does not start (for the tarballs: libX11).

- Debian family with root/sudo -> `.deb` via apt; RPM family (amd64/arm64) with root/sudo -> `.rpm` via dnf/zypper/yum;
  otherwise the tarball into `~/.local` (or `/usr/local` as root).
- **No prebuilt file for the machine** (other CPU, BSD, ARMv6, 32-bit x86) **or the file is missing from the release**
  -> it downloads `-src.tar.gz`, checks it, and compiles it locally (`cc` or `gcc` or `clang`, and `make`), then
  installs into the prefix. `KC_METHOD=src` forces this.
- Options: `KC_VERSION`, `KC_METHOD=auto|deb|rpm|tar|src`, `KC_PREFIX`, `KC_REPO`, `KC_BASE_URL`, `KC_DRY_RUN=1`,
  `KC_VERIFY_ATTESTATION=1` (also checks the attestation; needs `gh`). `sh install.sh --uninstall` removes it again
  (apt/dnf for packages, plain file removal for tarball and source installs).
- **Microcomputers (single-board computers).** The installer reads the board model from the device tree
  (`Raspberry Pi 4 Model B`, `Orange Pi 5`, `BeagleBone`, `StarFive VisionFive`, ...) and shows it. Pi 2-5 and other
  ARMv7 / ARM64 / RISC-V 64 boards get the matching prebuilt file; a 32-bit OS on a 64-bit Pi gets `armhf`. **ARMv6
  boards (Pi Zero, Pi 1) and ARMv5** cannot run the `armhf` packages (they need ARMv7), so they compile locally. Local
  compiles are sized for small boards: jobs are limited to one per 512 MB of RAM (`KC_JOBS` overrides), the work
  directory is the disk-backed `/var/tmp` rather than a RAM-backed `/tmp` (`KC_TMPDIR` overrides), low RAM / disk
  produce a warning with the swap command, and `KC_INSTALL_DEPS=1` installs the compiler, `make` and the optional
  libX11 / readline headers first (apt, dnf, yum, zypper, apk, pacman, Termux `pkg`). A board without X11 / readline
  headers gets a working headless build and a note saying how to add them. Android (Termux) is compiled locally too.
- **Microcontrollers** (Arduino, ESP32, RP2040, STM32 running bare metal) have no operating system or C library, so
  KingCobra cannot run on them; the installer says so instead of failing obscurely. Boards that run Linux are microcomputers.
- Everything lives in functions and `main` runs last, so a truncated `curl | sh` download cannot run half a script.

`install.ps1` (PowerShell 5.1+/7) installs to `%LOCALAPPDATA%\Programs\KingCobra`, adds it to the user PATH, creates
`kc.cmd`, checks the SHA-256, and needs no administrator rights. `KC_UNINSTALL=1` removes it.

## Other channels

- **Chocolatey** (`choco install kingcobra`): `packaging/chocolatey/`. The package contains no binary; its install script
  downloads the release zip and verifies the SHA-256 baked into the package. The `chocolatey` job packs it, test-installs
  and uninstalls it on a Windows runner and attaches the `.nupkg`. Add the secret `CHOCOLATEY_API_KEY` to push to
  community.chocolatey.org (every version goes through their moderation first).
- **Scoop**: `scoop install https://github.com/DivyanshuSinha136/kingcobra/releases/latest/download/kingcobra.json`
  (works with no extra repository; the manifest has `checkver`/`autoupdate`). Template: `packaging/scoop/`.
- **Homebrew**: formula `kingcobra.rb` is attached to every release and **builds from source on the user's Mac**
  (template `packaging/homebrew/`). To offer `brew install owner/kingcobra/kingcobra`, create a repository
  `homebrew-kingcobra`, then set the repository variable `HOMEBREW_TAP=owner/homebrew-kingcobra` and the secret
  `HOMEBREW_TAP_TOKEN` (a token allowed to push there); the `homebrew` job keeps `Formula/kingcobra.rb` up to date.
- **winget**: set variable `WINGET_PUBLISH=true` and secret `WINGET_TOKEN`. The first version of a package has to be
  submitted to microsoft/winget-pkgs by hand; later releases are sent as pull requests by the `winget` job.
- **APT repository on GitHub Pages**: set variable `APT_PAGES=true`, enable Pages (source: GitHub Actions). For signing add
  the secret `APT_GPG_PRIVATE_KEY` and variable `APT_GPG_KEY_ID`. `packaging/debian/make-apt-repo.sh` builds the tree
  (`pool/`, `dists/stable/`, `InRelease`, keyring); it can also be run by hand:
  `GPG_KEY="you@example.com" packaging/debian/make-apt-repo.sh repo dist/kingcobra_*.deb`
  (create a key with `gpg --quick-generate-key "KingCobra Archive <you@example.com>" rsa3072 sign 3y`). Users then:

      sudo install -d /etc/apt/keyrings
      curl -fsSL https://OWNER.github.io/REPO/kingcobra-archive-keyring.gpg | sudo tee /etc/apt/keyrings/kingcobra.gpg >/dev/null
      echo "deb [signed-by=/etc/apt/keyrings/kingcobra.gpg] https://OWNER.github.io/REPO stable main" | sudo tee /etc/apt/sources.list.d/kingcobra.list
      sudo apt update && sudo apt install kingcobra

## GitHub Actions

- **`ci.yml`** (every push and PR): lint (`tools/check-release.sh`, shellcheck, actionlint, installer dry runs); compiles and
  runs the full smoke suite on Linux amd64, arm64 (must pass), armhf, riscv64 (QEMU), Alpine/musl amd64 and arm64, Fedora
  amd64 and arm64 (rpm), macOS arm64 and Intel, Windows x64; and an end-to-end test that serves the fresh packages like
  a release and runs `install.sh` against them (tarball, compile-from-source, deb, uninstall). Entries that have not
  been green yet have `blocking: false`; flip them to `true` once they are.
- **`release.yml`** (tag `vX.Y.Z`, must equal `KC_VERSION`): the same matrix, then installs the `.deb`/`.rpm`/tarballs in
  clean containers (Debian 12, Ubuntu 22.04 / 24.04, Fedora, Rocky 9, Alpine), builds the source tarball, renders the
  manifests, writes `SHA256SUMS`, attests the files, and publishes the Release with notes taken from the matching
  `## X.Y.Z` section of `docs/CHANGELOG.md`. A tag containing `-` (for example `v1.1.0-rc1`) is published as a
  pre-release. Linux amd64/arm64/armhf/riscv64, Windows and the glibc verification must pass; musl, rpm and macOS
  builds are best-effort (if one fails that file is simply missing and the installers compile locally instead).
  Run it by hand (Actions tab -> Release -> Run workflow) to build everything without publishing.

## Cutting a release

1. Bump `KC_VERSION` in `compiler/include/kc_common.h`; add `## X.Y.Z` to `docs/CHANGELOG.md`, a new top entry to
   `debian/changelog`, and update the version in `tests/smoke_test.sh`, `assets/banner.svg` and `index.html`.
2. `tools/check-release.sh` must print "release checks passed".
3. `git tag vX.Y.Z && git push origin vX.Y.Z`. Watch the Release workflow; the files appear on the Release page.

## Building packages by hand

    make deb                           # Debian/Ubuntu, this machine's architecture -> ../kingcobra_<ver>_<arch>.deb
    make rpm                           # Fedora & co. (needs rpm-build) -> dist/kingcobra-<ver>-1.<arch>.rpm
    make dist-src                      # source tarball from git -> dist/
    packaging/ci/make-tarball.sh out.tar.gz [--no-mime]
    packaging/ci/build-in-container.sh linux/arm64 debian:bookworm dist     # the CI build, locally with Docker
    packaging/ci/verify-package.sh linux/amd64 ubuntu:22.04 dist/kingcobra_1.0.0_amd64.deb

Debian: `sudo apt install build-essential dpkg-dev libx11-dev libreadline-dev`. `libx11-dev` and `libreadline-dev` are
optional features (std.gui window backend, REPL history); the package depends on whatever shared libraries the binary
really links (`dpkg-shlibdeps` / rpm's automatic dependencies). Packages install `kingcobra` and `kc`, the man pages, the
`.kc` file type and icons, and docs and examples under `share/doc/kingcobra`.

## Honest status

Verified here (x86-64 Ubuntu): the build with zero compiler warnings, the 1056-check smoke suite, `.deb` and `.rpm`
construction and contents, `install.sh` end to end against a local fake release (tarball, deb, compile-from-source,
automatic fallback when a prebuilt file is missing, uninstall), manifest rendering, and shellcheck / actionlint on every
script and workflow. **Not run here** (no such machines): the ARM, RISC-V, musl, Fedora, macOS and Windows builds, the
attestation, and the Homebrew / winget / Pages publishing steps. They are written to fail safe (see above), but expect
to fix small things the first time a tag is pushed: run **Actions -> Release -> Run workflow** once before the real tag.
