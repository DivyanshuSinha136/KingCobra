# KCOS build -- Author: Divyanshu Sinha (MIT License)
#
#   make -C kcos            build build/kernel.bin and the bootable disk image build/kcos.img
#   make -C kcos run        boot it in QEMU (window)         make -C kcos run-serial   headless, shell on this terminal
#   make -C kcos test       boot it headless and check the shell (needs qemu-system-x86_64 and python3)
#   make -C kcos boot       re-assemble the boot stage from boot/boot.asm (needs nasm; boot/boot.bin is shipped pre-built)
#
# The kernel is compiled by kcc (KingCobra's own compiler); the interpreter must be built first (`make` in the repository root).
KINGCOBRA ?= ../kingcobra
KCC       := $(KINGCOBRA) ../kcc/src/main.kc
BUILD     := build
SRC       := $(wildcard kernel/*.kc)
QEMU      ?= qemu-system-x86_64
QEMUFLAGS ?= -m 128

all: $(BUILD)/kcos.img

$(KINGCOBRA):
	@echo "build the KingCobra interpreter first: run 'make' in the repository root"; false

$(BUILD)/kernel.bin: $(SRC) $(wildcard ../kcc/src/*.kc) | $(KINGCOBRA)
	@mkdir -p $(BUILD)
	$(KCC) kernel/main.kc -o $@ --target kcos --map

$(BUILD)/kcos.img: boot/boot.bin $(BUILD)/kernel.bin tools/mkimage.kc
	$(KINGCOBRA) tools/mkimage.kc boot/boot.bin $(BUILD)/kernel.bin -o $@

boot:
	nasm -f bin boot/boot.asm -o boot/boot.bin

run: $(BUILD)/kcos.img
	$(QEMU) -drive format=raw,file=$(BUILD)/kcos.img $(QEMUFLAGS)

run-serial: $(BUILD)/kcos.img
	$(QEMU) -drive format=raw,file=$(BUILD)/kcos.img $(QEMUFLAGS) -display none -serial stdio

test: $(BUILD)/kcos.img
	python3 tests/smoke.py $(BUILD)/kcos.img

clean:
	rm -rf $(BUILD)

.PHONY: all boot run run-serial test clean
