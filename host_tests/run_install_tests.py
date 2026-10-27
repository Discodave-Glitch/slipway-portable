#!/usr/bin/env python3
"""Compiles and runs the installer's host tests (simulated FM-1 install sequence; stock record; update-from-SD logic)
under AddressSanitizer + UBSan. Exit code 0 only if everything passes."""
import os, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
inc = ["-I" + os.path.join(root, "components/fm1proto/include"), "-I" + os.path.join(root, "components/fm1install/include")]
flags = ["gcc", "-std=gnu11", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
jobs = [
    ("install sequence against a simulated FM-1",
     ["install_sim.c", "../components/fm1install/fm1install.c", "../components/fm1proto/fm1proto.c"], inc),
    ("stock record + update-from-SD logic",
     ["installer_units.c", "../installer/main/sdstate.c", "../installer/main/sdupdate.c"],
     ["-I" + os.path.join(here, "stubs"), "-I" + os.path.join(root, "installer/main")]),
]
bad = 0
with tempfile.TemporaryDirectory() as tmp:
    for title, srcs, extra in jobs:
        exe = os.path.join(tmp, "t")
        cmd = flags + extra + [os.path.join(here, s) for s in srcs] + ["-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print("COMPILE FAILED:", title); print(r.stdout + r.stderr); bad += 1; continue
        print("==", title)
        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout + r.stderr)
        if r.returncode: bad += 1
print("\nALL PASSED" if not bad else "\n%d GROUP(S) FAILED" % bad)
sys.exit(1 if bad else 0)
