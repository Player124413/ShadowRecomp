#!/usr/bin/env python3
"""Write stand-in `generated/` files so the native build can be *compiled* without the game.

`generated/` normally comes from `python tools/gen.py`, which needs the player's own disc image and is never
committed. That makes a plain CI checkout unbuildable, which is a bad place to be: the Android port's code is
where breakage hides, and nobody wants to find it on their phone.

So this script synthesises the two things the build needs from `generated/`:

  generated/files.cmake  - GEN_SOURCES, empty (no recompiled game code to add)
  generated/funcs.h      - a declaration for every guest function the runtime calls by name

The result compiles everything (runtime + Aurora + the Android glue) and links nothing: a link would need the real
recompiled functions. That is exactly what "does the port still build?" needs to know, and nothing more. It is NOT a
usable game: `python tools/gen.py` remains the only way to get one of those.

    python3 tools/android/stub_generated.py         # writes generated/, prints what it did
    cmake -B build ... && ninja -k 0                # compile-only: the link step is expected to fail
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "generated")

# Guest functions the runtime refers to directly: gen.py names them f_XXXXXXXX (the guest address), and the hand-written
# overrides hle_<name>. Both take the emulated CPU, which is the only parameter shape that has to be supported here.
CALL_RE = re.compile(r"\bf_[0-9A-Fa-f]{8}\b")
HLE_RE = re.compile(r"\bhle_[A-Za-z0-9_]+\b")
# "void hle_X(Cpu& c, uint32_t target)" - an override that takes more than the CPU. Those are declared by whoever needs
# them; inventing a prototype for them here would clash with the definition.
HLE_DEF = re.compile(r"\bvoid\s+(hle_[A-Za-z0-9_]+)\s*\(([^)]*)\)")


def sources():
    for folder in ("runtime", "android"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, folder)):
            for name in files:
                if name.endswith((".cpp", ".h", ".hpp")):
                    path = os.path.join(dirpath, name)
                    try:
                        yield name, open(path, encoding="utf-8", errors="ignore").read()
                    except OSError:
                        continue


def scan() -> tuple[list[str], list[str]]:
    guest = set()
    hle = set()
    odd_signature = set()
    texts = list(sources())
    for _, text in texts:
        guest.update(CALL_RE.findall(text))
        hle.update(HLE_RE.findall(text))
        for name, params in HLE_DEF.findall(text):
            # count the parameters: a "(Cpu& c, uint32_t target)" definition cannot be declared as "(Cpu&)"
            if params.count(",") >= 1 and params.strip():
                odd_signature.add(name)
    hle -= odd_signature
    return sorted(guest), sorted(hle)


def main() -> int:
    guest, hle = scan()
    names = guest + hle
    os.makedirs(OUT, exist_ok=True)
    written = []
    files_cmake = os.path.join(OUT, "files.cmake")
    text = ("# written by tools/android/stub_generated.py - NOT the real game code. Compile checks only.\n"
            "set(GEN_SOURCES)\n")
    if not (os.path.exists(files_cmake) and open(files_cmake, encoding="utf-8").read() == text):
        open(files_cmake, "w", encoding="utf-8").write(text)
        written.append("files.cmake")

    funcs_h = os.path.join(OUT, "funcs.h")
    body = ["#pragma once",
            "// written by tools/android/stub_generated.py - declarations only, so the runtime compiles without the",
            "// game's recompiled code. There are no definitions here; linking needs the real generated/.",
            "",
            "struct Cpu;",
            ""]
    body += ["void %s(Cpu&);" % n for n in names]
    body.append("")
    text = "\n".join(body)
    if not (os.path.exists(funcs_h) and open(funcs_h, encoding="utf-8").read() == text):
        open(funcs_h, "w", encoding="utf-8").write(text)
        written.append("funcs.h")

    print("stub generated/: %d functions declared (%d guest, %d hle)%s"
          % (len(names), len(guest), len(hle), (", wrote " + ", ".join(written)) if written else ""))
    if not names:
        print("warning: no f_XXXXXXXX names found in runtime/ - did the layout change?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
