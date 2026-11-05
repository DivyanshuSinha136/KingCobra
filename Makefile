# KingCobra build — Author: Divyanshu Sinha (MIT License)
#
# Linux/macOS: builds directly. Supported CPUs: x86-64 (interpreter + closure JIT),
# ARM64 / ARMhf / RISC-V / x86 (interpreter; the JIT is x86-64 only). See docs/PLATFORMS.md.
# Cross-compiling: `make arm64`, `make armhf`, `make riscv64` (need the matching
# gcc cross toolchain), or pass CC=<triple>-gcc yourself. Objects are kept per
# target triple, so native and cross builds never mix.
# Windows: tested with MSYS2/MinGW-w64 (`make` + `gcc` on PATH). Native
# MSVC (`cl`/`nmake`) is not supported by this Makefile; use MinGW.
CC       ?= gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wno-unused-parameter
OPT      := -O2
INCLUDES := -Icompiler/include
# `char` is signed on x86 but UNSIGNED by default on ARM and RISC-V. KingCobra source is written for signed
# char; pin it so every architecture runs the same program the same way.
ARCHFLAGS := -fsigned-char
BIN      := kingcobra
# extra linker flags, e.g. `make LDFLAGS=-static` (the Windows release is linked statically)
LDFLAGS  ?=

ifeq ($(OS),Windows_NT)
  # __USE_MINGW_ANSI_STDIO: MinGW's default msvcrt printf lacks %zu/%lld, which several modules use in messages
  DEFS := -D_GNU_SOURCE -DWIN32_LEAN_AND_MEAN -D__USE_MINGW_ANSI_STDIO=1
  LIBS := -lm -lws2_32 -lgdi32 -luser32          # no -ldl: kc_platform.h uses LoadLibrary/GetProcAddress instead
  BIN  := kingcobra.exe
else
  # _FILE_OFFSET_BITS=64: off_t must be 64-bit (std.mmio maps physical addresses); it already is on 64-bit
  # hosts, but 32-bit ARM / RISC-V / x86 default to a 32-bit off_t. On 32-bit targets also ask glibc for a
  # 64-bit time_t (ignored by older glibc and by musl, which is already 64-bit).
  DEFS := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
  ifeq ($(shell echo __SIZEOF_POINTER__ | $(CC) -E -P - 2>/dev/null | tail -n1),4)
    DEFS += -D_TIME_BITS=64
  endif
  LIBS := -lm -ldl
  # std.gui: use X11 when its headers are installed; otherwise the module
  # still builds and runs headless (see docs/STD_GUI.md).
  # The headers are looked up with the TARGET compiler ($(CC) -E), not by hard-coded host paths, so a cross
  # build only links X11 / readline when the cross sysroot really has them. `make KC_NO_X11=1` forces headless.
  ifndef KC_NO_X11
    ifeq ($(shell printf '\043include <X11/Xlib.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes),yes)
      DEFS += -DKC_GUI_X11
      LIBS += -lX11
    endif
  endif
  # REPL line editing / history / Tab completion: use GNU readline when its
  # headers are installed; otherwise the REPL falls back to plain stdin.
  # Disable explicitly with `make KC_NO_READLINE=1`. (macOS: its readline is
  # libedit; install GNU readline and pass CPPFLAGS/LDFLAGS, or skip.)
  ifndef KC_NO_READLINE
    ifeq ($(shell printf '\043include <readline/readline.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes),yes)
      ifneq ($(shell uname -s),Darwin)
        DEFS += -DKC_READLINE
        LIBS += -lreadline
      endif
    endif
  endif
endif

SRC := $(wildcard compiler/src/*.c) \
       std/system/system.c \
       std/system/system_info.c \
       std/system/system_path.c \
       std/system/system_time.c \
       std/system/system_proc.c \
       std/memory/memory.c \
       std/network/network.c \
       std/http/http.c \
       std/http/http_url.c \
       std/http/http_inflate.c \
       std/http/http_tls.c \
       std/http/http_conn.c \
       std/http/http_client.c \
       std/http/http_server.c \
       std/rhex/rhex.c \
       std/rhex/rhex_buf.c \
       std/rhex/rhex_call.c \
       std/rhex/rhex_cpu.c \
       std/rhex/rhex_hw.c \
       std/music_note/music_note.c \
       std/music_note/mn_score.c \
       std/music_note/mn_synth.c \
       std/music_note/mn_dsp.c \
       std/music_note/mn_analysis.c \
       std/music_note/mn_io.c \
       std/music_note/mn_groove.c \
       std/io/io.c \
       std/vector/vector.c \
       std/regx/regx.c \
       std/nlp/nlp.c \
       std/nn/nn.c \
       std/nn/nn_core.c \
       std/nn/nn_tensor.c \
       std/nn/nn_math.c \
       std/nn/nn_linalg.c \
       std/nn/nn_layers.c \
       std/nn/nn_conv.c \
       std/nn/nn_rnn.c \
       std/nn/nn_attn.c \
       std/nn/nn_model.c \
       std/nn/nn_loss.c \
       std/nn/nn_optim.c \
       std/nn/nn_train.c \
       std/nn/nn_io.c \
       std/nn/nn_api_tensor.c \
       std/nn/nn_api_model.c \
       std/nn/nn_api_math.c \
       std/nn/nn_api_linalg.c \
       std/gui/gui.c \
       std/game/game.c \
       std/game/game_util.c \
       std/game/game_gfx.c \
       std/game/game_model.c \
       std/game/game_modelio.c \
       std/game/game_r3d.c \
       std/game/game_world.c \
       std/game/game_scene.c \
       std/game/game_hud.c \
       std/ascii_screen/as_util.c \
       std/ascii_screen/as_cell.c \
       std/ascii_screen/as_term.c \
       std/ascii_screen/as_image.c \
       std/ascii_screen/as_render.c \
       std/ascii_screen/as_svg.c \
       std/ascii_screen/as_font.c \
       std/ascii_screen/as_anim.c \
       std/ascii_screen/as_hud.c \
       std/ascii_screen/as_map.c \
       std/ascii_screen/as_game.c \
       std/ascii_screen/as_api_core.c \
       std/ascii_screen/as_api_draw.c \
       std/ascii_screen/as_api_media.c \
       std/ascii_screen/as_api_game.c \
       std/ascii_screen/as_api.c \
       std/mmio/mmio.c \
       std/acpi/acpi.c \
       std/json/json.c \
       std/json/json_ops.c \
       std/json/json_query.c \
       std/json/json_schema.c \
       std/csv/csv.c \
       std/fbin/fbin.c \
       std/mmu/mmu.c \
       std/mmu/mmu_arch.c \
       std/mmu/mmu_phys.c \
       std/mmu/mmu_pt.c \
       std/mmu/mmu_cpu.c \
       std/mmu/mmu_vm.c \
       std/mmu/mmu_api.c \
       std/mmu/mmu_api_vm.c \
       std/mmu/mmu_host.c \
       std/tts/tts.c \
       std/tts/tts_phon.c \
       std/tts/tts_dict.c \
       std/tts/tts_g2p.c \
       std/tts/tts_text.c \
       std/tts/tts_plan.c \
       std/tts/tts_synth.c \
       std/tts/tts_style.c

# Objects live in build/<version>/ so that a new release can NEVER be linked against objects compiled from an
# older one (a mix of old objects and new headers shows up as garbage argument counts, bad structs and silent
# crashes - most often when a zip is unpacked over an existing tree whose timestamps look newer).
KCVER  := $(strip $(shell sed -n 's/.*define KC_VERSION "\([^"]*\)".*/\1/p' compiler/include/kc_common.h))
ifeq ($(KCVER),)
  KCVER := dev
endif
# ...and per target triple, so `make arm64` after a native build (or the reverse) can never mix architectures.
TRIPLE := $(strip $(shell $(CC) -dumpmachine 2>/dev/null))
ifeq ($(TRIPLE),)
  TRIPLE := native
endif
OBJDIR := build/$(KCVER)/$(TRIPLE)
OBJ    := $(patsubst %.c,$(OBJDIR)/%.o,$(SRC))

# Objects are also rebuilt whenever the compiler, the flags or the detected features (X11, readline) change.
# Without this, installing libx11-dev / libreadline-dev after a first build would silently leave objects compiled
# without them mixed with objects compiled with them.
export KC_FLAGS := $(CC) $(CSTD) $(ARCHFLAGS) $(WARN) $(OPT) $(DEFS) $(INCLUDES) $(LDFLAGS) $(LIBS)
FLAGS_STAMP := $(OBJDIR)/.flags
.PHONY: FORCE
FORCE:
$(FLAGS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s' "$$KC_FLAGS" | cmp -s - $@ || printf '%s' "$$KC_FLAGS" > $@
$(OBJ): $(FLAGS_STAMP)

# The first rule in this file would become the default goal; make it explicit so that `make` always builds the program.
.DEFAULT_GOAL := all

# Windows: embed assets/icons/kingcobra.ico as the icon of kingcobra.exe (needs windres, part of MinGW-w64)
ifeq ($(OS),Windows_NT)
  WINDRES ?= windres
  RES     := packaging/windows/kingcobra_res.o
  OBJ     += $(RES)
$(RES): packaging/windows/kingcobra.rc assets/icons/kingcobra.ico
	$(WINDRES) -i $< -o $@ -O coff
endif

.PHONY: deb rpm arm64 armhf riscv64 ide ide-test all clean test showcase install install-bin install-man install-docs uninstall pgo icons install-mime uninstall-mime install-mime-user install-windows version check dist-src

all: $(BIN)

# Cross builds. Debian/Ubuntu packages: gcc-aarch64-linux-gnu, gcc-arm-linux-gnueabihf, gcc-riscv64-linux-gnu.
# Override the compiler with e.g. `make arm64 CROSS_ARM64=aarch64-none-linux-gnu-gcc`.
CROSS_ARM64   ?= aarch64-linux-gnu-gcc
CROSS_ARMHF   ?= arm-linux-gnueabihf-gcc
CROSS_RISCV64 ?= riscv64-linux-gnu-gcc
# Debian/Ubuntu package for THIS machine's architecture -> ../kingcobra_<version>_<arch>.deb  (see docs/PLATFORMS.md)
deb:
	dpkg-buildpackage -us -uc -b
arm64:
	$(MAKE) CC=$(CROSS_ARM64) BIN=kingcobra-arm64
armhf:
	$(MAKE) CC=$(CROSS_ARMHF) BIN=kingcobra-armhf
riscv64:
	$(MAKE) CC=$(CROSS_RISCV64) BIN=kingcobra-riscv64

$(BIN): $(OBJ)
	$(CC) $(CSTD) $(OPT) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

# -MMD -MP: record header dependencies so editing a .h rebuilds every .c that includes it
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(ARCHFLAGS) $(WARN) $(OPT) $(DEFS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

# std.nn is numeric inner-loop code: -O3/unrolling gives ~1.7x on training. (No -ffast-math: NaN/divergence
# detection and bit-exact save/load round trips rely on strict IEEE semantics.)
$(OBJDIR)/std/nn/%.o: OPT := -O3 -funroll-loops

# std.game is a software rasteriser: its inner loops want -O3
$(OBJDIR)/std/game/%.o: OPT := -O3

# std.ascii_screen: image resampling / SVG raster / ASCII renderers are per-pixel loops
$(OBJDIR)/std/ascii_screen/%.o: OPT := -O3

# Profile-guided build: compile an instrumented interpreter, run a training workload, then rebuild
# using the recorded profile so GCC lays out and inlines the hot interpreter paths. Typically
# 10-25% faster than a plain -O2 build. Needs GCC; the profile is machine-local and not committed.
pgo:
	$(MAKE) clean
	find . -name '*.gcda' -delete
	$(MAKE) OPT="$(OPT) -fprofile-generate -fprofile-update=single"
	bash tests/pgo_train.sh ./$(BIN)
	rm -rf build
	rm -f $(BIN)
	$(MAKE) OPT="$(OPT) -fprofile-use -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
	find . -name '*.gcda' -delete
	@echo "pgo build complete: ./$(BIN)"

clean:
	rm -rf build
	rm -f kingcobra kingcobra.exe packaging/windows/kingcobra_res.o
	-find compiler std -name '*.o' -delete -o -name '*.d' -delete 2>/dev/null
	rm -f examples/native/mathutil.o examples/native/mathutil.kcnative.so examples/native/mathutil.kcnative.dll

# examples/native/mathutil.o is a compiled binary and deliberately NOT
# committed to source control — it's platform-specific (and the native
# import feature it demonstrates re-links it into a shared library on
# every run anyway, so shipping a prebuilt one would be stale on arrival
# for anyone not on this exact OS/arch). `make test` builds it fresh.
examples/native/mathutil.o: examples/native/mathutil.c
	$(CC) $(CSTD) -fPIC -c $< -o $@

test: all examples/native/mathutil.o
	@for f in examples/*.kc; do \
		echo "--- $$f ---"; \
		./$(BIN) run $$f || exit 1; \
	done

showcase: all
	@for f in examples/showcase/0*.kc; do \
		echo "--- $$f ---"; \
		./$(BIN) run $$f || exit 1; \
	done

# ---------------------------------------------------------------------------------------------
# Install.  PREFIX/DESTDIR work as usual:   make install PREFIX=/usr   |   make install PREFIX=$HOME/.local
# On Linux/BSD, `install` also registers the *.kc file type, so every .kc file shows the KingCobra
# icon in file managers (see install-mime). Windows: see install-windows.
# ---------------------------------------------------------------------------------------------
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
ICON_SIZES := 16x16 24x24 32x32 48x48 64x64 128x128 256x256 512x512

ifeq ($(OS),Windows_NT)
install: all
	@echo "Windows: copy $(BIN) somewhere on PATH, then run 'make install-windows' to give .kc files the KingCobra icon."
else
install: install-bin install-man install-mime
endif

# Just the program (+ the `kc` short name). Used by the Homebrew formula, the macOS tarball and Scoop-style installs,
# where there is no freedesktop MIME database to update.
install-bin: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/kingcobra
	ln -sf kingcobra $(DESTDIR)$(BINDIR)/kc

# man pages: kingcobra(1) and kc(1) (kc is the short name of the same program)
install-man:
	install -d $(DESTDIR)$(DATADIR)/man/man1
	install -m 644 packaging/debian/kingcobra.1 $(DESTDIR)$(DATADIR)/man/man1/kingcobra.1
	ln -sf kingcobra.1 $(DESTDIR)$(DATADIR)/man/man1/kc.1

# README, LICENSE, docs/ and examples/ under share/doc/kingcobra (the .deb/.rpm put them in the same place)
install-docs:
	install -d $(DESTDIR)$(DATADIR)/doc/kingcobra
	install -m 644 README.md LICENSE $(DESTDIR)$(DATADIR)/doc/kingcobra/
	cp -R docs examples $(DESTDIR)$(DATADIR)/doc/kingcobra/
	find $(DESTDIR)$(DATADIR)/doc/kingcobra \( -name '*.o' -o -name '*.so' -o -name '*.dll' \) -exec rm -f {} +

uninstall: uninstall-mime
	rm -f $(DESTDIR)$(BINDIR)/kingcobra $(DESTDIR)$(BINDIR)/kc
	rm -f $(DESTDIR)$(DATADIR)/man/man1/kingcobra.1 $(DESTDIR)$(DATADIR)/man/man1/kc.1
	rm -rf $(DESTDIR)$(DATADIR)/doc/kingcobra

# Linux/BSD: file type (text/x-kingcobra, glob *.kc) + the logo as its icon, in the standard icon-theme layout.
# The cache refresh tools are optional; a missing one is not an error (DESTDIR/package builds skip them).
install-mime:
	install -d $(DESTDIR)$(DATADIR)/mime/packages
	install -m 644 packaging/linux/kingcobra.xml $(DESTDIR)$(DATADIR)/mime/packages/kingcobra.xml
	@for s in $(ICON_SIZES); do \
		for kind in mimetypes:text-x-kingcobra apps:kingcobra; do \
			dir=$${kind%%:*}; name=$${kind##*:}; \
			install -d $(DESTDIR)$(DATADIR)/icons/hicolor/$$s/$$dir; \
			install -m 644 assets/icons/hicolor/$$s/$$dir/$$name.png $(DESTDIR)$(DATADIR)/icons/hicolor/$$s/$$dir/$$name.png; \
		done; \
	done
	@if [ -z "$(DESTDIR)" ]; then \
		(update-mime-database $(DATADIR)/mime >/dev/null 2>&1 || echo "note: update-mime-database not found; run it on $(DATADIR)/mime"); \
		(gtk-update-icon-cache -q -t -f $(DATADIR)/icons/hicolor >/dev/null 2>&1 || true); \
		echo "KingCobra file type and icon installed under $(DATADIR) (restart your file manager if the icon does not appear)"; \
	fi

uninstall-mime:
	rm -f $(DESTDIR)$(DATADIR)/mime/packages/kingcobra.xml
	@for s in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/$$s/mimetypes/text-x-kingcobra.png $(DESTDIR)$(DATADIR)/icons/hicolor/$$s/apps/kingcobra.png; \
	done
	@if [ -z "$(DESTDIR)" ]; then \
		(update-mime-database $(DATADIR)/mime >/dev/null 2>&1 || true); \
		(gtk-update-icon-cache -q -t -f $(DATADIR)/icons/hicolor >/dev/null 2>&1 || true); \
	fi

# no root needed: installs the file type + icon for the current user only
install-mime-user:
	$(MAKE) install-mime DATADIR=$(HOME)/.local/share

# Windows (MinGW): associate .kc with kingcobra.exe for the current user and show the logo on every .kc file
install-windows: all
	powershell -NoProfile -ExecutionPolicy Bypass -File packaging/windows/associate.ps1 -Exe $(BIN) -Icon assets/icons/kingcobra.ico

# Regenerate every icon from assets/icons/kingcobra-logo.jpg (needs python3 + Pillow; the results are committed)
icons:
	python3 tools/make_icons.py

# KingCobra IDE (kcide) - terminal IDE written in C11, see docs/IDE.md
ide:
	$(MAKE) -C ide
ide-test:
	$(MAKE) -C ide test

.PHONY: test-kcc
test-kcc: $(BIN)
	sh kcc/tests/verify.sh

# KCOS (operating system written in KingCobra): `make kcos` builds kcos/build/kcos.img, `make kcos-run` boots it in QEMU
.PHONY: kcos kcos-run kcos-test
kcos: all
	$(MAKE) -C kcos
kcos-run: all
	$(MAKE) -C kcos run
kcos-test: all
	$(MAKE) -C kcos test

.PHONY: test-kasm
test-kasm: $(BIN)
	sh kasm/tests/verify.sh

# ---------------------------------------------------------------------------------------------
# Release helpers (used by .github/workflows and by hand)
# ---------------------------------------------------------------------------------------------
# print the version compiled into the program (the single source of truth for tags and package versions)
version:
	@echo $(KCVER)

# build + the full smoke test suite
check: all examples/native/mathutil.o
	bash tests/smoke_test.sh

# kingcobra-<version>-src.tar.gz in dist/ (needs a git checkout; honours export-ignore in .gitattributes).
# This is the file `install.sh` builds from on CPUs / systems that have no prebuilt package.
dist-src:
	mkdir -p dist
	git archive --format=tar.gz --prefix=kingcobra-$(KCVER)/ -o dist/kingcobra-$(KCVER)-src.tar.gz HEAD
	@echo "dist/kingcobra-$(KCVER)-src.tar.gz"

# RPM for THIS machine's architecture -> dist/kingcobra-<version>-1.<arch>.rpm  (needs rpmbuild; see packaging/rpm)
rpm:
	bash packaging/rpm/build-rpm.sh dist
