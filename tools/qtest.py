#!/usr/bin/env python3
"""在 QEMU + OVMF 里真机跑一遍引导，按键推进并逐帧截屏"""
import os, glob, subprocess, time, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "XP-Wizard.iso")
OUT = os.path.join(ROOT, "build", "shots")
os.makedirs(OUT, exist_ok=True)

def find(pats):
    for p in pats:
        g = sorted(glob.glob(p))
        if g: return g[0]
    return None

CODE = find(["/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd",
             "/usr/share/ovmf/OVMF_CODE.fd", "/usr/share/qemu/OVMF_CODE.fd"])
VARS = find(["/usr/share/OVMF/OVMF_VARS_4M.fd", "/usr/share/OVMF/OVMF_VARS.fd",
             "/usr/share/ovmf/OVMF_VARS.fd", "/usr/share/qemu/OVMF_VARS.fd"])
if not CODE or not VARS:
    print("OVMF not found"); sys.exit(1)

vars_copy = os.path.join(ROOT, "build", "OVMF_VARS_run.fd")
open(vars_copy, "wb").write(open(VARS, "rb").read())

cmd = ["qemu-system-x86_64", "-machine", "q35", "-m", "1024", "-smp", "2",
       "-drive", f"if=pflash,format=raw,readonly=on,file={CODE}",
       "-drive", f"if=pflash,format=raw,file={vars_copy}",
       "-drive", f"file={os.path.join(ROOT,'build','efiboot.img')},format=raw,if=ide",
       "-vga", "std", "-display", "none", "-monitor", "stdio", "-no-reboot",
       "-serial", f"file:{os.path.join(ROOT,'build','serial.log')}",
       "-d", "guest_errors,int", "-D", os.path.join(ROOT, "build", "qemu-debug.log")]

log = open(os.path.join(ROOT, "build", "qmon.log"), "w")
q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)

def cmdline(c):
    try:
        q.stdin.write(c + "\n"); q.stdin.flush()
    except Exception as e:
        print("monitor write failed", e)

time.sleep(5.0)
for i in range(12):
    cmdline("sendkey ret")
    time.sleep(1.1)
    cmdline(f"screendump {OUT}/shot_{i:02d}.ppm")
    time.sleep(0.5)

cmdline("quit")
try:
    q.wait(timeout=10)
except Exception:
    q.kill()

shots = sorted(glob.glob(os.path.join(OUT, "*.ppm")))
print("shots:", len(shots))
