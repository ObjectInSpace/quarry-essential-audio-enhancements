"""Independent check of what Dialogue=positional loads.

usage: python tools/check_dialogue_bank.py <the game's Speech.bnk> <changed copy> [wwiser.pyz]
  The changed copy is build/dialogue_patched.bnk, written by the "dialogue"
  test scenario with the default settings when QSA_SPEECH_BNK is set.

Written separately from the C rebuild in src/dialogue.c on purpose: it parses
both banks itself and checks what the change PROMISES rather than re-running
the same algorithm.
  1. every chunk but HIRC is byte for byte the original
  2. HIRC holds the same items in the same order; every item not in
     src/dialogue_table.h is byte for byte the original
  3. each changed attenuation keeps its header, its original curves and its
     RTPC tail; slots 1, 2, 4 and 6 are unchanged; slots 0, 5 and 3 point at
     three new curves
  4. those curves, evaluated where Wwise would evaluate them, behave as the
     defaults say: volume 0 dB to 2 m, -6 dB at 4 m, -12 dB from 8 m on;
     spread 30% at 0 m, 5% from 3 m; muffling 0 to 2 m, 35 from 20 m,
     rising in between, about half way at the geometric middle (6.3 m)
  5. the new volume curves are stored on the SAME scale as the game's own
     dB curves. A dB-scaled curve holds dB / 20 (log10 of the amplitude):
     the game's own end at values like -0.6019 (-12.04 dB). This check was
     added after the first in-game run: the C code and this script both
     read those values as plain dB, agreed with each other, passed, and
     every voice beyond 2 m went silent. Agreement between two readers that
     share an assumption is not evidence; the game's own data is.
  6. if wwiser.pyz is given, that unrelated bank parser reads the copy
     without errors
Exits non-zero on any failure.
"""
import os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
if len(sys.argv) < 3:
    sys.exit(__doc__)
orig = open(sys.argv[1], "rb").read()
patched_path = sys.argv[2]
wwiser = sys.argv[3] if len(sys.argv) > 3 else None
new = open(patched_path, "rb").read()
table = {int(m.group(1)) for m in re.finditer(r"\{\s*(\d+)u,", open(os.path.join(ROOT, "src", "dialogue_table.h")).read())}
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += not ok


def chunks(b):
    o, out = 0, {}
    while o < len(b):
        tag, sz = b[o:o + 4].decode(), struct.unpack_from("<I", b, o + 4)[0]
        out[tag] = b[o + 8:o + 8 + sz]
        o += 8 + sz
    return out, o


def items(h):
    n, p, out = struct.unpack_from("<I", h, 0)[0], 4, []
    for _ in range(n):
        t, sz = h[p], struct.unpack_from("<I", h, p + 1)[0]
        out.append((t, h[p + 5:p + 5 + sz]))
        p += 5 + sz
    return out, p == len(h)


def curves(body):
    ctu = list(struct.unpack_from("<7b", body, 6))
    n, o, cs = body[13], 14, []
    for _ in range(n):
        sc, np_ = body[o], struct.unpack_from("<H", body, o + 1)[0]
        pts = [struct.unpack_from("<ffI", body, o + 3 + 12 * k) for k in range(np_)]
        cs.append((sc, pts, body[o:o + 3 + 12 * np_]))
        o += 3 + 12 * np_
    return ctu, cs, body[o:]


def at(pts, x):
    if x <= pts[0][0]: return pts[0][1]
    for (x0, y0, _), (x1, y1, _) in zip(pts, pts[1:]):
        if x0 <= x <= x1: return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


oc, on = chunks(orig)
nc, nn = chunks(new)
print("1. chunks")
check(nn == len(new), "the changed bank's chunks tile it exactly")
check(list(oc) == list(nc), f"same chunks in the same order: {list(nc)}")
check(all(oc[k] == nc[k] for k in oc if k != "HIRC"), "every chunk but HIRC is unchanged")

print("2. items")
oi, ook = items(oc["HIRC"])
ni, nok = items(nc["HIRC"])
check(ook and nok and len(oi) == len(ni), f"{len(ni)} items in both, each HIRC tiled by its items")
check(all(a[0] == b[0] for a, b in zip(oi, ni)), "same item types in the same order")
others = [(a, b) for a, b in zip(oi, ni) if not (a[0] == 0x0E and struct.unpack_from("<I", a[1])[0] in table)]
check(all(a == b for a, b in others), f"all {len(others)} items outside the table are unchanged")

print("3./4. changed attenuations")
pairs = [(a[1], b[1]) for a, b in zip(oi, ni) if a[0] == 0x0E and struct.unpack_from("<I", a[1])[0] in table]
check(len(pairs) == len(table), f"{len(table)} changed attenuations found ({len(pairs)})")
bad_struct, bad_curve = [], []
for ob, nb in pairs:
    aid = struct.unpack_from("<I", ob)[0]
    octu, ocs, otail = curves(ob)
    nctu, ncs, ntail = curves(nb)
    k = len(ocs)
    ok = (nb[:6] == ob[:6] and len(ncs) == k + 3 and [c[2] for c in ncs[:k]] == [c[2] for c in ocs] and
          ntail == otail and nctu[0] == k and nctu[5] == k + 1 and nctu[3] == k + 2 and
          [nctu[i] for i in (1, 2, 4, 6)] == [octu[i] for i in (1, 2, 4, 6)])
    if not ok: bad_struct.append(aid); continue
    vol, spr, muf = ncs[nctu[0]], ncs[nctu[5]], ncs[nctu[3]]
    v = lambda m: 20 * at(vol[1], m * 100)   # stored as dB / 20
    s = lambda m: at(spr[1], m * 100)
    f = lambda m: at(muf[1], m * 100)
    good = (vol[0] == 2 and spr[0] == 0 and muf[0] == 0 and
            abs(v(0)) < 0.01 and abs(v(2)) < 0.01 and abs(v(4) + 6) < 0.6 and abs(v(8) + 12) < 0.01 and
            abs(v(20) + 12) < 0.01 and all(v(a) >= v(b) for a, b in zip(range(0, 20), range(1, 21))) and
            abs(s(0) - 30) < 0.01 and abs(s(3) - 5) < 0.01 and abs(s(20) - 5) < 0.01 and
            abs(f(1)) < 0.01 and abs(f(2)) < 0.01 and abs(f(20) - 35) < 0.01 and abs(f(6.32) - 17.5) < 2.5 and
            all(f(a) <= f(b) for a, b in zip(range(0, 20), range(1, 21))))
    if not good: bad_curve.append(aid)
check(not bad_struct, f"header, original curves, RTPC tail and other slots kept; new slots point at new curves {bad_struct or ''}")
check(not bad_curve, f"the new curves behave as the defaults say {bad_curve or ''}")

print("5. the game's own scale")
own = [y for ob, nb in pairs for sc, pts, raw in curves(ob)[1] if sc == 2 for x, y, i in pts]
mine = [y for ob, nb in pairs for sc, pts, raw in [curves(nb)[1][curves(nb)[0][0]]] for x, y, i in pts]
check(own and mine and min(mine) >= min(own) - 0.5 and min(mine) < 0,
      f"new volume values ({min(mine):.3f} lowest) sit on the scale of the game's own dB curves "
      f"({min(own):.3f} lowest, i.e. {20 * min(own):.1f} dB)")

print("6. wwiser")
if wwiser:
    r = subprocess.run([sys.executable, wwiser, "-d", "txt", "-dn", patched_path + "_dump", patched_path],
                       capture_output=True, text=True)
    dump_path = patched_path + "_dump.txt"
    dump = open(dump_path, encoding="utf-8", errors="replace").read() if os.path.exists(dump_path) else ""
    errs = [l for l in (r.stdout + r.stderr).splitlines() if re.search(r"error|exception|traceback", l, re.I)]
    check(r.returncode == 0 and dump and not errs and "CAkAttenuation" in dump,
          f"wwiser parsed it without errors{': ' + errs[0] if errs else ''}")
else:
    print("  not run (no wwiser.pyz given)")

print("PASSED" if not fails else f"FAILED: {fails} failure(s)")
sys.exit(1 if fails else 0)
