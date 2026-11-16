#!/usr/bin/env python3
"""Scenario tests for vulkan_pipeline_cache.cpp: every run() is one app process."""
import os, shutil, struct, subprocess, sys, tempfile

if len(sys.argv) != 2:
    sys.exit("usage: %s <path to the harness executable>" % sys.argv[0])
HARNESS = os.path.abspath(sys.argv[1])
HEADER = 56            # FileHeader of vulkan_pipeline_cache.cpp
DRIVER_HEADER = 32     # VkPipelineCacheHeaderVersionOne
failures = 0; checks = 0; current = ""

def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL [{current}] {what}")

def scenario(name):
    global current
    current = name

class Phone:
    """The cache directory of one installation, plus the driver the device has."""
    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="pctest_")
        self.file = os.path.join(self.root, "cache", "vulkan_pipeline_cache.bin")
        self.marker = self.file + ".loading"
        self.env = {}

    def run(self, *operations, timeout=120):
        env = {"PATH": os.environ.get("PATH", ""), "ASAN_OPTIONS": "detect_leaks=0"}
        env.update(self.env)
        p = subprocess.run([HARNESS, self.file] + list(operations), env=env, capture_output=True, text=True, timeout=timeout)
        out = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
        result = {key: int(value) for key, value in out.items()}
        result["rc"] = p.returncode
        result["log"] = p.stderr
        return result

    def ok(self, *operations):
        r = self.run(*operations)
        if r["rc"] != 0 or "hooked" not in r:
            check(False, f"harness exited {r['rc']}: {r['log'][-600:]}")
            for key in ("cache_created", "create_cache_calls", "initial_data", "driver_ignored_data", "hooked",
                        "hits", "misses", "uncached", "entries", "get_data_calls"):
                r.setdefault(key, -999)
        return r

    def bytes(self):
        try:
            with open(self.file, "rb") as f:
                return f.read()
        except OSError:
            return b""

    def write(self, data):
        with open(self.file, "wb") as f:
            f.write(data)

    def rehash(self):
        check(subprocess.run([HARNESS, "--rehash", self.file]).returncode == 0, "rehash helper")

    def identity(self):
        try:
            return os.stat(self.file).st_ino
        except OSError:
            return None

    def seed(self, ids="1,2,3,4,5"):
        """A file written and confirmed by earlier runs."""
        self.ok("init", "create:" + ids, "save")
        return self.bytes()

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)

def entries(n):
    return DRIVER_HEADER + 4 + 4 * n

# ---------------------------------------------------------------------------------------
scenario("first run")
d = Phone()
r = d.ok("init", "create:1,2,3,4,5", "save")
check(r["cache_created"] == 1 and r["create_cache_calls"] == 1 and r["initial_data"] == 0, "an empty cache is created")
check(r["hooked"] == 1, "pipeline creation is routed through it")
check(r["misses"] == 5 and r["hits"] == 0 and r["uncached"] == 0, f"every pipeline is compiled: {r}")
check("starting empty, there is no file yet" in r["log"], "the log says why it starts empty")
check(os.path.isfile(d.file), "the cache is written, its directory created")
check(not os.path.exists(d.marker), "nothing was read, so there is no marker")
check(sorted(os.listdir(os.path.dirname(d.file))) == ["vulkan_pipeline_cache.bin"], "no temporary file left")
data = d.bytes()
magic, version, vendor, device, driver_version = struct.unpack_from("<4sIIII", data, 0)
uuid = data[20:36]
size = struct.unpack_from("<Q", data, 40)[0]
check(magic == b"SRPC" and version == 1, f"file header: {magic} {version}")
check((vendor, device, driver_version) == (0x5143, 0x43050A01, 100) and uuid == b"u" * 16, "the driver is named in it")
check(size == entries(5) and len(data) == HEADER + size, f"followed by exactly the driver's data: {size} of {len(data)}")
check(struct.unpack_from("<II", data, HEADER) == (DRIVER_HEADER, 1), "which starts with the driver's own header")

scenario("second run")
identity = d.identity()
r = d.ok("init", "create:1,2,3,4,5", "save", "confirm")
check(r["initial_data"] == entries(5) and r["create_cache_calls"] == 1, f"the driver is given the data: {r['initial_data']}")
check(r["driver_ignored_data"] == 0 and r["hits"] == 5 and r["misses"] == 0, "and compiles nothing again")
check("read 0 KiB from an earlier run" in r["log"], "the log says so")
check(d.identity() == identity and d.bytes() == data, "nothing new: the file is not written again")
check(not os.path.exists(d.marker), "marker gone once the data is confirmed")
r = d.ok("init", "save", "confirm")
check(r["get_data_calls"] == 1, "a save with no pipeline created does not even ask the driver")
r = d.ok("init", "confirm", "create:1", "save", "create:2", "save", "save")
check(r["get_data_calls"] == 3 and d.identity() == identity, "pipelines that were all cached cost one size query each save")

scenario("new pipelines are added to the file")
r = d.ok("init", "create:1,6,7", "save", "confirm")
check(r["hits"] == 1 and r["misses"] == 2, "only the new ones are compiled")
check(d.identity() != identity and len(d.bytes()) == HEADER + entries(7), "and the file grows")
check("wrote 0 KiB" in r["log"], "the log notes the write")
r = d.ok("init", "create:6,7", "confirm")
check(r["hits"] == 2 and r["entries"] == 7, "the next run has them")
# Several saves in one run: each writes only if something was added since the last.
identity = d.identity()
r = d.ok("init", "confirm", "create:8", "save", "create:8", "save", "create:9", "save", "save")
check(len(d.bytes()) == HEADER + entries(9) and r["log"].count("wrote") == 2, f"two of four saves write: {r['log'].count('wrote')}")
d.close()

scenario("a run that ends before the data is confirmed")
d = Phone()
d.seed()
r = d.ok("init", "create:1,2")
check(r["hits"] == 2 and os.path.exists(d.marker), "the marker stays behind")
r = d.ok("init", "create:1,2")
check(r["initial_data"] == 0 and r["misses"] == 2, "the next run does not read the file again")
check("previous run ended before the data it read was confirmed" in r["log"], "and says why")
check(not os.path.exists(d.file) and not os.path.exists(d.marker), "file and marker are removed")
r = d.ok("init", "create:1,2", "save")
check(os.path.isfile(d.file) and not os.path.exists(d.marker), "what it compiles is saved as usual")
r = d.ok("init", "create:1,2", "confirm")
check(r["hits"] == 2 and not os.path.exists(d.marker), "and used by the run after")
# Confirming without having read anything, twice, or saving first: all harmless.
d2 = Phone()
r = d2.ok("init", "confirm", "confirm", "create:1", "save", "confirm")
check(r["misses"] == 1 and os.path.isfile(d2.file) and not os.path.exists(d2.marker), "confirm with nothing read")
d2.close()
d.close()

scenario("a driver that dies of the data")
d = Phone()
d.seed()
d.env["TEST_CRASH_ON_DATA"] = "1"
r = d.run("init", "create:1")
check(r["rc"] == -6, f"the run that hands it the data dies: {r['rc']}")
check(os.path.exists(d.marker) and os.path.exists(d.file), "leaving the marker")
r = d.ok("init", "create:1", "save")
check(r["initial_data"] == 0 and r["misses"] == 1, "the next run starts empty instead of dying the same way")
r = d.run("init", "create:1")
check(r["rc"] == -6, "(the data of that run is tried once, too)")
r = d.ok("init", "create:1")
check(r["initial_data"] == 0, "and dropped again: never two crashes in a row")
d.close()

scenario("a driver that turns the data down")
d = Phone()
d.seed()
d.env["TEST_REJECT_DATA"] = "1"
r = d.ok("init", "create:1,2", "save")
check(r["create_cache_calls"] == 2 and r["initial_data"] == 0 and r["cache_created"] == 1, "an empty cache is created instead")
check(r["hooked"] == 1 and r["misses"] == 2, "and used")
check("did not accept the data" in r["log"], "the log says why")
check(not os.path.exists(d.marker) and len(d.bytes()) == HEADER + entries(2), "the rejected file is replaced, no marker left")
d.close()

scenario("a driver without pipeline caches")
d = Phone()
d.env["TEST_NO_CACHE"] = "1"
r = d.ok("init", "create:1,2", "save", "confirm")
check(r["cache_created"] == 0 and r["hooked"] == 0, "pipeline creation is left alone")
check(r["uncached"] == 2, "pipelines are still created")
check("could not create one" in r["log"], "with a warning")
check(not os.path.exists(d.file) and not os.path.exists(d.marker) and r["get_data_calls"] == 0, "nothing is written")
d.close()

scenario("damaged files")
d = Phone()
good = d.seed()
def damaged(label, data, reason):
    d.write(data)
    if os.path.exists(d.marker): os.remove(d.marker)
    r = d.ok("init", "create:1,2,3,4,5,6", "save")
    check(r["initial_data"] == 0 and r["misses"] == 6, f"{label}: not handed to the driver ({r['initial_data']})")
    check(reason in r["log"], f"{label}: reason given ({r['log'].splitlines()[0][-70:] if r['log'] else ''})")
    check(not os.path.exists(d.marker), f"{label}: no marker for data that was not read")
    check(len(d.bytes()) == HEADER + entries(6), f"{label}: replaced by a good file")
    r = d.ok("init", "create:6", "confirm")
    check(r["hits"] == 1, f"{label}: which the next run reads")

flipped = bytearray(good); flipped[HEADER + DRIVER_HEADER + 6] ^= 0x01
damaged("one bit flipped in the data", bytes(flipped), "the file is damaged")
flipped = bytearray(good); flipped[48] ^= 0x80
damaged("checksum altered", bytes(flipped), "the file is damaged")
damaged("truncated", good[:-1], "incomplete or has trailing data")
damaged("cut in the header", good[:30], "not a pipeline cache")
damaged("header only", good[:HEADER], "incomplete or has trailing data")
damaged("empty", b"", "not a pipeline cache")
damaged("trailing byte", good + b"\0", "incomplete or has trailing data")
damaged("other magic", b"XXXX" + good[4:], "not a pipeline cache")
damaged("other version", good[:4] + struct.pack("<I", 2) + good[8:], "another version of the game")
damaged("size too small", good[:40] + struct.pack("<Q", 8) + good[48:], "implausible size")
damaged("size absurd", good[:40] + struct.pack("<Q", 1 << 40) + good[48:], "implausible size")
damaged("size one more than there is", good[:40] + struct.pack("<Q", len(good) - HEADER + 1) + good[48:], "incomplete or has trailing data")
damaged("text", b"this is not a cache at all, just a text file of some length to get past the header\n" * 3, "not a pipeline cache")
# The driver's own header inside intact data: checked here as well, not left to the driver.
for label, offset, value in (("header size", 0, struct.pack("<I", 8)), ("header size beyond the data", 0, struct.pack("<I", 4096)),
                             ("header version", 4, struct.pack("<I", 2)), ("vendor", 8, struct.pack("<I", 0x13B5)),
                             ("device", 12, struct.pack("<I", 7)), ("UUID", 16, b"v" * 16)):
    inner = bytearray(good)
    inner[HEADER + offset:HEADER + offset + len(value)] = value
    d.write(bytes(inner)); d.rehash()
    data = d.bytes()
    check(data != good and len(data) == len(good), f"driver {label}: test file prepared")
    damaged(f"driver {label} differs", data, "does not belong to this graphics driver")
# A directory where the file should be.
os.remove(d.file); os.makedirs(d.file)
r = d.ok("init", "create:1", "save")
check(r["initial_data"] == 0 and r["misses"] == 1 and "could not write" in r["log"], "a directory in its place: the run goes on without")
os.rmdir(d.file)
d.close()

scenario("another driver or device")
for label, key, value in (("vendor", "TEST_VENDOR", "0x13B5"), ("device", "TEST_DEVICE", "0x7"),
                          ("driver version", "TEST_DRIVER_VERSION", "101"), ("cache UUID", "TEST_UUID", "v")):
    d = Phone()
    d.seed()
    d.env[key] = value
    r = d.ok("init", "create:1,2,3,4,5", "save")
    check(r["initial_data"] == 0 and r["driver_ignored_data"] == 0, f"other {label}: the old data never reaches the driver")
    check("written by another graphics driver" in r["log"], f"other {label}: reason given")
    check(r["misses"] == 5 and not os.path.exists(d.marker), f"other {label}: compiled again")
    r = d.ok("init", "create:1,2,3,4,5", "confirm")
    check(r["hits"] == 5, f"other {label}: and cached for that driver")
    # Back on the first driver its data is gone: one file, for the driver in use.
    del d.env[key]
    r = d.ok("init", "create:1", "save")
    check(r["initial_data"] == 0, f"other {label}: switching back starts over")
    d.close()

scenario("the cache grows while it is saved")
d = Phone()
d.env["TEST_GROW_ON_QUERY"] = "1"
r = d.ok("init", "create:1,2,3", "save")
check(os.path.isfile(d.file) and len(d.bytes()) == HEADER + entries(3), "what fits is written")
del d.env["TEST_GROW_ON_QUERY"]
r = d.ok("init", "create:1,2,3", "confirm")
check(r["driver_ignored_data"] == 0 and r["hits"] == 3, "and is usable by itself")
d.close()
d = Phone()
d.env["TEST_GROW_ON_QUERY"] = "1"
r = d.ok("init", "create:1,2,3", "save", "create:9", "save")
check(os.path.isfile(d.file) and len(d.bytes()) == HEADER + entries(5) and r["log"].count("wrote") == 2, "the rest follows with the next save")
d.close()

scenario("a driver with absurd amounts of data")
d = Phone()
d.env["TEST_HUGE"] = "1"
r = d.ok("init", "create:1", "save", "create:2", "save")
check(not os.path.exists(d.file), "nothing is written")
check(r["log"].count("is not written to disk") == 1, "warned about once")
d.close()

scenario("a driver that reports its data at another size")
d = Phone()
d.seed()
d.env["TEST_SIZE_DRIFT"] = "1"
identity = d.identity()
r = d.ok("init", "create:1,2,3,4,5", "save", "confirm")
check(r["hits"] == 5 and d.identity() == identity, "unchanged data is not written again because of it")
r = d.ok("init", "create:6", "save", "confirm")
check(d.identity() != identity and len(d.bytes()) == HEADER + entries(6) + 8, "new data is")
d.close()

scenario("threads")
d = Phone()
r = d.ok("init", "threads:8:300", "save")
check(r["misses"] == 2400 and r["entries"] == 2400, f"all created: {r['misses']}")
check(len(d.bytes()) == HEADER + entries(2400), "the last save has everything")
check(not [f for f in os.listdir(os.path.dirname(d.file)) if f.endswith(".tmp")], "no temporary file left")
r = d.ok("init", "threads:8:300", "save")
check(r["hits"] == 2400 and r["misses"] == 0, "and the next run compiles none of them")
check(not os.path.exists(d.marker), "confirmed from another thread than the one saving")
d.close()

scenario("storage trouble")
d = Phone()
d.file = "/proc/definitely/not/writable/cache.bin"; d.marker = d.file + ".loading"
r = d.ok("init", "create:1", "save", "create:2", "save", "confirm")
check(r["hooked"] == 1 and r["misses"] == 2 and r["log"].count("could not write") == 2, "an unwritable location only costs the reuse")
d.close()
d = Phone()
d.seed()
with open(d.file + ".tmp", "wb") as f: f.write(b"left by a save that was killed")
r = d.ok("init", "create:7", "save", "confirm")
check(len(d.bytes()) == HEADER + entries(6) and not os.path.exists(d.file + ".tmp"), "a leftover temporary file is overwritten")
# The file of an unconfirmed run cannot be removed (here: it is a non-empty directory).
os.remove(d.file); os.makedirs(os.path.join(d.file, "x")); open(d.marker, "w").close()
r = d.ok("init", "create:1", "save")
check(r["initial_data"] == 0 and "could not be removed" in r["log"], "not read, and said so")
check(os.path.exists(d.marker), "the marker stays while the file it warns about does")
shutil.rmtree(d.file)
r = d.ok("init", "create:1", "save")
check(r["initial_data"] == 0 and not os.path.exists(d.marker) and os.path.isfile(d.file), "until a run could deal with it")
d.close()

scenario("misuse")
d = Phone()
r = d.ok("save", "confirm", "create:1")
check(r["hooked"] == 0 and r["uncached"] == 1 and r["create_cache_calls"] == 0, "save and confirm before initialization do nothing")
r = d.ok("init-nothing", "create:1", "save")
check(r["hooked"] == 0 and r["create_cache_calls"] == 0 and not os.path.exists(d.file), "no device or no path: nothing happens")
r = d.ok("init", "create:1", "init", "create:2", "save")
check(r["create_cache_calls"] == 1 and r["misses"] == 2 and len(d.bytes()) == HEADER + entries(2), "a second initialization is ignored")
d.close()

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
