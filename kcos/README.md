# KCOS

An operating system written in KingCobra. Build with `make -C kcos` (after `make` in the repository root), boot with `make -C kcos run`.
Full documentation: [docs/KCOS.md](../docs/KCOS.md).

```
boot/boot.asm   BIOS boot stage (NASM); boot/boot.bin is the pre-assembled result
kernel/*.kc     the kernel, compiled by `kcc --target kcos`
tools/mkimage.kc  builds build/kcos.img
tests/smoke.py  headless QEMU test
```
