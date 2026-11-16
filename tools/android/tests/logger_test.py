#!/usr/bin/env python3
"""Scenario tests for logger_android.cpp: every run() is one app process."""
import os, re, shutil, subprocess, sys, tempfile, time

if len(sys.argv) != 2:
    sys.exit("usage: %s <path to the harness executable>" % sys.argv[0])
HARNESS = os.path.abspath(sys.argv[1])
failures = 0; checks = 0; current = ""

def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL [{current}] {what}")

def run(directory, mode, arg="", timeout=120, extra_env=None):
    env = {"PATH": os.environ.get("PATH", ""), "SHADOWRECOMP_APP_VERSION": "1.2.3-test"}
    if directory is not None:
        env["SHADOWRECOMP_EXTERNAL_DIR"] = directory
        env["SHADOWRECOMP_INTERNAL_DIR"] = directory
    env.update(extra_env or {})
    return subprocess.run([HARNESS, mode, arg], env=env, capture_output=True, text=True, timeout=timeout)

def read(path):
    with open(path, errors="replace") as f:
        return f.read()

current = "first run"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "plain", "1")
check(p.returncode == 0, f"exit code {p.returncode} {p.stderr[-300:]}")
log = read(os.path.join(d, "log.txt"))
check("Shadow Recompiled log started" in log, "start line")
check("apk_version=1.2.3-test" in log, "version line")
check("device=TestPhone" in log, "device line")
check("run_id=1 " in log, "run id 1")
check("hello from run 1" in log, "info line")
check("[warn]" in log and "a warning" in log, "warning tagged")
check("[error]" in log and "an error" in log, "error tagged")
check(log.count("location=in game") == 1, "location logged once per change")
check("location=unknown" in log, "empty location normalised")
check(all(re.match(r"\[ *\d+\.\d{3}\]\[t\d+\]", line) for line in log.splitlines()), "every line has time and thread")
check(sorted(os.listdir(d)) == ["log.txt", "run_counter.txt"], f"only the log and the counter: {sorted(os.listdir(d))}")
check(p.stdout.strip().startswith("logcat_lines=") and int(p.stdout.strip().split("=")[1]) >= 3, "mirrored to logcat")

current = "rotation and pruning"
for n in range(2, 10):
    run(d, "plain", str(n))
names = sorted(os.listdir(d))
check("hello from run 9" in read(os.path.join(d, "log.txt")), "log.txt is the latest run")
check("hello from run 8" in read(os.path.join(d, "log_prev.txt")), "log_prev.txt is the run before")
archives = [n for n in names if n.startswith("log_run_")]
check(archives == [f"log_run_{i:06d}.txt" for i in range(4, 9)], f"the last five runs are archived: {archives}")
for i in range(4, 9):
    check(f"hello from run {i}" in read(os.path.join(d, f"log_run_{i:06d}.txt")), f"archive {i} holds run {i}")
check(read(os.path.join(d, "run_counter.txt")).strip() == "9", "run counter")
# Files that merely look similar are not touched; stale archives from "the future" go.
for name in ("log_run_000001.txt.bak", "log_run_abc.txt", "my_log_run_000001.txt", "log_run_000001.txtx"):
    open(os.path.join(d, name), "w").write("keep")
open(os.path.join(d, "log_run_999999.txt"), "w").write("stale")
os.makedirs(os.path.join(d, "driver_import"))
run(d, "plain", "10")
names = sorted(os.listdir(d))
for name in ("log_run_000001.txt.bak", "log_run_abc.txt", "my_log_run_000001.txt", "log_run_000001.txtx", "driver_import"):
    check(name in names, f"{name} untouched")
check("log_run_999999.txt" not in names, "archive numbered ahead of the counter removed")
check([n for n in names if re.fullmatch(r"log_run_\d+\.txt", n)] == [f"log_run_{i:06d}.txt" for i in range(5, 10)], "window moved on")
# A lost counter restarts numbering; old archives do not pile up.
os.remove(os.path.join(d, "run_counter.txt"))
run(d, "plain", "again")
names = sorted(os.listdir(d))
check([n for n in names if re.fullmatch(r"log_run_\d+\.txt", n)] == ["log_run_000000.txt"], f"counter reset: {names}")
shutil.rmtree(d)

current = "stderr capture"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "stderr")
log = read(os.path.join(d, "log.txt"))
check("[stderr] vulkan error one" in log, "stderr line captured")
check("line two" in log, "second stderr line captured")
check(p.stderr == "", "stderr no longer reaches the terminal")
shutil.rmtree(d)

current = "crash report"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "crash")
check(p.returncode == -11, f"dies with SIGSEGV after reporting: {p.returncode}")
log = read(os.path.join(d, "log.txt"))
check("[crash] FATAL SIGNAL 11 (SIGSEGV)" in log, "signal reported")
check("location=loading the Vulkan driver" in log.split("[crash]")[1], "location repeated in the report")
check("fault_addr=0x0" in log, "fault address")
check("run_id=1 runtime_ms=" in log, "run id and runtime")
check(log.rstrip().endswith("has the full backtrace."), "report complete")
p = run(d, "abort")
check(p.returncode == -6, f"SIGABRT: {p.returncode}")
log = read(os.path.join(d, "log.txt"))
check("[crash] FATAL SIGNAL 6 (SIGABRT)" in log and "location=in game" in log, "abort reported")
check("FATAL SIGNAL 11" in read(os.path.join(d, "log_prev.txt")), "the crashed run is kept as log_prev.txt")
shutil.rmtree(d)

current = "no log directory"
p = run(None, "plain")
check(p.returncode == 0, f"runs without a log directory: {p.returncode}")
p = run("/proc/definitely/not/writable", "plain")
check(p.returncode == 0, f"runs with an unwritable log directory: {p.returncode}")
p = run("/proc/definitely/not/writable", "crash")
check(p.returncode == -11, "crash without a log file still terminates normally")

current = "threads"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "threads")
log = read(os.path.join(d, "log.txt"))
lines = [l for l in log.splitlines() if "thread " in l and " line " in l]
check(len(lines) == 16000, f"all lines present: {len(lines)}")
check(all(re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]\[\*\] thread \d line \d+", l) for l in lines), "no interleaving inside a line")
shutil.rmtree(d)

current = "line lengths"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "lengths")
check(p.returncode == 0, f"exit code {p.returncode} {p.stderr[-300:]}")
log = read(os.path.join(d, "log.txt"))
lines = log.splitlines()
for n in range(900, 1101):
    text = "<" + chr(ord("a") + n % 26) * n + ">"
    wanted = ("[function_name] " if n % 2 else " ") + text
    found = [l for l in lines if l.endswith(wanted) and re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]", l[:-len(wanted)])]
    if len(found) != 1:
        check(False, f"a {n} character message is written whole, once: {len(found)}")
        break
else:
    check(True, "messages around the buffer size are written whole")
check(any(re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\] ", l) for l in lines), "empty message")
check(any(re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]\[warn\]\[f\] ", l) for l in lines), "empty message with tag and function")
check(any(l.endswith("[error][big] <" + "z" * 200000 + ">") for l in lines), "a very long message")
check(log.endswith("\n") and "\x00" not in log, "no stray bytes")
shutil.rmtree(d)

current = "startup timeline"
now_ms = int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000)
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "startup", extra_env={"SHADOWRECOMP_LAUNCH_UPTIME_MS": str(now_ms - 400)})
check(p.returncode == 0, f"exit code {p.returncode} {p.stderr[-300:]}")
lines = [l for l in read(os.path.join(d, "log.txt")).splitlines() if "[startup]" in l]
check(len(lines) == 5, f"one line per phase and event: {len(lines)}")
first = re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]\[startup\] first step: (\d+) ms \((\d+) ms since Launch game was pressed\)", lines[0])
second = re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]\[startup\] second step: (\d+) ms \((\d+) ms since Launch game was pressed\)", lines[1])
event = re.fullmatch(r"\[ *\d+\.\d{3}\]\[t\d+\]\[startup\] first frame presented \((\d+) ms since Launch game was pressed\)", lines[2])
check(first and second and event, f"line format: {lines[:3]}")
if first and second and event:
    check(500 <= int(first.group(1)) < 5000, f"the first phase counts from the load of the library: {first.group(1)} ms")
    check(80 <= int(second.group(1)) < 500, f"a phase lasts from the end of the previous one: {second.group(1)} ms")
    check(900 <= int(first.group(2)) < 10000, f"total counted from the launcher's time: {first.group(2)} ms")
    check(int(second.group(2)) - int(first.group(2)) >= 80, "and it keeps counting")
    check(int(second.group(2)) <= int(event.group(1)) < int(second.group(2)) + 5000, "an event carries only the total")
check(len(lines[3]) < 400 and "pppp" in lines[3] and len(lines[4]) < 400 and "eeee" in lines[4], "overlong names are cut")
check(int(p.stdout.strip().split("=")[1]) >= 8, "mirrored to logcat")
shutil.rmtree(d)
# Without a believable time from the launcher the total counts from the load of the library.
cases = [("none", None), ("empty", ""), ("not a number", "soon"), ("trailing text", str(now_ms - 400) + "ms"),
         ("zero", "0"), ("negative", "-5"), ("in the future", str(now_ms + 3600000))]
if now_ms > 300000: # needs a machine that has been up for longer than the logger looks back
    cases.append(("a launch long past", str(now_ms - 200000)))
for label, value in cases:
    d = tempfile.mkdtemp(prefix="logtest_")
    p = run(d, "startup", extra_env=None if value is None else {"SHADOWRECOMP_LAUNCH_UPTIME_MS": value})
    lines = [l for l in read(os.path.join(d, "log.txt")).splitlines() if "[startup]" in l]
    match = len(lines) == 5 and re.search(r"first step: (\d+) ms \((\d+) ms since the game library was loaded\)", lines[0])
    check(bool(match) and abs(int(match.group(1)) - int(match.group(2))) <= 1, f"launch time {label}: counted from the library load")
    shutil.rmtree(d)

current = "size limit"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "flood", timeout=300)
check(p.returncode == -11, f"flood run ends in the crash: {p.returncode}")
size = os.path.getsize(os.path.join(d, "log.txt"))
check(32 * 1024 * 1024 <= size <= 33 * 1024 * 1024, f"log stops at the limit: {size}")
log = read(os.path.join(d, "log.txt"))
check(log.count("Log size limit reached") == 1, "limit reported once")
tail = log[log.index("Log size limit reached"):]
check("xxxxxxxx" not in tail, "nothing but reports after the limit")
check("[crash] FATAL SIGNAL 11" in tail, "the crash is still recorded")
shutil.rmtree(d)

current = "hang watchdog"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "hang")
log = read(os.path.join(d, "log.txt"))
check("HANG DETECTED: no frame presented for" in log, "hang detected")
dump = [l for l in log.splitlines() if "[watchdog] tid " in l]
check(len(dump) >= 2 and all("state=" in l and "wchan=" in l for l in dump), f"thread dump written: {len(dump)} threads")
check("RESUMED at frame #" in log, "resume noted")
check(log.index("HANG DETECTED") < log.index("RESUMED"), "in order")
shutil.rmtree(d)

current = "watchdog suspended"
d = tempfile.mkdtemp(prefix="logtest_")
p = run(d, "suspended")
log = read(os.path.join(d, "log.txt"))
check("HANG DETECTED" not in log, "a backgrounded game is not a hang")
check("alive: frame #" in log, "liveness notes while running")
shutil.rmtree(d)

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
