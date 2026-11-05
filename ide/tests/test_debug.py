#!/usr/bin/env python3
"""Protocol test for `kingcobra debug`: breakpoints, stepping, stack, variables, eval, stop-on-exception.
Usage: test_debug.py [path/to/kingcobra]   (stdlib only; skipped on platforms without os.mkfifo)"""
import os, select, subprocess, sys, tempfile, time
if not hasattr(os, "mkfifo"): print("skip  debugger protocol (no mkfifo)"); sys.exit(0)
KC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "kingcobra")
KC = os.path.abspath(KC)
if not os.path.exists(KC): print("skip  debugger protocol (kingcobra not built)"); sys.exit(0)
SRC = '''struct P { x: int, y: int }
let G = 100;
fn add(a: int, b: int) -> int {
    let s = a + b;
    return s * 2;
}
fn main() {
    let p = P { x: 1, y: 2 };
    let total = 0;
    for (i in 0..3) {
        total = total + add(i, p.x);
    }
    print("total", total);
    let z = 1 / 0;
}
'''
d = tempfile.mkdtemp(); src = os.path.join(d, "t.kc"); open(src, "w").write(SRC); base = os.path.join(d, "p")
for e in (".cmd", ".evt"): os.mkfifo(base + e)
evt = os.open(base + ".evt", os.O_RDONLY | os.O_NONBLOCK); cmd = os.open(base + ".cmd", os.O_RDWR | os.O_NONBLOCK)
p = subprocess.Popen([KC, "debug", src], env=dict(os.environ, KC_DEBUG_PIPE=base), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
buf = b""
def send(s): os.write(cmd, (s + "\n").encode())
def until(prefixes, t=5.0):
    global buf; out = []; end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([evt], [], [], 0.05)
        if r:
            c = os.read(evt, 65536)
            if c: buf += c
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1); out.append(l.decode())
            if l.decode().startswith(prefixes) or l.decode() == "end": return out
    raise SystemExit("FAIL timeout waiting for %r, got %r" % (prefixes, out))
def check(c, msg):
    if not c: raise SystemExit("FAIL " + msg)
check(until("ready")[-1].startswith("ready"), "ready")
send("break\t%s\t4" % src); send("go")
st = until("stopped")[-1].split("\t"); check(st[1] == "breakpoint" and st[3] == "4", "breakpoint at line 4: %r" % st)
send("stack"); fr = until("end"); check(fr[0] == "stack\t3" and "\tadd\t" in fr[1] and "\tmain\t" in fr[2], "stack %r" % fr)
send("vars\t0"); v = until("end"); check("var\tlocal\ta\tint\t0" in v and "var\tlocal\tb\tint\t1" in v and "var\tglobal\tG\tint\t100" in v, "vars %r" % v)
send("eval\t0\ta * 10 + G"); check(until("result")[-1] == "result\tok\tint\t100", "eval")
send("eval\t1\tp.x + 5"); check(until("result")[-1] == "result\tok\tint\t6", "eval in caller frame")
send("eval\t0\tnope"); check(until("result")[-1].startswith("result\terr"), "eval error")
send("eval\t0\ta = 7"); check("assigned" in until("result")[-1], "assign"); send("vars\t0"); check("var\tlocal\ta\tint\t7" in until("end"), "assignment visible")
send("stepover"); st = until("stopped")[-1].split("\t"); check(st[1] == "step" and st[3] == "5", "stepover -> line 5: %r" % st)
send("stepin"); st = until("stopped")[-1].split("\t"); check(st[3] in ("12", "11", "13"), "stepout of add by stepping: %r" % st)
send("clear"); send("continue"); st = until("stopped")[-1].split("\t"); check(st[1] == "exception" and st[3] == "14" and "division by zero" in st[5], "exception stop: %r" % st)
send("vars\t0"); v = until("end"); check(any(x.startswith("var\tlocal\ttotal\tint\t") for x in v), "vars at exception")
send("continue"); time.sleep(0.5); out = p.stdout.read().decode(); rc = p.wait(timeout=5)
check(rc == 65 and "total" in out, "exit code / output: %r %r" % (rc, out)); print("ok  debugger protocol (break, step, stack, vars, eval, assign, exception stop)")
