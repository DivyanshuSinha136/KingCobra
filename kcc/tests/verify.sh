#!/bin/sh
# verify.sh -- build the example programs for every target and check them with independent tools.
#   Linux  : .o linked by gcc and run; .so loaded by ctypes (and linked against by gcc)
#   Windows: .obj linked by mingw-w64 (if installed); .dll loaded and called under wine (if installed)
#   Executables: ELF run directly; .exe run through a mini PE loader (tests/harness_exe_win.c) or wine; Mach-O parsed by LIEF
#   macOS  : .o and .dylib parsed with LIEF (python3 -m pip install lief); cannot be executed on Linux
cd "$(dirname "$0")/../.." || exit 1
KC=./kingcobra; MAIN=kcc/src/main.kc; OUT=${TMPDIR:-/tmp}/kcc_verify; mkdir -p "$OUT"; fail=0
ok()  { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fail=1; }

echo "== Linux"
$KC $MAIN kcc/examples/showcase.kc -o $OUT/show.o   >/dev/null && gcc kcc/tests/harness_showcase.c $OUT/show.o -o $OUT/h_show -lm \
  && $OUT/h_show | grep -q "add 42 many 72.5 fib 6765" && ok "showcase.o linked with gcc, results correct" || bad "showcase.o"
$KC $MAIN kcc/examples/showcase.kc -o $OUT/show.so  >/dev/null && gcc kcc/tests/harness_showcase.c $OUT/show.so -o $OUT/h_show_so -lm -Wl,-rpath,$OUT \
  && $OUT/h_show_so | grep -q "u64div 6148914691236517205" && ok "showcase.so (native linker) linked + run" || bad "showcase.so"
$KC $MAIN kcc/examples/features.kc -o $OUT/feat.so  >/dev/null && (cd $OUT && python3 "$OLDPWD/kcc/tests/harness_features.py" | grep -q "ptr 3055 expect 3055 sizes 32808004 big 3") \
  && ok "features.so loaded by ctypes, all checks match" || bad "features.so"

echo "== Windows"
$KC $MAIN kcc/examples/showcase.kc -o $OUT/show.obj --target windows >/dev/null && ok "showcase.obj written" || bad "showcase.obj"
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  x86_64-w64-mingw32-gcc -shared $OUT/show.obj -o $OUT/show_mingw.dll && ok "mingw-w64 linked our COFF object" || bad "mingw link of .obj"
else echo "  skip  mingw-w64 not installed"; fi
$KC $MAIN kcc/examples/showcase.kc -o $OUT/show_kcc.dll >/dev/null && ok "show_kcc.dll written (native PE linker)" || bad "dll"
WINE=$(command -v wine64 || ls /usr/lib/wine/wine64 2>/dev/null)
if [ -n "$WINE" ] && command -v x86_64-w64-mingw32-gcc >/dev/null; then
  x86_64-w64-mingw32-gcc kcc/tests/harness_win.c -o $OUT/wt.exe && (cd $OUT && WINEDEBUG=-all WINEPREFIX=$OUT/wineprefix $WINE ./wt.exe 2>/dev/null | grep -q "add 42 fib 6765 hyp 5.0 many 72.5 greet 16 list 571 ptrs 2404084") \
    && ok "DLL loaded and called under wine, results correct" || bad "dll under wine"
else echo "  skip  wine / mingw-w64 not installed"; fi

echo "== macOS (structural check only)"
$KC $MAIN kcc/examples/showcase.kc -o $OUT/show.dylib >/dev/null && $KC $MAIN kcc/examples/showcase.kc -o $OUT/show_m.o --target macos >/dev/null && ok "show.dylib / show_m.o written" || bad "macho"
python3 - "$OUT" <<'PY' && ok "LIEF parses dylib: 32 exports, 2 binds, load commands valid" || bad "LIEF check"
import sys
try: import lief
except ImportError: print("  skip  lief not installed"); sys.exit(0)
b = lief.parse(sys.argv[1] + "/show.dylib")
assert len(list(b.exported_symbols)) == 32, "exports"
assert sorted(s.name for s in b.imported_symbols) == ["_printf", "_strlen"], "imports"
assert len(list(b.dyld_info.bindings)) == 2, "binds"
assert lief.parse(sys.argv[1] + "/show_m.o").header.file_type == lief.MachO.Header.FILE_TYPE.OBJECT
PY

echo "== Executables"
$KC $MAIN kcc/examples/hello.kc -o $OUT/hello_x >/dev/null && ok "Linux ELF executable written" || bad "linux exe"
if [ -x $OUT/hello_x ]; then
  got=$($OUT/hello_x a b | tr '\n' '|'); rc=$($OUT/hello_x >/dev/null; echo $?)
  [ "$got" = "hello from a kcc executable|argc=3 fib(20)=6765 counter=42|argv[1]=a|" ] && [ "$rc" = 7 ] \
    && ok "Linux executable runs: output, argc/argv and exit code correct" || bad "linux exe run ($got rc=$rc)"
  [ "$($OUT/hello_x | wc -l)" = 2 ] && ok "stdio is flushed when piped (exit goes through libc)" || bad "stdout flush when piped"
fi
$KC $MAIN kcc/examples/nolibc.kc -o $OUT/nolibc_x >/dev/null && $OUT/nolibc_x one; rc=$?
[ "$rc" = 32 ] && ok "libc-free executable runs (raw syscalls), exit code 32" || bad "nolibc exe (rc=$rc)"
if command -v readelf >/dev/null; then
  [ "$(readelf -d $OUT/nolibc_x | grep -c NEEDED)" = 0 ] && readelf -d $OUT/hello_x | grep -q "libc.so.6" \
    && ok "readelf: nolibc has no DT_NEEDED, hello needs libc.so.6" || bad "readelf NEEDED"
fi
$KC $MAIN kcc/examples/hello.kc -o $OUT/hello_w.exe >/dev/null && ok "hello.exe written (PE32+ executable)" || bad "exe"
$KC $MAIN kcc/examples/hello.kc -o $OUT/hello_gui.exe --subsystem windows >/dev/null && ok "GUI-subsystem .exe written" || bad "gui exe"
gcc kcc/tests/harness_exe_win.c -o $OUT/h_exe 2>/dev/null && {
  got=$($OUT/h_exe $OUT/hello_w.exe a 2>/dev/null | tr '\n' '|'); $OUT/h_exe $OUT/hello_w.exe >/dev/null 2>&1; rc=$?
  [ "$got" = "hello from a kcc executable|argc=2 fib(20)=6765 counter=42|argv[1]=a|" ] && [ "$rc" = 7 ] \
    && ok ".exe entry stub, relocations and imports run (mini PE loader, ms_abi shims): output + exit code correct" || bad ".exe in PE harness ($got rc=$rc)"
} || echo "  skip  gcc cannot build the PE harness"
WINE=$(command -v wine64 || command -v wine || ls /usr/lib/wine/wine64 2>/dev/null)
if [ -n "$WINE" ]; then
  got=$(cd $OUT && WINEDEBUG=-all WINEPREFIX=$OUT/wineprefix $WINE ./hello_w.exe a 2>/dev/null | tr -d '\r' | tr '\n' '|')
  [ "$got" = "hello from a kcc executable|argc=2 fib(20)=6765 counter=42|argv[1]=a|" ] && ok ".exe run natively under wine" || bad ".exe under wine ($got)"
else echo "  skip  wine not installed (the .exe was run through the mini PE loader instead)"; fi
$KC $MAIN kcc/examples/hello.kc -o $OUT/hello_m --target macos >/dev/null && ok "macOS Mach-O executable written" || bad "macho exe"
python3 - "$OUT" <<'PY' && ok "LIEF parses PE exe + Mach-O exe (entry, imports, segments)" || bad "LIEF exe check"
import sys
try: import lief
except ImportError: print("  skip  lief not installed"); sys.exit(0)
o = sys.argv[1]
p = lief.parse(o + "/hello_w.exe")
assert p.optional_header.addressof_entrypoint > 0 and not p.header.has_characteristic(lief.PE.Header.CHARACTERISTICS.DLL)
assert p.optional_header.subsystem == lief.PE.OptionalHeader.SUBSYSTEM.WINDOWS_CUI
assert lief.parse(o + "/hello_gui.exe").optional_header.subsystem == lief.PE.OptionalHeader.SUBSYSTEM.WINDOWS_GUI
assert {e.name for i in p.imports for e in i.entries} == {"puts", "printf", "__getmainargs", "exit"}
m = lief.parse(o + "/hello_m")
assert m.header.file_type == lief.MachO.Header.FILE_TYPE.EXECUTE and m.has_main_command
assert [s.name for s in m.segments][:2] == ["__PAGEZERO", "__TEXT"]
assert sorted(b.symbol.name for b in m.dyld_info.bindings) == ["_printf", "_puts"]
assert [l.name for l in m.libraries] == ["/usr/lib/libSystem.B.dylib"]
PY

echo "== KingCobra test suite"
$KC test kcc/tests/kcc_test.kc | tail -1 | grep -q " 0 failed" && ok "kcc_test.kc" || bad "kcc_test.kc"
echo "== grammar programs (compile, run, compare with golden output)"
sh kcc/tests/grammar/run.sh >/tmp/kcc_g.$$ 2>&1 && ok "grammar programs g01..g07 (switch, match, goto, defer/try, generics, impl, closures, ...)" || { cat /tmp/kcc_g.$$; bad "grammar programs"; }
rm -f /tmp/kcc_g.$$
echo "== struct-by-value ABI (needs gcc)"
sh kcc/tests/abi/run.sh >/tmp/kcc_a.$$ 2>&1 && ok "struct ABI interop: $(grep -c '^ok' /tmp/kcc_a.$$) of 2 ABIs checked ($(grep -c skip /tmp/kcc_a.$$) skipped)" || { cat /tmp/kcc_a.$$; bad "struct ABI interop"; }
rm -f /tmp/kcc_a.$$
[ $fail = 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail
