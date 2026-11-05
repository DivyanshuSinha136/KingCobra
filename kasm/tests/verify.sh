#!/bin/sh
# verify.sh -- end-to-end checks for KASM.  Run from the project root:   sh kasm/tests/verify.sh
# Always: type-check + unit tests.  When GNU binutils / gcc are present: differential encoding tests against `as`,
# and the produced ELF files are run / linked for real (Linux x86-64 only).
set -e
KC="${KINGCOBRA:-./kingcobra}"
fail=0
echo "== type-check"; $KC check kasm/src/*.kc kcc/src/objin.kc kcc/src/link.kc kasm/tests/kasm_test.kc kasm/tools/dump.kc kasm/examples/kasm_rhex.kc
echo "== unit tests"; $KC test kasm/tests/kasm_test.kc
if command -v as >/dev/null 2>&1 && command -v objdump >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
  echo "== differential tests vs GNU as"
  for spec in 64:insns64 32:insns32 16:insns16 64:insns64_vex 64:insns_x87; do
    python3 kasm/tools/diff_gas.py kasm/tests/${spec#*:}.txt ${spec%%:*} | tail -1 || fail=1
  done
else echo "== (skipping differential tests: need as, objdump and python3)"; fi
if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ]; then
  T="$(mktemp -d)"
  echo "== run assembled programs"
  # KASM writes the object / binary, KCC links it
  ./kasm/kasm kasm/examples/hello.kasm -f obj -o "$T/hello.o" >/dev/null && ./kcc/kcc "$T/hello.o" -o "$T/hello" >/dev/null && [ "$("$T/hello")" = "Hello from KASM!" ] && echo "hello ok (kasm .o -> kcc)" || fail=1
  ./kasm/kasm kasm/examples/hello.kasm -f elf -o "$T/hello2" >/dev/null && [ "$("$T/hello2")" = "Hello from KASM!" ] && echo "hello ok (kasm -f elf, linked by kcc)" || fail=1
  ./kasm/kasm kasm/examples/fibonacci.kasm -f obj -o "$T/fib.o" >/dev/null && ./kcc/kcc "$T/fib.o" -o "$T/fib" >/dev/null && [ "$("$T/fib" | tr '\n' ' ')" = "0 1 1 2 3 5 8 13 21 34 " ] && echo "fibonacci ok" || fail=1
  ./kasm/kasm kasm/examples/bootsector.kasm -o "$T/boot.img" >/dev/null && [ "$(wc -c < "$T/boot.img")" -eq 512 ] && echo "bootsector ok (512 bytes)" || fail=1
  printf 'bits 64\nmov eax, 42\nret\n' > "$T/r.kasm"
  ./kasm/kasm "$T/r.kasm" -o "$T/r.bin" >/dev/null && ./kcc/kcc "$T/r.bin" -o "$T/r" >/dev/null; rc=0; "$T/r" || rc=$?; [ $rc -eq 42 ] && echo "raw .bin -> kcc ok (exit 42)" || fail=1
  ./kasm/kasm kasm/examples/calls_libc.kasm -f obj -o "$T/cl.o" >/dev/null && ./kcc/kcc "$T/cl.o" -o "$T/cl" >/dev/null && [ "$("$T/cl")" = "12 squared is 144" ] && echo "libc object ok (kcc executable)" || fail=1
  if command -v gcc >/dev/null 2>&1; then
    gcc -no-pie "$T/cl.o" -o "$T/cl_gcc" && [ "$("$T/cl_gcc")" = "12 squared is 144" ] && echo "libc object ok (gcc)" || fail=1
    printf '#include <stdio.h>\nint main(void){puts("from gcc");return 3;}\n' > "$T/g.c"; gcc -O1 -fPIC -c "$T/g.c" -o "$T/g.o" && ./kcc/kcc "$T/g.o" -o "$T/g" >/dev/null; rc=0; "$T/g" >/dev/null || rc=$?; [ $rc -eq 3 ] && echo "gcc object -> kcc ok" || fail=1
  fi
  $KC kasm/examples/kasm_rhex.kc | head -2
  rm -rf "$T"
fi
[ $fail -eq 0 ] && echo "kasm: all checks passed" || { echo "kasm: FAILED"; exit 1; }
