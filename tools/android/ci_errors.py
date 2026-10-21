#!/usr/bin/env python3
"""Turn a ninja build.log into GitHub annotations that carry their own context.

Why this exists: the build log lives in an artifact (blob storage) and `gh run view --log` is not always reachable, so
the check-run annotations are the channel that actually gets read. A bare "error:" line says what broke but not why,
and the three things needed to fix a cross-compilation failure are:

  * the *include chain* ("In file included from ..."), i.e. which of our files pulled the header in;
  * the *include path* the compiler actually had (every -I/-isystem), which immediately answers
    "fatal error: 'sqlite3.h' file not found";
  * the *notes* clang prints after the error (the "candidate constructor not viable" lines), which are the whole
    explanation for a template error such as "no matching function for call to '__construct_at'".

GitHub shows at most ten error annotations per step, so at most ten are emitted; everything is also printed as plain
text with --report for the job summary.

Usage: ci_errors.py build.log [--limit N] [--report]
       ci_errors.py configure.log --configure
       ci_errors.py apk-build.log --keywords
Without --report it prints the ::error:: workflow commands (the annotations); with it, the same findings as readable
text for the job summary instead.
"""
from __future__ import annotations

import os
import re
import sys

ERROR_RE = re.compile(r"(?::\d+:\d+: (?:fatal )?error:|^error:)")
FAILED_RE = re.compile(r"^FAILED: ")
PROGRESS_RE = re.compile(r"^\[\d+/\d+\] ")
COMMAND_RE = re.compile(r"\s-c\s")
CONTEXT_RE = re.compile(
    r"^(?:In file included from|.*: note: in instantiation|.*requested here|.*: note: .*required from)")
INCLUDE_RE = re.compile(r"(?:-I\S+|-isystem\s+\S+|-iquote\s*\S+)")
TAIL_STOP_RE = re.compile(r"^(?:ninja: |FAILED: |\[\d+/\d+\] |\d+ (?:warning|error)s? generated)")

MAX_AFTER_LINES = 30
MAX_MESSAGE = 6000


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
            if PROGRESS_RE.match(line):
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
        stripped = PROGRESS_RE.sub("", line)
        if COMMAND_RE.search(stripped) and len(stripped) > len(best):
            best = stripped
    if not best:
        for line in block:                      # linking/custom rules
            if line.startswith(("cd ", "/", "cmake ")):
                best = line
                break
    return best


def include_path(command: str) -> str:
    """Just the include flags, deduplicated: the part of a long command line that answers "header not found"."""
    seen: list[str] = []
    for flag in INCLUDE_RE.findall(command):
        flag = flag.replace("-isystem ", "-isystem ").strip()
        if flag not in seen:
            seen.append(flag)
    return " ".join(seen)


def context_chain(block: list[str], index: int) -> list[str]:
    """The contiguous run of include/instantiation lines immediately above the error."""
    chain: list[str] = []
    cursor = index - 1
    while cursor >= 0 and CONTEXT_RE.match(block[cursor].strip()):
        chain.append(block[cursor].strip())
        cursor -= 1
        if len(chain) >= 12:
            break
    chain.reverse()
    return chain


def notes(block: list[str], index: int) -> list[str]:
    """What clang printed after the error: candidate lists, "required from here", the caret lines."""
    tail: list[str] = []
    for line in block[index + 1:]:
        if TAIL_STOP_RE.match(line):
            break
        if len(tail) < MAX_AFTER_LINES:
            tail.append(line.rstrip())
    # Drop the trailing blank lines but keep the structure in between.
    while tail and not tail[-1].strip():
        tail.pop()
    return tail


def translation_unit(command: str) -> str:
    """The file being compiled, so two identical system-header errors in different files both get reported."""
    match = re.search(r"(?:^|\s)-c\s+(\S+)", command)
    return os.path.basename(match.group(1)) if match else ""


def message(block: list[str], index: int, command: str) -> str:
    parts = [block[index].rstrip()]
    parts.extend(notes(block, index))
    chain = context_chain(block, index)
    if chain:
        parts.append("... included from:")
        parts.extend(chain)
    includes = include_path(command)
    if includes:
        parts.append("include path: " + includes)
    if command:
        parts.append("compile: " + (command if len(command) <= 300 else command[:300] + " ..."))
    text = "\n".join(parts)
    return text if len(text) <= MAX_MESSAGE else text[:MAX_MESSAGE] + "\n... (truncated)"


def configure_report(lines: list[str]) -> str:
    """The reason a cmake run stopped: its "CMake Error" block(s), then the last lines before it gave up."""
    starts = [index for index, line in enumerate(lines)
              if re.search(r"CMake Error|^-- Configuring incomplete|Error at .*CMake", line)]
    body: list[str] = []
    if starts:
        first = starts[0]
        body.extend(line.rstrip() for line in lines[first:first + 60])
    else:
        body.extend(line.rstrip() for line in lines[-30:])
    text = "\n".join(body)
    text = re.sub(r"\n{3,}", "\n\n", text)
    text = text if len(text) <= MAX_MESSAGE else text[:MAX_MESSAGE] + "\n... (truncated)"
    return text


def keyword_report(lines: list[str]) -> str:
    """The reason a Gradle/CMake/NDK build stopped, when it was not a plain compiler log.

    Gradle prints the whole CMake/ninja transcript inside its "What went wrong" block - hundreds of `[12/670] Building
    ...` progress lines, with the actual failure at the very end. So the progress lines are dropped and the tail, where
    the error is, is kept.
    """
    progress = re.compile(r"^\s*\[\d+/\d+\] ")
    body = [line.rstrip() for line in lines if not progress.match(line)]
    # Keep the head of the failure (the task that failed) and above all the tail (what the compiler said).
    tail = body[-45:]
    head = body[:12]
    errors = [line for line in body if re.search(r"(?:error|Error):|fatal error:|undefined symbol|cannot find", line)][-12:]
    parts = list(head)
    parts.append("...")
    if errors:
        parts.append("... error lines:")
        parts.extend(errors)
        parts.append("...")
    parts.extend(tail)
    text = "\n".join(parts)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text if len(text) <= MAX_MESSAGE else text[:MAX_MESSAGE] + "\n... (truncated)"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[0])
        print("usage: ci_errors.py build.log [--limit N] [--report]", file=sys.stderr)
        return 2
    path = argv[1]
    limit = 10
    if "--limit" in argv:
        limit = int(argv[argv.index("--limit") + 1])
    report_only = "--report" in argv
    configure_mode = "--configure" in argv
    keyword_mode = "--keywords" in argv
    if not os.path.exists(path):
        print(f"::warning ::{escape(path)} was never written, so the compiler produced no output to report")
        return 0

    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    if configure_mode:
        print(f"::error title=cmake ::{escape(configure_report(lines))}")
        return 0
    if keyword_mode:
        print(f"::error title=build ::{escape(keyword_report(lines))}")
        return 0

    seen: set[str] = set()
    report: list[str] = []
    emitted = 0
    for block in blocks(lines):
        command = compile_command(block)
        for index, line in enumerate(block):
            if not ERROR_RE.search(line) or line.lstrip().startswith("ninja:"):
                continue
            # The same complaint about the same file is one complaint; the same libc++ error in two translation units is
            # two, because each one has its own instantiation chain.
            key = translation_unit(command) + "|" + re.sub(r":\d+:\d+", "", line)
            if key in seen:
                continue
            seen.add(key)
            text = message(block, index, command)
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
