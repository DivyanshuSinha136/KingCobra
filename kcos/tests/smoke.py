#!/usr/bin/env python3
"""KCOS smoke test: boot the image headless in QEMU, drive the shell over the serial port, check the output.
usage: smoke.py build/kcos.img      (needs qemu-system-x86_64; works on a temporary copy, the image is not modified)"""
import os, select, shutil, subprocess, sys, tempfile, time

src = sys.argv[1] if len(sys.argv) > 1 else "build/kcos.img"
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
tmp = tempfile.mkdtemp()
img = os.path.join(tmp, "test.img")
shutil.copy(src, img)

def session(commands, boot_wait=3.0):
    """boot, run (command, seconds-to-wait) pairs, return the serial output"""
    p = subprocess.Popen([qemu, "-drive", f"format=raw,file={img}", "-display", "none", "-serial", "stdio",
                          "-monitor", "none", "-no-reboot", "-m", "128"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    def drain(seconds):
        nonlocal out
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([p.stdout], [], [], 0.05)
            if r:
                d = os.read(p.stdout.fileno(), 65536)
                if not d:
                    return
                out += d
    drain(boot_wait)
    for cmd, wait in commands:
        p.stdin.write((cmd + "\r").encode()); p.stdin.flush(); drain(wait)
    p.kill()
    return out.decode(errors="replace").replace("\r", "")

checks = []
def check(desc, cond): checks.append((desc, bool(cond)))

# ---- boot 1: fresh disk
t1 = session([(c, 0.8) for c in ["echo hello kcos > /tmp/t.txt", "echo second >> /tmp/t.txt", "cat /tmp/t.txt", "mkdir /tmp/d", "ls /tmp",
              "calc (2+3)*4-10/3", "calc 0x10 * 0x10", "mem", "heap", "cpuinfo", "hexdump 0x500 16", "cd /etc", "pwd", "cat motd",
              "nosuchcmd", "uptime", "disk"]] +
             [("threads", 5.0), ("tasks", 0.8), ("echo kept across reboot > /home/keep.txt", 0.5), ("sync", 1.0)])
check("boot banner + boot stages OK", t1.count("[  OK  ]") >= 8)
check("echo/cat round trip through KCFS", "hello kcos\nsecond" in t1)
check("mkdir + ls", "d/" in t1)
check("calc precedence", "17  (0x11)" in t1)
check("calc hex", "256  (0x100)" in t1)
check("memory manager (E820)", "RAM (E820 usable):" in t1 and "Heap:" in t1)
check("heap alloc/free/coalesce", "(coalesced)" in t1)
check("cpuid", "vendor:" in t1)
check("boot info magic readable", "|KCOS" in t1)
check("cd + pwd", "\n/etc\n" in t1)
check("unknown command", "nosuchcmd: command not found" in t1)
check("timer ticking", "ticks @ 100 Hz" in t1)
check("ATA disk detected", "ATA primary master:" in t1 and "2048 sectors" in t1)
check("preemptive threads + atomics (kc_switch, fnaddr, callp)", "= 900000  (correct)" in t1)
check("threads actually interleaved", "context switches: " in t1 and "context switches: 0 " not in t1)
check("sync writes the filesystem to disk", "filesystem saved to disk" in t1)

# ---- boot 2: same disk -> the filesystem comes back
t2 = session([("cat /home/keep.txt", 0.8), ("ls /tmp", 0.8), ("disk", 0.8), ("crash", 1.0)])
check("filesystem restored from disk after reboot", "KCFS restored from disk" in t2 and "kept across reboot" in t2)
check("restored directory tree", "t.txt" in t2 and "d/" in t2)
check("divide-by-zero panic screen", "KCOS KERNEL PANIC" in t2 and "#DE divide error" in t2)

shutil.rmtree(tmp)
bad = 0
for desc, ok in checks:
    print(("PASS  " if ok else "FAIL  ") + desc)
    bad += 0 if ok else 1
if bad:
    print("\n--- boot 1 ---\n" + t1 + "\n--- boot 2 ---\n" + t2)
print(f"\n{len(checks) - bad}/{len(checks)} checks passed")
sys.exit(1 if bad else 0)
