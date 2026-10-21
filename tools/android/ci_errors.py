#!/usr/bin/env python3
"""Turn a ninja build.log into GitHub annotations that carry their own context.

Why this exists: the build log lives in an artifact (blob storage) and `gh run view --log` is not always reachable, so
the check-run annotations are the channel that actually gets read. A bare "error:" line says what broke but not where
it was instantiated from, and for "file not found" it does not say what include paths the compiler had - which is
exactly the information needed to fix it. So each annotation here carries:

  * the error line itself and the two lines after it (clang puts the note/caret there),
  * up to six lines of "In file included from ..." / "in instantiation of ..." chain before it,
  * the compile command that failed, truncated, so the -I flags are visible.

GitHub shows at most ten error annotations per step, so at most ten are emitted; everything else still goes to the
step summary (full text) and to build.log (the whole thing).

Usage: ci_errors.py build.log [--limit N] [--report]
Without --report it prints the ::error:: workflow commands (the annotations); with it, the same findings as readable text
for the job summary instead.
"""
from __future__ import annotations

import os
import re
import sys

ERROR_RE = re.compile(r"(?::\d+:\d+: (?:fatal )?error:|^error:)")
CONTEXT_RE = re.compile(r"^(?:In file included from|.*: note: in instantiation|.*requested here|.*: note: .*required from)")
FAILED_RE = re.compile(r"^FAILED: ")
COMMAND_RE = re.compile(r"\s-c\s")


def escape(text: str) -> str:
    """Workflow-command escaping: %, CR and LF all end up as plain text inside the annotation."""
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def blocks(lines: list[str]):
    """Split the log into ninja failure blocks (one per failed compile)."""
    current: list[str] | None = None
    for line in lines:
        if FAILED_RE.match(line):
            if current:
                yield current
            current = [line.rstrip("\n")]
        elif current is not None:
            # A new [n/m] step line means the previous failure is over even if ninja printed no trailer.
            if re.match(r"^\[\d+/\d+\] ", line):
                yield current
                current = None
            else:
                current.append(line.rstrip("\n"))
    if current:
        yield current


def compile_command(block: list[str]) -> str:
    """The command ninja ran: the longest line mentioning `-c` (the [n/m] progress line is the same command)."""
    best = ""
    for line in block:
        stripped = re.sub(r"^\[\d+/\d+\] ", "", line)
        if COMMAND_RE.search(stripped) and len(stripped) > len(best):
            best = stripped
    if not best:
        for line in block:                      # linking/custom rules
            if line.startswith(("cd ", "/", "cmake ")):
                best = line
                break
    return best if len(best) <= 1600 else best[:800] + " ... " + best[-700:]


def errors(block: list[str]):
    """(index, line) for every error line in the block, keeping the file it was reported in."""
    for index, line in enumerate(block):
        if ERROR_RE.search(line) and not line.lstrip().startswith("ninja:"):
            yield index, line


def message(block: list[str], index: int, command: str) -> str:
    error = block[index]
    after = [line for line in block[index + 1:index + 3] if line.strip()]
    context = [line for line in block[max(0, index - 8):index] if CONTEXT_RE.match(line.strip())][-6:]
    parts = [error]
    parts.extend(after)
    if context:
        parts.append("... included from:")
        parts.extend(context)
    if command:
        parts.append("command: " + command)
    return "\n".join(parts)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[0])
        print("usage: ci_errors.py build.log [--limit N]", file=sys.stderr)
        return 2
    path = argv[1]
    limit = 10
    report_only = "--report" in argv
    if "--limit" in argv:
        limit = int(argv[argv.index("--limit") + 1])
    if not os.path.exists(path):
        print(f"::warning ::{escape(path)} was never written, so the compiler produced no output to report")
        return 0

    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    seen: set[str] = set()
    report: list[str] = []
    emitted = 0
    for block in blocks(lines):
        command = compile_command(block)
        for index, line in errors(block):
            text = message(block, index, command)
            key = re.sub(r":\d+:\d+", "", line)     # same complaint about the same file twice is one complaint
            if key in seen:
                continue
            seen.add(key)
            report.append(text)
            if emitted < limit:
                if not report_only:
                    print(f"::error ::{escape(text)}")
                emitted += 1

    if report_only:
        # Everything, readable, for the job summary (this is also what a human scrolls in the run page).
        print(f"errors found: {len(report)} (the first {min(emitted, len(report))} are the annotations)")
        for text in report:
            print()
            print(text)
    elif not report:
        print("::notice ::the build produced no errors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
