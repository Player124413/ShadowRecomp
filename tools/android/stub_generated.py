#!/usr/bin/env python3
"""Write stand-in `generated/` files so the native build can be *compiled* without the game.

`generated/` normally comes from `python tools/gen.py`, which needs the player's own disc image and is never
committed. That makes a plain CI checkout unbuildable, which is a bad place to be: the Android port's code is
where breakage hides, and nobody wants to find it on their phone.

So this script synthesises what the build needs from `generated/`:

  generated/files.cmake  - GEN_SOURCES, empty (no recompiled game code to add)
  generated/funcs.h      - a declaration for every guest function the runtime calls by name
  generated/stub_definitions.cpp   (--definitions only) - an empty body for each of them, so that the native library
                           can also be *linked* and an APK assembled. CMake only compiles it with
                           -DSHADOW_STUB_GAME_CODE=ON, so it cannot end up in a real build by accident.

Without --definitions the result compiles everything (runtime + Aurora + the Android glue) and links nothing: a link
would need the real recompiled functions. That is exactly what "does the port still build?" needs to know.

With --definitions the app links, installs and shows its launcher, but it cannot play anything: every guest function
returns immediately. Use it to check the APK pipeline on a machine that has no disc. `python tools/gen.py` remains the
only way to get a working game.

    python3 tools/android/stub_generated.py                 # compile only (CI's compile job)
    python3 tools/android/stub_generated.py --definitions   # ... and linkable, for a launcher-only APK
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
# Some overrides are written with a helper macro, e.g. WS_RES_HOOK(hle_ResByName34, f_80014640, "name", 0x34), which
# defines that function. Any ALL_CAPS(...) invocation naming an hle_ function is treated as a definition.
HLE_MACRO_DEF = re.compile(r"^[ \t]*[A-Z][A-Z0-9_]*\s*\(\s*(hle_[A-Za-z0-9_]+)", re.MULTILINE)


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


def scan() -> tuple[list[str], list[str], set[str]]:
    guest = set()
    hle = set()
    odd_signature = set()
    defined = set()
    texts = list(sources())
    for _, text in texts:
        guest.update(CALL_RE.findall(text))
        hle.update(HLE_RE.findall(text))
        defined.update(HLE_MACRO_DEF.findall(text))
        for name, params in HLE_DEF.findall(text):
            defined.add(name)
            # count the parameters: a "(Cpu& c, uint32_t target)" definition cannot be declared as "(Cpu&)"
            if params.count(",") >= 1 and params.strip():
                odd_signature.add(name)
    hle -= odd_signature
    return sorted(guest), sorted(hle), defined


def definitions(guest: list[str], hle: list[str], defined: set[str]) -> str:
    """Stand-in bodies for the guest functions, for a launcher-only build (see the module docstring)."""
    body = ["// written by tools/android/stub_generated.py - NOT the game's code.",
            "//",
            "// One empty body per guest function the runtime calls by name. It exists so a launcher-only APK can be built",
            "// and installed before the player has run tools/gen.py against their own disc; it plays nothing. The guard",
            "// below is deliberate: a normal configure does not define SHADOW_STUB_GAME_CODE, so a real build cannot",
            "// silently link this file and ship an app that does nothing.",
            "#ifndef SHADOW_STUB_GAME_CODE",
            "#error \"stub_definitions.cpp is a stand-in for the game code; configure with -DSHADOW_STUB_GAME_CODE=ON\"",
            "#endif",
            "",
            "#include \"funcs.h\"",
            "",
            "// Weak: a real definition elsewhere in the runtime (several hle_* overrides are written with a macro, which is",
            "// why they are not easy to spot here) then wins over the empty stand-in instead of colliding with it.",
            "#if defined(__GNUC__) || defined(__clang__)",
            "#define SHADOW_STUB_WEAK __attribute__((weak))",
            "#else",
            "#define SHADOW_STUB_WEAK",
            "#endif",
            ""]
    body += ["SHADOW_STUB_WEAK void %s(Cpu&) {}" % n for n in guest]
    # hle_* overrides that the runtime does not define itself. The weak attribute covers the ones this filter misses.
    body += ["SHADOW_STUB_WEAK void %s(Cpu&) {}" % n for n in hle if n not in defined]
    body.append("")
    return "\n".join(body)


def main() -> int:
    write_definitions = "--definitions" in sys.argv[1:]
    guest, hle, defined = scan()
    names = guest + hle
    os.makedirs(OUT, exist_ok=True)
    written = []
    files_cmake = os.path.join(OUT, "files.cmake")
    text = ("# written by tools/android/stub_generated.py - NOT the real game code. Compile checks only.\n"
            "set(GEN_SOURCES)\n")
    if not (os.path.exists(files_cmake) and open(files_cmake, encoding="utf-8").read() == text):
        open(files_cmake, "w", encoding="utf-8").write(text)
        written.append("files.cmake")

    if write_definitions:
        stub_cpp = os.path.join(OUT, "stub_definitions.cpp")
        text = definitions(guest, hle, defined)
        if not (os.path.exists(stub_cpp) and open(stub_cpp, encoding="utf-8").read() == text):
            open(stub_cpp, "w", encoding="utf-8").write(text)
            written.append("stub_definitions.cpp")

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
