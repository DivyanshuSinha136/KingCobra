#!/bin/sh
# Struct-by-value ABI interop: kcc-compiled functions called from gcc-compiled C and back, for the SysV ABI and
# (through gcc's ms_abi attribute, using KCC_FORCE_ABI=win64) the Microsoft x64 ABI. Needs gcc.
cd "$(dirname "$0")/../../.." || exit 1
command -v gcc >/dev/null || { echo "skip  gcc not installed"; exit 0; }
d=$(mktemp -d); fail=0
./kingcobra kcc/src/main.kc kcc/tests/abi/structs.kc -o $d/lib.so >/dev/null || exit 1
gcc kcc/tests/abi/structs_main.c $d/lib.so -o $d/m -Wl,-rpath,$d && $d/m > $d/sysv.txt
KCC_FORCE_ABI=win64 ./kingcobra kcc/src/main.kc kcc/tests/abi/structs.kc -o $d/libms.so >/dev/null || exit 1
gcc kcc/tests/abi/structs_main_ms.c $d/libms.so -o $d/mm -Wl,-rpath,$d && $d/mm > $d/ms.txt
exp="13 8
2 6
10 5
6 2 8
201
1.75 10
62
111 36 -0.25"
[ "$(cat $d/sysv.txt)" = "$exp" ] && echo "ok   SysV struct ABI" || { echo "FAIL SysV struct ABI"; fail=1; }
[ "$(cat $d/ms.txt)" = "$exp" ] && echo "ok   Win64 struct ABI" || { echo "FAIL Win64 struct ABI"; fail=1; }
rm -rf $d; exit $fail
