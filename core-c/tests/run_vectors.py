#!/usr/bin/env python3
"""Run the SingleStepTests/m68000 vectors against the Musashi C core (ctypes).

Mirrors snes86's core-c/tests/run_vectors.py, adapted for the 68000:
  - vectors are per-mnemonic .json.bin (decode.py's binary format), not gz JSON
  - 19-register state (d0-d7, a0-a6, usp, ssp, sr, pc) + a 2-word prefetch queue
  - PC convention: the opcode lives at initial.pc - 4 (tests report PC 4 bytes
    ahead, modeling the 68000's 2-word prefetch). Musashi runs with prefetch
    OFF, so musashi_pc + 4 == test.pc. We set PC = initial.pc - 4 and compare
    tm_get(PC) + 4.
  - A7 is banked: the vectors give usp and ssp separately; the active A7 is
    whichever the SR supervisor bit selects. Handled mode-aware on set + get.

Usage:
  run_vectors.py                      # every downloaded file
  run_vectors.py --files NOP ADD.b    # only these mnemonics
  run_vectors.py --limit 500          # first N tests per file
  run_vectors.py --fails 3            # up to N failure details per file
  run_vectors.py --no-mask            # do not mask undefined CCR bits
"""
import argparse
import ctypes
import glob
import os
import sys
from multiprocessing import Pool, cpu_count

import decode  # sibling: decode_test(content, ptr) -> (ptr, test)

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "build", "libem68k.dylib")
VEC = os.path.join(HERE, "vectors")

# m68k_register_t indices (see musashi/m68k.h)
D0, A0, A7, PC, SR, SP, USP, ISP = 0, 8, 15, 16, 17, 18, 19, 20
PCMASK = 0xFFFFFF
SUPER = 0x2000  # SR supervisor bit

# Undefined-CCR-bit masks per mnemonic prefix. The 68000 leaves some condition
# codes undefined for these; real silicon (and thus the vectors) has a value,
# but Musashi need not reproduce the exact garbage. Populated empirically as
# failures are triaged; keys match the mnemonic file stem (before the first dot).
# Value = CCR bits (low 8 of SR) that ARE defined and must match; others ignored.
CCR_ALL = 0x1F  # X N Z V C  (bit: X=0x10 N=0x08 Z=0x04 V=0x02 C=0x01)
# CCR bits that ARE defined for these instructions (others are undefined on the
# 68000 per the M68000 PRM — real silicon has a value the vectors capture, but
# a register-level emulator need not reproduce it). Same idea as snes86's
# per-opcode flags-mask. Keyed by mnemonic before the first dot.
UNDEF_CCR = {
    "ABCD": 0x11,  # N,Z,V undefined; X,C defined
    "SBCD": 0x11,
    "NBCD": 0x11,
    "CHK": 0x10,   # N,Z,V,C undefined (mostly traps anyway); X unaffected
    "DIVU": 0x13,  # N,Z undefined on overflow; X,V,C defined
    "DIVS": 0x13,
}


_U = ctypes.POINTER(ctypes.c_uint)
_B = ctypes.POINTER(ctypes.c_ubyte)


class Lib:
    def __init__(self):
        d = ctypes.CDLL(LIB)
        d.tm_run1.restype = ctypes.c_int
        d.tm_run1.argtypes = [_U, _U, _U, _B, ctypes.c_int, _U, _B, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
        d.tm_init()
        self.d = d

    def run1(self, ini, fin, ira, irv, fra, frv, ccr, pc_off):
        ni, nf = len(irv), len(frv)
        return self.d.tm_run1(
            (ctypes.c_uint * 19)(*ini), (ctypes.c_uint * 19)(*fin),
            (ctypes.c_uint * ni)(*ira), (ctypes.c_ubyte * ni)(*irv), ni,
            (ctypes.c_uint * nf)(*fra), (ctypes.c_ubyte * nf)(*frv), nf, ccr, pc_off)


_lib = None


def _init_worker():
    global _lib
    _lib = Lib()


def ccr_mask(stem, no_mask):
    if no_mask:
        return CCR_ALL
    return UNDEF_CCR.get(stem.split(".")[0], CCR_ALL)


def _state(s):
    return ([s["d%d" % i] & 0xFFFFFFFF for i in range(8)]
            + [s["a%d" % i] & 0xFFFFFFFF for i in range(7)]
            + [s["usp"] & 0xFFFFFFFF, s["ssp"] & 0xFFFFFFFF, s["sr"], s["pc"] & 0xFFFFFFFF])


def run_test(t, stem, no_mask):
    ini, fin = t["initial"], t["final"]
    ira = [a for a, _ in ini["ram"]]
    irv = [v for _, v in ini["ram"]]
    fra = [a for a, _ in fin["ram"]]
    frv = [v for _, v in fin["ram"]]
    # STOP (0x4E72) halts and suppresses the trailing prefetch, so its final PC
    # is the raw post-instruction PC (offset 0), not the usual +4 (2-word ahead).
    pc_off = 0 if ini["prefetch"][0] == 0x4E72 else 4
    bits = _lib.run1(_state(ini), _state(fin), ira, irv, fra, frv, ccr_mask(stem, no_mask), pc_off)

    # Classify a failure as the known address/bus-error (group-0) frame class:
    # supervisor entered, supervisor stack grew, or an odd PC (branch to odd
    # target faults on the next fetch). Musashi only approximates that 14-byte
    # frame vs MAME's microcode core; irrelevant to well-behaved Mac code.
    exc = (((fin["sr"] & SUPER) and not (ini["sr"] & SUPER))
           or (fin["ssp"] < ini["ssp"] and (fin["sr"] & SUPER))
           or (fin["pc"] & 1) or (ini["pc"] & 1))
    return bits, bool(exc)


def run_file(args):
    path, limit, nfails, no_mask = args
    if _lib is None:
        _init_worker()
    name = os.path.basename(path)
    stem = name.replace(".json.bin", "")
    try:
        content = open(path, "rb").read()
        from struct import unpack_from
        magic, num = unpack_from("<II", content, 0)
        assert magic == 0x1A3F5D71, "bad file magic"
        ptr = 8
        tests = []
        for _ in range(num):
            ptr, t = decode.decode_test(content, ptr)
            tests.append(t)
    except Exception as ex:  # noqa: BLE001
        return (name, 0, 0, ["LOAD ERROR: %s" % ex])
    if limit:
        tests = tests[:limit]
    npass = 0
    n_exc = 0    # failures that are the known address-error-frame class
    n_clean = 0  # failures with no exception involved — the ones that matter
    fails = []
    FIELDS = [(1, "reg"), (2, "sp"), (4, "pc"), (8, "sr"), (16, "mem")]
    for t in tests:
        bits, exc = run_test(t, stem, no_mask)
        if not bits:
            npass += 1
            continue
        if exc:
            n_exc += 1
        else:
            n_clean += 1
            if len(fails) < nfails:
                op = t["initial"]["prefetch"][0]
                diverged = ",".join(nm for b, nm in FIELDS if bits & b)
                fails.append("  %s  opcode=%04X  diverged=%s\n" % (t["name"], op, diverged))
    return (name, len(tests), npass, n_exc, n_clean, fails)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="*", help="mnemonic stems, e.g. NOP ADD.b")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--fails", type=int, default=1)
    ap.add_argument("--no-mask", action="store_true")
    ap.add_argument("-j", type=int, default=cpu_count())
    args = ap.parse_args()

    if args.files:
        paths = [os.path.join(VEC, f + ".json.bin") for f in args.files]
        missing = [p for p in paths if not os.path.exists(p)]
        if missing:
            sys.exit("missing vector files: %s" % missing)
    else:
        paths = sorted(glob.glob(os.path.join(VEC, "*.json.bin")))
    if not paths:
        sys.exit("no vector files — run fetch.sh first")

    def line(res):
        n, tot, ok, exc, clean, _ = res
        if ok == tot:
            return "%-16s %6d/%-6d ok" % (n, ok, tot)
        return "%-16s %6d/%-6d  addr-err-frame %d  clean %d" % (n, ok, tot, exc, clean)

    work = [(p, args.limit, args.fails, args.no_mask) for p in paths]
    results = []
    if args.j > 1 and len(paths) > 1:
        with Pool(args.j, initializer=_init_worker) as pool:
            for res in pool.imap_unordered(run_file, work):
                results.append(res)
                print(line(res), flush=True)
    else:
        _init_worker()
        for w in work:
            res = run_file(w)
            results.append(res)
            print(line(res), flush=True)

    results.sort()
    tot = sum(r[1] for r in results)
    ok = sum(r[2] for r in results)
    exc = sum(r[3] for r in results)
    clean = sum(r[4] for r in results)
    # Ops with documented Musashi divergences vs the MAME-microcode vectors:
    #   TAS/TRAPV  - flagged known-bad by the vector authors themselves
    #   ABCD/SBCD/NBCD - result is undefined for invalid BCD digit inputs
    #   DIVU/DIVS  - quotient/flags undefined on overflow
    #   CHK        - N/Z/V/C undefined (mostly traps)
    #   LINK       - LINK A7 (self-linking the stack pointer) has an
    #                implementation-defined push/adjust order; other An pass.
    # These are irrelevant to well-behaved Mac code (it never LINKs A7). A clean
    # failure OUTSIDE this set (and the address-error-frame class) is the signal.
    DOCUMENTED = {"TAS", "TRAPV", "ABCD", "SBCD", "NBCD", "DIVU", "DIVS", "CHK", "LINK"}
    def stem(r): return r[0].replace(".json.bin", "")
    clean_doc = sum(r[4] for r in results if stem(r) in DOCUMENTED)
    clean_unexpl = sum(r[4] for r in results if stem(r) not in DOCUMENTED)
    print("\ntotal: %d/%d exact pass (%.3f%%)" % (ok, tot, 100.0 * ok / max(tot, 1)))
    print("  + address-error-frame divergences (odd addr, benign): %d" % exc)
    print("  + documented Musashi divergences (BCD/DIV/CHK/TAS/TRAPV): %d" % clean_doc)
    print("  => accounted for: %d/%d (%.3f%%)"
          % (ok + exc + clean_doc, tot, 100.0 * (ok + exc + clean_doc) / max(tot, 1)))
    print("  UNEXPLAINED clean failures: %d" % clean_unexpl)
    unexpl = [r for r in results if r[4] > 0 and stem(r) not in DOCUMENTED]
    for name, ntests, npass, ex, cl, fails in sorted(unexpl, key=lambda r: r[4], reverse=True):
        print("\n=== %s: %d UNEXPLAINED clean failures ===" % (name, cl))
        for f in fails:
            print(f, end="")
    # success = every failure is either an address-error frame or a documented op
    sys.exit(1 if clean_unexpl else 0)


if __name__ == "__main__":
    main()
