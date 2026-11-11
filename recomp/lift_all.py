#!/usr/bin/env python3
"""Lift every discovered function of th123.exe and report translation coverage.

Writes (local only, never versioned) to artifacts/recomp/<sha>/gen/:
  lifted.h, unit_NNN.c, table.c   the whole program as C
  faults.json                      every instruction lifted as guest_fault
  coverage.json                    counts by mnemonic and by region
With --compile it also builds every unit with gcc and links them with the
runtime and a stub main, which proves the program is complete C.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from recomp.analysis.discover import Discovery  # noqa: E402
from recomp.analysis.image import Image  # noqa: E402
from recomp.lift.lifter import Lifter  # noqa: E402

GCC = Path(r"C:\msys64\ucrt64\bin\gcc.exe")
# Statically linked MSVC CRT and third-party libraries (zlib, libpng, vorbis) live
# at the end of .text; they are candidates for native replacement, so their faults
# are reported apart from the game's own code. Boundary from the CRT entry points
# identified in docs/X87_MODE.md and docs/PE_INVENTORY.md.
LIBRARY_START = 0x81F000

STUB_MAIN = """#include <stdio.h>
#include "lifted.h"
extern const GuestEntry t123_lifted[];
extern const unsigned t123_lifted_count;
int main(void) {
    guest_register(t123_lifted, t123_lifted_count);
    printf("%u lifted functions linked\\n", t123_lifted_count);
    return 0;
}
"""


# The 汉化 (Chinese) build of the game changes one byte of the glyph routine at 0x412910:
# "cmp al, 0xa0" -> "cmp al, 0xff", so every byte 0x81-0xFE starts a two-byte (GBK) character
# instead of only the Shift-JIS lead bytes. The other eight Shift-JIS checks stay as in that build.
GBK_PATCHES = {0x4129BB: (0xA0, 0xFF)}


# Wide 960x544 fight view (--wide; on when the host sets t123_wide). The game keeps drawing in its
# 640x480 coordinates and the host shows -160..800 x -32..512 of them, so the HUD and menus stay
# centred; only the camera's view and the background layers' culling are widened symmetrically
# (screen centre 320 and ground line 420 stay). Each entry: the instruction whose screen-size
# constant changes, and the value it uses in wide mode.
WIDE_L, WIDE_R, WIDE_B = -160.0, 800.0, 512.0
WIDE_PATCHES = [
    (0x428E85, 960.0),   # camera target: max(players' distance + 80, 640) -> 960
    (0x428EEB, 960.0),   #   zoom = 640 / distance -> 960 / distance
    (0x428F02, 0.75),    #   min zoom 0.5 -> 0.75 (960 / 1280: never wider than the stage)
    (0x428F38, 800.0),   #   centre x <= 960 -> 800 (1280 - 480)
    (0x428F45, 800.0),
    (0x4290C8, 0.75),    # camera update: min zoom
    (0x429144, 960.0),   #   centre x clamp: half view = 640 / zoom / 2
    (0x4293C0, WIDE_R),  #   right edge = 640 / zoom - translate.x
    (0x4293D3, WIDE_B),  #   bottom edge = 480 / zoom - translate.y
    # background layers: culling "0 < right && left < 640 && 0 < bottom && top < 480"
    (0x46F44F, WIDE_L), (0x46F45E, WIDE_R), (0x46F484, WIDE_B),
    (0x473719, WIDE_L), (0x473728, WIDE_R), (0x47374E, WIDE_B),
    (0x4740AC, WIDE_L), (0x4740BB, WIDE_R), (0x4740EC, WIDE_B),
    (0x47419E, WIDE_L), (0x4741AD, WIDE_R), (0x4741DE, WIDE_B),
    # background tiling loops: run to the right / bottom edge
    (0x475958, WIDE_R), (0x476478, WIDE_R), (0x476F18, WIDE_R), (0x477010, WIDE_R), (0x4779AE, WIDE_R),
    (0x476FED, WIDE_B), (0x47798B, WIDE_B),
]
_LOAD = re.compile(r"x87_fld_(m32|m64)\(c, (\(uint32_t\)\(0x[0-9a-f]+u\))\)|x87_(m32|m64)\((\(uint32_t\)\(0x[0-9a-f]+u\))\)"
                   r"|x87_fld_const\(c, 0\)")


def apply_wide(files: dict[str, str]) -> None:
    for va, value in WIDE_PATCHES:
        label = f"L_{va:08x}:"
        name = next((n for n, text in files.items() if label in text), None)
        if name is None:
            raise SystemExit(f"wide patch {va:#x}: instruction not lifted")
        text = files[name]
        start = text.index("\n", text.index(label)) + 1
        end = text.index("\n", start)
        lit = f"(x87_t){value!r}"

        def swap(m: re.Match) -> str:
            if m.group(1):
                return f"x87_push(c, t123_wide ? {lit} : x87_{m.group(1)}({m.group(2)}))"
            if m.group(3):
                return f"(t123_wide ? {lit} : x87_{m.group(3)}({m.group(4)}))"
            return f"x87_push(c, t123_wide ? {lit} : (x87_t)0.0)"
        line, n = _LOAD.subn(swap, text[start:end])
        if n != 1:
            raise SystemExit(f"wide patch {va:#x}: expected one constant load in {text[start:end].strip()!r}")
        files[name] = text[:start] + line + text[end:]


def apply_patches(image: Image, patches: dict[int, tuple[int, int]]) -> None:
    for va, (old, new) in patches.items():
        s = image.section_at(va)
        i = va - s.start
        if s.data[i] != old:
            raise SystemExit(f"patch {va:#x}: expected {old:#04x}, found {s.data[i]:#04x}")
        image.sections[image.sections.index(s)] = dataclasses.replace(
            s, data=s.data[:i] + bytes([new]) + s.data[i + 1:])


def region(va: int) -> str:
    return "library" if va >= LIBRARY_START else "game"


def compile_all(gen: Path, jobs: int) -> dict:
    runtime = ROOT / "recomp/runtime"
    env = dict(os.environ, PATH=str(GCC.parent) + os.pathsep + os.environ.get("PATH", ""))
    flags = ["-std=gnu11", "-O1", "-frounding-math", "-fno-strict-aliasing", "-w",
             f"-I{runtime}", f"-I{gen}"]
    (gen / "stub_main.c").write_text(STUB_MAIN, encoding="utf-8")
    sources = sorted(gen.glob("unit_*.c")) + [gen / "table.c", gen / "stub_main.c",
                                              runtime / "guest.c"]
    errors = {}

    def build(src: Path):
        obj = gen / (src.stem + ".o")
        run = subprocess.run([str(GCC), *flags, "-c", str(src), "-o", str(obj)], env=env,
                             capture_output=True, text=True)
        if run.returncode:
            errors[src.name] = run.stderr[-2000:]
        return obj

    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(build, sources))
    compile_seconds = time.monotonic() - started
    if errors:
        return {"compiled": False, "errors": errors}
    exe = gen / "th123_lifted.exe"
    link = subprocess.run([str(GCC), *map(str, objects), "-lm", "-o", str(exe)], env=env,
                          capture_output=True, text=True)
    if link.returncode:
        return {"compiled": True, "linked": False, "errors": {"link": link.stderr[-4000:]}}
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    return {"compiled": True, "linked": True, "compile_seconds": round(compile_seconds, 1),
            "units": len(sources) - 3, "stub_output": run.stdout.strip(),
            "exe_bytes": exe.stat().st_size}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "original/th123.exe")
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--compile-only", action="store_true",
                        help="build the existing gen/ output without lifting again")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--gbk", action="store_true",
                        help="lift with the Chinese (汉化) build's GBK glyph patch; pair with build_vita.py --gbk")
    parser.add_argument("--wide", action="store_true",
                        help="wide 960x544 fight view, switched on by the host's t123_wide; pair with build_vita.py --wide")
    args = parser.parse_args()

    image = Image.from_pe(args.exe)
    if args.gbk:
        apply_patches(image, GBK_PATCHES)
    report = ROOT / "artifacts/recomp" / image.sha256 / "discovery.json"
    if not report.exists():
        raise SystemExit("run recomp/discover.py first")
    entries = sorted(int(f["entry"], 16) for f in json.loads(report.read_text())["functions"])
    gen = ROOT / "artifacts/recomp" / image.sha256 / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    if args.compile_only:
        coverage = json.loads((gen / "coverage.json").read_text(encoding="utf-8"))
        coverage["build"] = compile_all(gen, args.jobs)
        (gen / "coverage.json").write_text(json.dumps(coverage, indent=1), encoding="utf-8")
        print(json.dumps(coverage["build"], indent=1))
        return
    started = time.monotonic()
    lifter = Lifter(Discovery(image), set(entries))
    files, result = lifter.lift_units(
        entries, progress=lambda i, n: print(f"  lifting {i}/{n}", flush=True))
    if args.wide:
        apply_wide(files)
    # unchanged units keep their mtime, so the builds recompile only what changed
    # (a full -O2 Vita build is ~70 min)
    for old in gen.glob("unit_*.c"):
        if old.name not in files:
            old.unlink()
    for name, text in files.items():
        path = gen / name
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")
    lift_seconds = time.monotonic() - started

    by_mnemonic: dict[str, Counter] = {"game": Counter(), "library": Counter()}
    for va, text in result.faults.items():
        by_mnemonic[region(va)][text.split(" (")[0].split()[0]] += 1
    coverage = {
        "functions": len(entries),
        "units": sum(1 for n in files if n.startswith("unit_")),
        "c_bytes": sum(len(t) for t in files.values()),
        "lift_seconds": round(lift_seconds, 1),
        "traps": {"game": sum(1 for va in lifter.traps if region(va) == "game"),
                  "library": sum(1 for va in lifter.traps if region(va) == "library")},
        "faults_total": len(result.faults),
        "dead_flag_instructions": result.dead_flags,
        "faults_game": sum(by_mnemonic["game"].values()),
        "faults_library": sum(by_mnemonic["library"].values()),
        "fault_mnemonics_game": dict(by_mnemonic["game"].most_common()),
        "fault_mnemonics_library": dict(by_mnemonic["library"].most_common()),
    }
    (gen / "faults.json").write_text(json.dumps(
        {f"0x{va:08x}": text for va, text in sorted(result.faults.items())}, indent=1),
        encoding="utf-8")
    if args.compile:
        coverage["build"] = compile_all(gen, args.jobs)
    (gen / "coverage.json").write_text(json.dumps(coverage, indent=1), encoding="utf-8")
    print(json.dumps(coverage, indent=1))


if __name__ == "__main__":
    main()
