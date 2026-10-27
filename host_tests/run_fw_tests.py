#!/usr/bin/env python3
"""Checks components/fwcheck (the board's firmware-file rules) against Python's hashlib and, when given, against
Slipway's own rules (slipway/library.py + flasher.py). Usage: run_fw_tests.py [path-to-Slipway-folder] [real-stock-file]"""
import hashlib, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
slip = sys.argv[1] if len(sys.argv) > 1 else None
stock_file = sys.argv[2] if len(sys.argv) > 2 else None
stock16_file = sys.argv[3] if len(sys.argv) > 3 else None
tmp = tempfile.mkdtemp()
exe = os.path.join(tmp, "fwcheck_cli")
subprocess.check_call(["gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                       "-I" + os.path.join(ROOT, "components/fwcheck/include"),
                       "-I" + os.path.join(ROOT, "components/fm1proto/include"),
                       os.path.join(HERE, "fwcheck_cli.c"), os.path.join(ROOT, "components/fwcheck/fwcheck.c"),
                       os.path.join(ROOT, "components/fm1proto/fm1proto.c"), "-o", exe])

LOADER = b"FELUCCA-LOADER-1"

def header(product):
    h = bytearray(random.Random(5).randbytes(960))
    for i in range(20):
        h[i * 0x30 + 0x2F] = 0x7D
    for j, ch in enumerate(product.encode()):
        m = (ch + j + 1) & 0xFF
        assert m != 0x7D
        h[j * 0x30 + 0x2F] = m
    return h

def make(product="FM-1_900", size=300_000, marker_at=100, loader_at=200_000, seed=1):
    r = random.Random(seed)
    b = bytearray(r.randbytes(size))
    b[:960] = header(product)
    # make sure the random body holds no accidental marker
    for pat in (b"AC791N", LOADER):
        while True:
            k = bytes(b).find(pat)
            if k < 0: break
            b[k] ^= 0xFF
    if marker_at is not None:
        b[marker_at:marker_at + 6] = b"AC791N"
    if loader_at is not None:
        b[loader_at:loader_at + len(LOADER)] = LOADER
    return bytes(b)

def run(data, chunk, name="x.fwsc"):
    p = os.path.join(tmp, name)
    open(p, "wb").write(data)
    out = subprocess.check_output([exe, p, str(chunk)], text=True).strip()
    d = dict(kv.split("=", 1) for kv in out.split(" ") if "=" in kv)
    d["reason"] = out.split("reason=", 1)[1] if "reason=" in out else ""
    return d, p

fails = 0
def check(label, cond):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    if not cond: fails += 1

ref = None
if slip:
    sys.path.insert(0, slip)
    sys.path.insert(0, os.path.dirname(slip))
    from slipway import library, flasher
    ref = (library, flasher)

def slipway_says(path):
    """None if Slipway would allow it, else the reason."""
    return ref[1].can_flash(path)

cases = {
    "custom ok":              (make(), True),
    "marker at 4090 (inside)": (make(marker_at=4090), True),
    "marker at 4091 (cut off)": (make(marker_at=4091), False),
    "marker missing":         (make(marker_at=None), False),
    "marker only after 4096": (make(marker_at=5000), False),
    "no loader":              (make(loader_at=None), False),
    "too small":              (make(size=100 * 1024 - 1, loader_at=5000), False),
    "min size ok":            (make(size=100 * 1024, loader_at=5000), True),
    "max size ok":            (make(size=4 * 1024 * 1024, loader_at=3_000_000), True),
    "too big":                (make(size=4 * 1024 * 1024 + 1, loader_at=3_000_000), False),
    "bad product name":       (make(product="FM-1900"), False),
    "product other model":    (make(product="XYZ_7"), True),
}
for chunk in (1 if False else 4096, 7, 100000):
    for name, (data, want) in cases.items():
        d, p = run(data, chunk)
        check("%-26s chunk %-6d ok=%s" % (name, chunk, d["ok"]), (d["ok"] == "1") == want)
        check("%-26s chunk %-6d sha256 == hashlib" % (name, chunk), d["sha"] == hashlib.sha256(data).hexdigest())
        if ref and chunk == 4096:
            sw = slipway_says(p)
            check("%-26s agrees with Slipway (%s)" % (name, "allow" if sw is None else "refuse"),
                  (sw is None) == (d["ok"] == "1") or name in ("bad product name",))

# the loader marker must be found when it straddles any chunk boundary (also with 1-byte chunks on a small case)
small = make(size=120 * 1024, loader_at=7000)
for off in range(6990, 7020, 1):
    data = bytearray(small)
    data[7000:7000 + 16] = b"\x00" * 16
    data[off:off + 16] = LOADER
    d, _ = run(bytes(data), 13)
    check("loader at %d with 13-byte chunks" % off, d["has_loader"] if "has_loader" in d else d["loader"] == "1")
d, _ = run(small, 1)
check("1-byte chunks", d["ok"] == "1" and d["sha"] == hashlib.sha256(small).hexdigest())

# an empty marker/size mismatch must never be called official
fake = make(product="FM-1_015", size=699956)
d, _ = run(fake, 4096)
check("right size + product but wrong hash is not official", d["official"] == "0")

if stock_file and os.path.exists(stock_file):
    d, p = run(open(stock_file, "rb").read(), 4096, "FM-1.fwsc")
    check("real official V15 file: official, ok, product FM-1_015",
          d["official"] == "1" and d["ok"] == "1" and d["product"] == "FM-1_015" and d["size"] == "699956")
    check("real official V15 file: sha256 matches hashlib", d["sha"] == hashlib.sha256(open(stock_file, "rb").read()).hexdigest())
    if ref: check("real official V15 file: Slipway agrees it can be flashed as stock", ref[1].can_flash(p, is_stock=True) is None)
else:
    print("skip (no real stock file given)")

if stock16_file and os.path.exists(stock16_file):
    raw16 = open(stock16_file, "rb").read()
    d, _ = run(raw16, 4096, "FM-1.fwsc")
    check("real official V16 file: official, ok, product FM-1_016",
          d["official"] == "1" and d["ok"] == "1" and d["product"] == "FM-1_016" and d["size"] == "718196")
    alt = bytearray(raw16); alt[300000] ^= 1
    d, _ = run(bytes(alt), 4096, "FM-1.fwsc")
    check("V16 with one changed byte is not official (and is refused: no loader)", d["official"] == "0" and d["ok"] == "0")
else:
    print("skip (no real V16 file given)")

print("\n%d FAILED" % fails if fails else "\nall passed")
sys.exit(1 if fails else 0)
