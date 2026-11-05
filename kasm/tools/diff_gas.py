#!/usr/bin/env python3
"""Differential test: assemble each line of a file with KASM and with GNU as (Intel syntax) and compare.
usage: diff_gas.py FILE [BITS]   (BITS = 16|32|64, default 64).  Differences are accepted when both encodings
disassemble to the same text (objdump), i.e. KASM chose a different but equivalent encoding."""
import subprocess, sys, os, tempfile, re
f = sys.argv[1]; bits = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 64
here = os.path.dirname(os.path.abspath(__file__))
kc = os.path.join(here, "..", "..", "kingcobra")
lines = [l.rstrip("\n") for l in open(f)]
out = subprocess.run([kc, os.path.join(here, "dump.kc"), f, str(bits)], capture_output=True, text=True)
kasm = out.stdout.split("\n")
if out.returncode: print(out.stderr[:500])
tmp = tempfile.mkdtemp()
def gas(line):
    code = {16: ".code16", 32: ".code32", 64: ".code64"}[bits]
    open(tmp + "/t.s", "w").write(".intel_syntax noprefix\n" + code + "\n" + line + "\n")
    r = subprocess.run(["as", "--" + ("64" if bits == 64 else "32"), "-o", tmp + "/t.o", tmp + "/t.s"], capture_output=True, text=True)
    if r.returncode: return None
    subprocess.run(["objcopy", "-O", "binary", "-j", ".text", tmp + "/t.o", tmp + "/t.bin"])
    return open(tmp + "/t.bin", "rb").read().hex()
def dis(hexs):
    open(tmp + "/d.bin", "wb").write(bytes.fromhex(hexs))
    m = {16: "i8086", 32: "i386", 64: "i386:x86-64"}[bits]
    r = subprocess.run(["objdump", "-D", "-b", "binary", "-m", m, "-M", "intel", tmp + "/d.bin"], capture_output=True, text=True)
    rows = [re.sub(r"\s+", " ", x.split("\t", 2)[-1]).strip() for x in r.stdout.split("\n") if re.match(r"\s+[0-9a-f]+:\t", x)]
    return " ; ".join(rows)
if "--gen" in sys.argv:
    # write the GNU as encodings next to the corpus as golden data for tests/kasm_test.kc
    with open(f.replace(".txt", ".hex"), "w") as o:
        for l in lines:
            if not l.strip() or l.startswith("#"): o.write("\n"); continue
            g = gas(l)
            o.write((g if g is not None else "-") + "\n")
    print("wrote", f.replace(".txt", ".hex")); sys.exit(0)
ok = same = bad = skipped = 0
for i, l in enumerate(lines):
    if not l.strip() or l.startswith("#"): continue
    k = kasm[i] if i < len(kasm) else "?"
    g = gas(l)
    if g is None:
        skipped += 1
        if not k.startswith("ERR"): print(f"  GAS rejects, KASM accepts: {l!r} -> {k}")
        continue
    if k == g: ok += 1; continue
    if k.startswith("ERR"):
        bad += 1; print(f"FAIL {l!r}: {k}   (gas {g})"); continue
    try:
        dk, dg = dis(k), dis(g)
    except Exception as e:
        dk = dg = None
    if dk is not None and dk == dg:
        same += 1; print(f"  equiv {l!r}: kasm {k} / gas {g}  [{dk}]")
    else:
        bad += 1; print(f"DIFF {l!r}: kasm {k} [{dk}] / gas {g} [{dg}]")
print(f"{ok} identical, {same} equivalent, {bad} wrong, {skipped} rejected by gas")
sys.exit(1 if bad else 0)
