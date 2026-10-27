#!/usr/bin/env python3
"""Compares the C protocol library with Felucca's own Python installer (tools/fm1_install.py), byte for byte.
Usage: python3 run_tests.py /path/to/Felucca/tools/fm1_install.py   (builds fm1cli with gcc first)"""
import importlib.util, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ref_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "fm1_install.py")
spec = importlib.util.spec_from_file_location("fm1_install", ref_path)
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)

exe = os.path.join(tempfile.mkdtemp(), "fm1cli")
subprocess.check_call(["gcc", "-Wall", "-Wextra", "-O1", "-fsanitize=address,undefined", "-I", os.path.join(HERE, "..", "components", "fm1proto", "include"),
                       os.path.join(HERE, "fm1cli.c"), os.path.join(HERE, "..", "components", "fm1proto", "fm1proto.c"), "-o", exe])

def cli(*a):
    r = subprocess.run([exe, *map(str, a)], capture_output=True, text=True)
    if r.returncode not in (0, 1):
        raise SystemExit("cli failed: %s %s" % (a, r.stderr))
    return r.stdout.strip()

random.seed(7)
checks = 0
def ok(cond, msg):
    global checks
    checks += 1
    if not cond: raise SystemExit("FAIL: " + msg)

# pack7 / unpack7
for n in list(range(0, 40)) + [100, 511, 512, 520]:
    d = bytes(random.randrange(256) for _ in range(n))
    ok(cli("pack7", d.hex()) == ref.pack7(d).hex(), "pack7 n=%d" % n)
    p = ref.pack7(d)
    ok(cli("unpack7", p.hex()) == ref.unpack7(p).hex(), "unpack7 n=%d" % n)

# responses and requests (both directions)
for n in [0, 1, 7, 8, 100, 511, 512]:
    for fl in (0, 1, 0x80):
        addr = random.randrange(0, 0x400000)
        d = bytes(random.randrange(256) for _ in range(n))
        ok(cli("response", addr, fl, d.hex()) == ref.response(addr, d, fl).hex(), "response n=%d" % n)
def request(fl, addr, n):
    u = bytearray([0x00, 0x59, 0x30, 0, 0, 0, fl]) + addr.to_bytes(4, "little") + n.to_bytes(3, "little")
    u.append(~sum(u[6:]) & 0xFF)
    return b"\xF0" + ref.pack7(bytes(u)) + b"\xF7"
for _ in range(50):
    fl, addr, n = random.randrange(256), random.randrange(0, 0x400000), random.randrange(0, 513)
    pkt = request(fl, addr, n)
    ok(ref.parse_request(pkt) == (fl, addr, n), "reference request")
    ok(cli("request", pkt.hex()) == "%d %d %d" % (fl, addr, n), "request %r" % ((fl, addr, n),))
for i in range(1, 17):          # flip one bit in each packed byte: C and Python must agree on accept/reject
    bad = bytearray(request(1, 2, 3)); bad[i] ^= 1
    r = ref.parse_request(bytes(bad))
    ok(cli("request", bytes(bad).hex()) == ("none" if r is None else "%d %d %d" % r), "corrupt request, byte %d" % i)
ok(cli("request", "f0f7") == "none", "short request rejected")

# identity
def identity(text, bad=False):
    d = bytearray(34); d[0:3] = b"\x00\x59\x11"; t = text.encode(); d[6:6 + len(t)] = t
    return b"\xF0" + ref.pack7(bytes(d)) + b"\xF7"
for t in ["FM-1_015", "FM-1_906", "ota-FM-1_15", "Felucca_1", "FM-1_", "_5", "FM-1_1_2", "FM-1", "FM-1_12a", "A_0"]:
    pkt = identity(t); r = ref.parse_identity(pkt)
    exp = "none" if r is None else "%s %s %d %d" % (r.text, r.model, r.version, 1 if r.loader else 0)
    ok(cli("identity", pkt.hex()) == exp, "identity %r" % t)

# a synthetic package: 20 marker bytes carry the identity (see product_of in the Python tool)
def make_pkg(ident, size):
    raw = bytearray(random.randrange(256) for _ in range(size))
    for i in range(20):
        raw[i * 0x30 + 0x2F] = ((ord(ident[i]) + i + 1) & 0xFF) if i < len(ident) else 0x7D
    return bytes(raw)
for ident, size in [("FM-1_015", 150000), ("FM-1_906", 977), ("Felucca_2", 300001)]:
    raw = make_pkg(ident, size)
    f = tempfile.NamedTemporaryFile(delete=False); f.write(raw); f.close()
    ok(ref.product_of(raw) == ident, "reference product")
    ok(cli("product", f.name) == ident, "product %s" % ident)
    logical = ref.logical_image(raw)
    ok(cli("lsize", len(raw)) == str(len(logical)), "logical size")
    for _ in range(200):          # random reads, including ones crossing the 940-byte header boundary
        a = random.randrange(0, len(logical)); n = min(random.choice([1, 8, 47, 48, 512]), len(logical) - a)
        ok(cli("logical", f.name, a, n) == logical[a:a + n].hex(), "logical read %d+%d" % (a, n))
    for a, n in [(0, 940), (0, 941), (939, 2), (940, 1), (930, 30), (0, 512), (len(logical) - 5, 5)]:
        if a + n > len(logical): continue
        ok(cli("logical", f.name, a, n) == logical[a:a + n].hex(), "logical edge %d+%d" % (a, n))
    ok(cli("logical", f.name, len(logical) - 2, 5) == "fail", "read past the end refused")
    os.unlink(f.name)

# USB-MIDI packets: encode then decode, in odd chunk sizes
def usb_expected(s):
    out = bytearray(); i = 0
    while i < len(s):
        rem = len(s) - i
        if rem >= 3: out += bytes([0x7 if rem == 3 else 0x4]) + s[i:i + 3]; i += 3
        elif rem == 2: out += bytes([0x6]) + s[i:i + 2] + b"\0"; i += 2
        else: out += bytes([0x5]) + s[i:i + 1] + b"\0\0"; i += 1
    return bytes(out)
for n in list(range(2, 30)) + [600]:
    s = b"\xF0" + bytes(random.randrange(128) for _ in range(n - 2)) + b"\xF7"
    enc = cli("usbenc", s.hex())
    ok(enc == usb_expected(s).hex(), "usbmidi encode n=%d" % n)
    for ch in (4, 8, 12, len(bytes.fromhex(enc))):
        ok(cli("usbdec", enc, ch) == "pkt " + s.hex(), "usbmidi decode n=%d chunk=%d" % (n, ch))
# two packets back to back, plus a note-on in between (must be ignored)
s1 = ref.HS_QUERY; s2 = identity("FM-1_015")
stream = usb_expected(bytes(s1)) + bytes([0x09, 0x90, 0x40, 0x7F]) + usb_expected(s2)
ok(cli("usbdec", stream.hex()).split("\n") == ["pkt " + bytes(s1).hex(), "pkt " + s2.hex()], "two packets and a note")
ok(cli("usbenc", bytes(ref.HS_QUERY).hex()) == usb_expected(bytes(ref.HS_QUERY)).hex(), "handshake query encoding")
ok(bytes(ref.HS_QUERY).hex() == "f00032450000004" "07ff7", "handshake constant matches")
ok(bytes(ref.UPGRADE).hex() == "f0222435" "7ff7", "upgrade constant matches")
print("PASS: %d checks, C library matches Felucca's Python tool" % checks)
