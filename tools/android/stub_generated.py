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

# Guest functions the runtime refers to directly: gen.py names them f_XXXXXXXX (the guest address).
CALL_RE = re.compile(r"\bf_[0-9A-Fa-f]{8}\b")


def scan() -> list[str]:
    names = set()
    for folder in ("runtime", "android"):
        base = os.path.join(ROOT, folder)
        for dirpath, _, files in os.walk(base):
            for name in files:
                if not name.endswith((".cpp", ".h", ".hpp")):
                    continue
                try:
                    text = open(os.path.join(dirpath, name), encoding="utf-8", errors="ignore").read()
                except OSError:
                    continue
                names.update(CALL_RE.findall(text))
    return sorted(names)


def main() -> int:
    names = scan()
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

    print("stub generated/: %d guest functions declared%s" % (len(names), (", wrote " + ", ".join(written)) if written else ""))
    if not names:
        print("warning: no f_XXXXXXXX names found in runtime/ - did the layout change?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
