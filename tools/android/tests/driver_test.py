#!/usr/bin/env python3
"""Scenario tests for vulkan_driver_android.cpp: every launch() is one app process."""
import json, os, shutil, subprocess, sys, tempfile, zipfile

if len(sys.argv) != 2:
    sys.exit("usage: %s <path to the harness executable>" % sys.argv[0])
HARNESS = os.path.abspath(sys.argv[1])
AUTO, SYSTEM, BUNDLED, VAUZI, A725, A8XX, IMPORTED = range(7)
M_AUTO, M_GMEM, M_SYSMEM = range(3)
N_BUNDLED = "vulkan.unleashed26_1_wfm_a732.so"
N_VAUZI = "vulkan.vauzi710_v2_7.so"
N_A725 = "vulkan.wb26_2_rp_pair_ccu_color_a725.so"
N_A8XX = "vulkan.mesa26_3_gralloc_a8xx.so"
ADRENO = {"ro.hardware.vulkan": "adreno", "ro.hardware.egl": "adreno"}
MALI = {"ro.hardware.vulkan": "mali", "ro.hardware.egl": "mali"}
XCLIPSE = {"ro.hardware.vulkan": "samsung", "ro.hardware.egl": "samsung"}

failures = 0
checks = 0
current = ""

def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL [{current}] {what}")

class Device:
    def __init__(self, props=ADRENO, assets=True, media=True):
        self.root = tempfile.mkdtemp(prefix="drvtest_")
        self.internal = os.path.join(self.root, "internal")
        self.external = os.path.join(self.root, "external")
        self.media = os.path.join(self.root, "media") if media else ""
        self.lib = os.path.join(self.root, "lib")
        self.assets = os.path.join(self.root, "assets")
        for d in (self.internal, self.external, self.lib, self.assets):
            os.makedirs(d)
        if media:
            os.makedirs(self.media)
        self.props = dict(props)
        self.extra_env = {}
        if assets:
            self.put_assets()

    def put_assets(self, salt=b""):
        d = os.path.join(self.assets, "bundled_driver")
        os.makedirs(d, exist_ok=True)
        for name in (N_BUNDLED, N_VAUZI, N_A725, N_A8XX):
            with open(os.path.join(d, name), "wb") as f:
                f.write(b"ELF:" + name.encode() + salt)

    @property
    def turnip(self): return os.path.join(self.internal, "turnip")
    @property
    def marker(self): return os.path.join(self.internal, "vulkan_startup_state.txt")
    @property
    def imports(self): return os.path.join(self.external, "driver_import")

    def phase(self):
        if not os.path.exists(self.marker):
            return None
        for line in open(self.marker).read().splitlines():
            if line.startswith("phase="):
                return line[6:]
        return "?"

    def launch(self, driver, mode=M_AUTO, ok=True, health=None):
        env = {"PATH": os.environ.get("PATH", ""),
               "SHADOWRECOMP_INTERNAL_DIR": self.internal,
               "SHADOWRECOMP_EXTERNAL_DIR": self.external,
               "SHADOWRECOMP_MEDIA_DIR": self.media,
               "SHADOWRECOMP_NATIVE_LIB_DIR": self.lib,
               "TEST_ASSETS_DIR": self.assets,
               "ASAN_OPTIONS": "detect_leaks=0"}
        for k, v in self.props.items():
            env["PROP_" + k.replace(".", "_")] = v
        env.update(self.extra_env)
        argv = [HARNESS, str(driver), str(mode), "1" if ok else "0"]
        if health is not None:
            argv.append(health)
        p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=60)
        out = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
        out["rc"] = p.returncode
        out["log"] = p.stderr
        if p.returncode != 0 or "custom" not in out:
            check(False, f"harness exited {p.returncode}: {p.stderr[-600:]}")
            out.setdefault("custom", "?"); out.setdefault("driver", "?"); out.setdefault("tu_debug", "?")
            out.setdefault("fd_dev_features", "?")
        return out

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)

def scenario(name):
    global current
    current = name

def read(path):
    with open(path) as f:
        return f.read()

# ---------------------------------------------------------------------------------------
scenario("fresh install on Adreno, Auto")
d = Device()
r = d.launch(AUTO)
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, f"bundled driver loaded: {r['driver']}")
check(r["driver_dir"] == d.turnip + "/", "driver dir is <internal>/turnip/")
check(r["hook_dir"] == d.lib, "hook libs come from the native library dir")
check(r["tu_debug"] == "none", f"TU_DEBUG none: {r['tu_debug']}")
check(r["fd_dev_features"] == "-", "no FD_DEV_FEATURES")
for name in (N_BUNDLED, N_VAUZI, N_A725, N_A8XX):
    check(os.path.isfile(os.path.join(d.turnip, name)), f"{name} provisioned")
    check(read(os.path.join(d.turnip, name)) == "ELF:" + name, f"{name} contents")
check(read(os.path.join(d.turnip, "driver_name.txt")) == N_BUNDLED, "driver_name.txt defaults to bundled")
check(d.phase() is None, "marker cleared after a successful start")
check(os.path.isfile(os.path.join(d.imports, "readme.txt")), "import readme written")
check(os.path.isfile(os.path.join(d.media, "driver_import", "readme.txt")), "media import readme written")
readme = read(os.path.join(d.imports, "readme.txt"))
check("force_no_bc" not in readme and "shimmer" not in readme and "capture" not in readme, "readme has no stale claims")
check(not [f for f in os.listdir(d.turnip) if f.endswith(".tmp")], "no temp files left")
check(r["location"] == "starting the renderer", "log location updated")
# An APK update that ships different driver bytes replaces the installed copies.
d.put_assets(b"-v2")
r = d.launch(AUTO)
check(read(os.path.join(d.turnip, N_BUNDLED)) == "ELF:" + N_BUNDLED + "-v2", "updated asset replaces the installed driver")
d.close()

scenario("crash recovery: custom fails once")
d = Device()
r = d.launch(BUNDLED, ok=False)
check(r["custom"] == "1" and d.phase() == "custom_pending", f"marker armed: {d.phase()}")
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "0" and r["driver"] == "-", "second launch is forced to the system driver")
check(r["tu_debug"] == "none", "recovery uses render mode Auto")
check(d.phase() is None, "marker cleared once the fallback works")
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, "third launch tries the configured driver again")
check(d.phase() is None, "no marker after success")
d.close()

scenario("crash recovery: fallback fails too")
d = Device()
d.launch(BUNDLED, M_SYSMEM, ok=False)
check(d.phase() == "custom_pending", "L1 custom_pending")
r = d.launch(BUNDLED, M_SYSMEM, ok=False)
check(r["custom"] == "0" and d.phase() == "safe_fallback_pending", f"L2 forced system, phase {d.phase()}")
r = d.launch(BUNDLED, M_SYSMEM, ok=False)
check(r["custom"] == "1" and d.phase() == "recovery_exhausted", f"L3 configured driver, recovery exhausted: {r['custom']} {d.phase()}")
check(r["tu_debug"] == "sysmem", "L3 uses the configured render mode")
r = d.launch(BUNDLED, M_SYSMEM, ok=False)
check(r["custom"] == "1" and d.phase() == "recovery_exhausted", "L4 no restart loop: stays exhausted, no forced fallback")
# Changing the selection re-arms recovery for the new one.
r = d.launch(VAUZI, M_SYSMEM, ok=False)
check(r["custom"] == "1" and r["driver"] == N_VAUZI and d.phase() == "custom_pending", f"L5 new selection re-armed: {d.phase()}")
check("configured_driver=3" in read(d.marker), "marker records the new driver")
r = d.launch(VAUZI, M_SYSMEM, ok=True)
check(r["custom"] == "0" and d.phase() is None, "L6 fallback for the new selection, then cleared")
d.close()

scenario("crash recovery: selection changed to System while exhausted")
d = Device()
d.launch(BUNDLED, ok=False); d.launch(BUNDLED, ok=False); d.launch(BUNDLED, ok=False)
check(d.phase() == "recovery_exhausted", "exhausted")
r = d.launch(SYSTEM, ok=True)
check(r["custom"] == "0" and d.phase() is None, "System works and clears the state")
d.close()

scenario("crash recovery with the Imported driver (highest enum value)")
d = Device()
os.makedirs(d.imports)
with open(os.path.join(d.imports, "libvulkan_freedreno.so"), "wb") as f: f.write(b"IMPORTED")
r = d.launch(IMPORTED, ok=False)
check(r["custom"] == "1" and r["driver"] == "libvulkan_freedreno.so", f"imported driver loaded: {r['driver']}")
check(d.phase() == "custom_pending" and "configured_driver=6" in read(d.marker), "marker armed with driver 6")
r = d.launch(IMPORTED, ok=False)
check(r["custom"] == "0" and d.phase() == "safe_fallback_pending", f"L2 forced system: {d.phase()}")
check("invalid/incomplete" not in r["log"], "the marker of an Imported launch is read back as valid")
r = d.launch(IMPORTED, ok=False)
check(r["custom"] == "1" and d.phase() == "recovery_exhausted", f"L3 exhausted, configured driver tried: {r['custom']} {d.phase()}")
r = d.launch(IMPORTED, ok=True)
check(r["custom"] == "1" and d.phase() is None, "L4 success clears")
d.close()

scenario("corrupt marker")
for garbage in ("", "garbage", "version=2\nphase=custom_pending\nconfigured_driver=2\nconfigured_render_mode=0\n",
                "version=1\nphase=bogus\nconfigured_driver=2\nconfigured_render_mode=0\n",
                "version=1\nphase=custom_pending\nconfigured_driver=7\nconfigured_render_mode=0\n",
                "version=1\nphase=custom_pending\nconfigured_driver=2\nconfigured_render_mode=3\n",
                "x" * 5000):
    d = Device()
    with open(d.marker, "w") as f: f.write(garbage)
    r = d.launch(BUNDLED, ok=True)
    check(r["custom"] == "0", f"corrupt marker ({garbage[:20]!r}) forces the safe path")
    check(d.phase() is None, "and is cleared by a successful start")
    r = d.launch(BUNDLED, ok=True)
    check(r["custom"] == "1", "next launch is back to normal")
    d.close()

scenario("marker cannot be written")
d = Device()
d.launch(BUNDLED, ok=True)                      # provision first
os.makedirs(d.marker + ".tmp")                  # a directory in the way of the temp file
r = d.launch(BUNDLED, ok=False)
check(r["custom"] == "0", "an untracked custom start is refused")
check(d.phase() is None, "no marker left behind")
d.close()

scenario("System selected")
d = Device()
r = d.launch(SYSTEM, ok=False)
check(r["custom"] == "0" and r["driver"] == "-" and d.phase() is None, "system driver, never a marker")
check(os.path.isfile(os.path.join(d.turnip, N_BUNDLED)), "bundled drivers are still provisioned")
d.close()

scenario("non-Adreno GPUs")
d = Device(props=MALI)
r = d.launch(AUTO, ok=False)
check(r["custom"] == "0" and d.phase() is None, "Mali + Auto: system driver, no marker")
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, "Mali + explicit Bundled is honoured")
d.close()
d = Device(props=XCLIPSE)
r = d.launch(AUTO)
check(r["custom"] == "0" and "ExynosTools" in r["log"], "Xclipse + Auto: system driver with a hint")
d.close()
d = Device(props={"ro.hardware.vulkan": "powervr", "ro.hardware.egl": "powervr"})
r = d.launch(AUTO)
check(r["custom"] == "0", "PowerVR + Auto: system driver")
d.close()
d = Device(props={})
r = d.launch(AUTO)
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, "unknown GPU: bundled is attempted (recovery protects it)")
d.close()
d = Device(props={"ro.hardware.vulkan": "ADRENO"})
r = d.launch(AUTO)
check(r["custom"] == "1", "property match ignores case")
d.close()
# An imported driver on a non-Adreno GPU stays selected in Auto.
d = Device(props=MALI)
os.makedirs(d.imports)
with open(os.path.join(d.imports, "libvulkan_panfrost.so"), "wb") as f: f.write(b"PANVK")
r = d.launch(AUTO)
check(r["custom"] == "1" and r["driver"] == "libvulkan_panfrost.so", f"Mali + imported PanVK in Auto: {r['driver']}")
d.close()

scenario("Snapdragon 8 Elite")
d = Device(props={**ADRENO, "ro.soc.model": "SM8750"})
r = d.launch(AUTO)
check(r["driver"] == N_A8XX and r["tu_debug"] == "sysmem,nobin", f"Auto picks the 8xx driver + preset: {r['driver']} {r['tu_debug']}")
os.makedirs(d.imports, exist_ok=True)
with open(os.path.join(d.imports, "tu_debug.txt"), "w") as f: f.write("nolrz\n")
r = d.launch(AUTO)
check(r["tu_debug"] == "sysmem,nobin", "a driver preset is not overridden by tu_debug.txt")
r = d.launch(BUNDLED)
check(r["driver"] == N_BUNDLED and r["tu_debug"] == "nolrz", "explicit Bundled on that SoC, tu_debug.txt applies")
d.close()
d = Device(props={**ADRENO, "ro.soc.model": "SM8750-AC"})
check(d.launch(AUTO)["driver"] == N_A8XX, "SoC variants match by prefix")
d.close()
d = Device(props={**ADRENO, "ro.soc.model": "SM8650"})
check(d.launch(AUTO)["driver"] == N_BUNDLED, "other SoCs get the general build")
d.close()

scenario("render modes")
d = Device()
check(d.launch(BUNDLED, M_AUTO)["tu_debug"] == "none", "Bundled/Auto -> none")
check(d.launch(BUNDLED, M_GMEM)["tu_debug"] == "none", "Bundled/GMEM -> none")
check(d.launch(BUNDLED, M_SYSMEM)["tu_debug"] == "sysmem", "Bundled/Sysmem -> sysmem")
r = d.launch(VAUZI, M_AUTO)
check(r["driver"] == N_VAUZI and r["tu_debug"] == "sysmem", "Vauzi/Auto -> sysmem")
check(d.launch(VAUZI, M_GMEM)["tu_debug"] == "none", "Vauzi/GMEM -> none")
r = d.launch(A725, M_GMEM)
check(r["driver"] == N_A725 and r["tu_debug"] == "sysmem,nobin", "A725 always sysmem,nobin")
r = d.launch(A8XX, M_GMEM)
check(r["driver"] == N_A8XX and r["tu_debug"] == "sysmem,nobin", "8xx always sysmem,nobin")
check(d.launch(SYSTEM, M_SYSMEM)["tu_debug"] == "-", "System leaves TU_DEBUG alone")
os.makedirs(d.imports, exist_ok=True)
with open(os.path.join(d.imports, "tu_debug.txt"), "w") as f: f.write("  \n")
check(d.launch(BUNDLED, M_SYSMEM)["tu_debug"] == "sysmem", "blank tu_debug.txt is ignored")
with open(os.path.join(d.imports, "tu_debug.txt"), "w") as f: f.write("noubwc,nolrz \r\n")
check(d.launch(BUNDLED, M_SYSMEM)["tu_debug"] == "noubwc,nolrz", "tu_debug.txt overrides and is trimmed")
with open(os.path.join(d.imports, "tu_debug.txt"), "w") as f: f.write("x" * 4000)
check(len(d.launch(BUNDLED)["tu_debug"]) == 255, "oversized tu_debug.txt is truncated, not overflowed")
d.close()

scenario("FD_DEV_FEATURES")
d = Device(props={**ADRENO, "ro.mi.os.version.name": "OS3.0"})
check(d.launch(AUTO)["fd_dev_features"] == "enable_tp_ubwc_flag_hint=1", "HyperOS 3")
d.props["ro.mi.os.version.name"] = "OS2.0.5"
check(d.launch(AUTO)["fd_dev_features"] == "-", "HyperOS 2")
d.props["ro.mi.os.version.name"] = "OS10"
check(d.launch(AUTO)["fd_dev_features"] == "enable_tp_ubwc_flag_hint=1", "HyperOS 10")
d.props["ro.mi.os.version.name"] = "weird"
check(d.launch(AUTO)["fd_dev_features"] == "-", "no digits")
os.makedirs(d.imports, exist_ok=True)
with open(os.path.join(d.imports, "fd_dev_features.txt"), "w") as f: f.write("a=1,b=0\n")
check(d.launch(SYSTEM)["fd_dev_features"] == "a=1,b=0", "override file, also with the system driver")
d.close()

scenario("import a bare .so")
d = Device()
d.launch(AUTO)
with open(os.path.join(d.imports, "libvulkan_freedreno.so"), "wb") as f: f.write(b"A" * 100000)
with open(os.path.join(d.imports, "notes.txt"), "w") as f: f.write("keep me")
r = d.launch(AUTO)
check(r["custom"] == "1" and r["driver"] == "libvulkan_freedreno.so", f"Auto selects the import: {r['driver']}")
check(os.path.getsize(os.path.join(d.turnip, "libvulkan_freedreno.so")) == 100000, "copied byte for byte")
check(os.stat(os.path.join(d.turnip, "libvulkan_freedreno.so")).st_mode & 0o777 == 0o755, "mode 0755")
check(read(os.path.join(d.turnip, "driver_name.txt")) == "libvulkan_freedreno.so", "driver_name.txt")
check(read(os.path.join(d.turnip, "last_imported_driver.txt")) == "libvulkan_freedreno.so", "last_imported_driver.txt")
check(not os.path.exists(os.path.join(d.imports, "libvulkan_freedreno.so")), "source moved away")
check(os.path.isfile(os.path.join(d.imports, "installed", "libvulkan_freedreno.so")), "into installed/")
check(os.path.isfile(os.path.join(d.imports, "notes.txt")), "unrelated files untouched")
check(d.launch(BUNDLED)["driver"] == N_BUNDLED, "explicit Bundled still loads the bundled build")
check(d.launch(IMPORTED)["driver"] == "libvulkan_freedreno.so", "Imported loads it")
check(d.launch(AUTO)["driver"] == "libvulkan_freedreno.so", "Auto keeps it on later launches")
# The imported file disappears: Auto falls back to bundled, Imported to the system driver.
os.remove(os.path.join(d.turnip, "libvulkan_freedreno.so"))
check(d.launch(AUTO)["driver"] == N_BUNDLED, "Auto recovers from a removed import")
r = d.launch(IMPORTED, ok=False)
check(r["custom"] == "0" and d.phase() is None, "Imported with a missing file: system driver, no marker")
d.close()

scenario("import through Android/media")
d = Device()
os.makedirs(os.path.join(d.media, "driver_import"))
with open(os.path.join(d.media, "driver_import", "turnip_media.so"), "wb") as f: f.write(b"M")
r = d.launch(AUTO)
check(r["driver"] == "turnip_media.so", f"media import selected: {r['driver']}")
check(os.path.isfile(os.path.join(d.media, "driver_import", "installed", "turnip_media.so")), "moved to installed/")
d.close()
d = Device(media=False)
check(d.launch(AUTO)["custom"] == "1", "no media dir: fine")
d.close()

def make_zip(path, entries):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(name, data)

scenario("import a driver package")
d = Device()
os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "turnip-v25.zip"), [
    ("meta.json", json.dumps({"schemaVersion": 1, "name": "Turnip", "libraryName": "vulkan.ad07XX.so"})),
    ("vulkan.ad07XX.so", b"T" * 5000),
    ("libdep.so", b"D"),
    ("docs/readme.md", b"hello"),
])
r = d.launch(AUTO)
check(r["driver"] == "vulkan.ad07XX.so", f"library from meta.json: {r['driver']}")
for name in ("vulkan.ad07XX.so", "libdep.so", "meta.json", "readme.md"):
    check(os.path.isfile(os.path.join(d.turnip, name)), f"{name} extracted flat")
check(os.path.getsize(os.path.join(d.turnip, "vulkan.ad07XX.so")) == 5000, "library contents")
check(os.path.isfile(os.path.join(d.imports, "installed", "turnip-v25.zip")), "zip moved to installed/")
check(d.launch(IMPORTED)["driver"] == "vulkan.ad07XX.so", "Imported loads it")
d.close()

d = Device(); os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "single.zip"), [("some/dir/libvulkan_x.so", b"X"), ("notes.txt", b"n")])
check(d.launch(AUTO)["driver"] == "libvulkan_x.so", "no meta.json: the single .so is used")
d.close()

d = Device(); os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "two.zip"), [("a.so", b"A"), ("b.so", b"B")])
r = d.launch(AUTO)
check(r["driver"] == N_BUNDLED, "two libraries and no meta.json: rejected, bundled stays")
check(not os.path.exists(os.path.join(d.turnip, "a.so")), "nothing extracted from a rejected package")
check(read(os.path.join(d.turnip, "driver_name.txt")) == N_BUNDLED, "selection unchanged")
check(not os.path.exists(os.path.join(d.imports, "two.zip")), "rejected package leaves the import folder")
check(os.path.isfile(os.path.join(d.imports, "rejected", "two.zip")), "and lands in rejected/")
check(not os.path.exists(os.path.join(d.imports, "installed", "two.zip")), "not in installed/")
r = d.launch(AUTO)
check("two.zip" not in r["log"], "a rejected package is not processed again")
d.close()

for bad in ("../evil.so", "/abs/evil.so", "sub/evil.so", "evil.txt", "", "..", "evil.so/"):
    d = Device(); os.makedirs(d.imports)
    make_zip(os.path.join(d.imports, "bad.zip"), [("meta.json", json.dumps({"libraryName": bad})), ("evil.so", b"E"), ("x.so", b"X")])
    r = d.launch(AUTO)
    check(r["driver"] == N_BUNDLED, f"unsafe libraryName {bad!r} rejected: {r['driver']}")
    d.close()

# Hostile entry paths cannot leave the driver directory.
d = Device(); os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "hostile.zip"), [
    ("meta.json", json.dumps({"libraryName": "ok.so"})), ("ok.so", b"O"),
    ("../../escape1.bin", b"1"), ("/tmp/drvtest_escape2.bin", b"2"), ("a/../../escape3.bin", b"3"),
])
r = d.launch(AUTO)
check(r["driver"] == "ok.so", "package with hostile paths still imports its library")
check(not os.path.exists(os.path.join(d.root, "escape1.bin")) and not os.path.exists(os.path.join(d.internal, "escape1.bin")), "no ../ escape")
check(not os.path.exists("/tmp/drvtest_escape2.bin"), "no absolute path escape")
check(os.path.isfile(os.path.join(d.turnip, "escape1.bin")) and os.path.isfile(os.path.join(d.turnip, "escape3.bin")), "flattened into the driver dir")
d.close()

for label, writer in (("not a zip", lambda p: open(p, "wb").write(b"this is not a zip archive")),
                      ("empty file", lambda p: open(p, "wb").close()),
                      ("bad json", lambda p: make_zip(p, [("meta.json", b"{not json"), ("a.so", b"A"), ("b.so", b"B")])),
                      ("json without name", lambda p: make_zip(p, [("meta.json", b"[1,2,3]"), ("a.so", b"A"), ("b.so", b"B")])),
                      ("wrong type", lambda p: make_zip(p, [("meta.json", json.dumps({"libraryName": 5}).encode()), ("a.so", b"A"), ("b.so", b"B")])),
                      ("empty zip", lambda p: make_zip(p, []))):
    d = Device(); os.makedirs(d.imports)
    writer(os.path.join(d.imports, "pkg.zip"))
    r = d.launch(AUTO)
    check(r["rc"] == 0 and r["driver"] == N_BUNDLED, f"{label}: rejected without crashing ({r['driver']})")
    check(os.path.isfile(os.path.join(d.imports, "rejected", "pkg.zip")), f"{label}: moved to rejected/")
    check(d.phase() is None, f"{label}: no marker left")
    d.close()

# Bad JSON but exactly one library: the single .so rule still applies.
d = Device(); os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "pkg.zip"), [("meta.json", b"{not json"), ("only.so", b"A")])
check(d.launch(AUTO)["driver"] == "only.so", "unreadable meta.json falls back to the single .so")
d.close()

scenario("import: extension case, several files, name clashes")
d = Device(); os.makedirs(d.imports)
with open(os.path.join(d.imports, "Turnip_R18.SO"), "wb") as f: f.write(b"UPPER")
r = d.launch(AUTO)
check(r["driver"] == "Turnip_R18.so", f".SO is installed under a .so name: {r['driver']}")
check(read(os.path.join(d.turnip, "Turnip_R18.so")) == "UPPER", "contents")
check(os.path.isfile(os.path.join(d.imports, "installed", "Turnip_R18.SO")), "source keeps its name in installed/")
d.close()
d = Device(); os.makedirs(d.imports)
make_zip(os.path.join(d.imports, "Package.ZIP"), [("lib_only.so", b"Z")])
check(d.launch(AUTO)["driver"] == "lib_only.so", ".ZIP is recognised")
d.close()
d = Device(); os.makedirs(d.imports)
for name in ("b_second.so", "a_first.so", "c_third.so"):
    with open(os.path.join(d.imports, name), "wb") as f: f.write(name.encode())
r = d.launch(AUTO)
check(r["driver"] == "c_third.so", f"several imports: the last in name order is selected ({r['driver']})")
check(sorted(os.listdir(os.path.join(d.imports, "installed"))) == ["a_first.so", "b_second.so", "c_third.so"], "all moved")
check(all(os.path.isfile(os.path.join(d.turnip, n)) for n in ("a_first.so", "b_second.so", "c_third.so")), "all installed")
# Importing a file with the same name again replaces the earlier one in both places.
with open(os.path.join(d.imports, "c_third.so"), "wb") as f: f.write(b"NEWER")
d.launch(AUTO)
check(read(os.path.join(d.turnip, "c_third.so")) == "NEWER", "re-import replaces the installed driver")
check(read(os.path.join(d.imports, "installed", "c_third.so")) == "NEWER", "and the kept copy")
# Sub-folders and other files in the import folder are left alone.
os.makedirs(os.path.join(d.imports, "folder.so"))
with open(os.path.join(d.imports, "driver.so.part"), "wb") as f: f.write(b"partial")
with open(os.path.join(d.imports, "empty.so"), "wb") as f: pass
r = d.launch(AUTO)
check(r["driver"] == "c_third.so", "directory named *.so, .part and an empty .so change nothing")
check(os.path.isdir(os.path.join(d.imports, "folder.so")) and os.path.isfile(os.path.join(d.imports, "driver.so.part")), "left in place")
check(os.path.isfile(os.path.join(d.imports, "rejected", "empty.so")), "empty .so rejected")
d.close()

scenario("APK without bundled drivers")
d = Device(assets=False)
r = d.launch(AUTO, ok=False)
check(r["custom"] == "0" and d.phase() is None, "Auto: system driver, no marker")
r = d.launch(BUNDLED, ok=False)
check(r["custom"] == "0" and d.phase() is None, "Bundled: system driver, no marker")
os.makedirs(d.imports, exist_ok=True)
with open(os.path.join(d.imports, "mine.so"), "wb") as f: f.write(b"M")
check(d.launch(AUTO)["driver"] == "mine.so", "an import still works")
d.close()

scenario("Imported selected, nothing imported")
d = Device()
r = d.launch(IMPORTED, ok=False)
check(r["custom"] == "0" and d.phase() is None, "system driver, no marker")
d.close()

scenario("adrenotools fails to load the driver")
d = Device()
d.extra_env["TEST_ADRENOTOOLS_FAILS"] = "1"
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "0" and r["driver"] == N_BUNDLED, "falls back to the system driver in the same launch")
check(d.phase() is None, "and the marker is cleared when that works")
d.close()

scenario("missing directories")
d = Device()
shutil.rmtree(d.lib); d.lib = ""
r = d.launch(BUNDLED, ok=False)
check(r["custom"] == "0" and d.phase() is None, "no native library dir: system driver")
d.close()
d = Device()
d.internal_backup = d.internal; d.internal = ""
r = d.launch(BUNDLED, ok=False)
check(r["rc"] == 0 and r["custom"] == "0", "no internal dir: system driver, no crash")
d.internal = d.internal_backup
d.close()

# ---------------------------------------------------------------------------------------
# The bundled drivers are compared with the APK only when one of the two may have changed.
ALL_BUNDLED = (N_BUNDLED, N_VAUZI, N_A725, N_A8XX)

def stamped(stamp="1700000000000-1000000-38000000", **kwargs):
    device = Device(**kwargs)
    device.extra_env["SHADOWRECOMP_APK_STAMP"] = stamp
    return device

def stamp_path(device):
    return os.path.join(device.turnip, "bundled_drivers.stamp")

def installed(device, name):
    with open(os.path.join(device.turnip, name), "rb") as f:
        return f.read()

scenario("bundled drivers: compared once per APK")
d = stamped()
r = d.launch(AUTO)
check(r["asset_opens"] == "4", f"first launch reads the four assets: {r['asset_opens']}")
check(os.path.isfile(stamp_path(d)), "stamp written")
lines = read(stamp_path(d)).splitlines()
check(lines[0] == "apk=1700000000000-1000000-38000000", f"stamp names the APK: {lines[0]}")
check([line.split("=")[0] for line in lines[1:]] == list(ALL_BUNDLED), "one line per bundled driver")
check(all(line.split("=")[1].split(":")[0] == str(len("ELF:" + line.split("=")[0])) for line in lines[1:]), "with the size of each")
r = d.launch(AUTO)
check(r["asset_opens"] == "0", f"second launch does not read the APK: {r['asset_opens']}")
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, "and loads the same driver")
check(r["log"].count("fingerprint") == 4 and "earlier launch" in r["log"], "the fingerprints are still in the log")
check(d.launch(SYSTEM)["asset_opens"] == "0" and d.launch(VAUZI)["asset_opens"] == "0", "whatever driver is selected")
check(not [f for f in os.listdir(d.turnip) if f.endswith(".tmp")], "no temp files left")
# What the skip rests on: with an unchanged stamp the assets are not looked at at all.
d.put_assets(b"-unseen")
r = d.launch(AUTO)
check(r["asset_opens"] == "0" and installed(d, N_BUNDLED) == b"ELF:" + N_BUNDLED.encode(), "same stamp: assets not consulted")
# An update of the APK changes the stamp.
d.put_assets(b"-v2")
d.extra_env["SHADOWRECOMP_APK_STAMP"] = "1800000000000-1000001-38000001"
r = d.launch(AUTO)
check(r["asset_opens"] == "4", "new APK: compared again")
check(all(installed(d, n) == b"ELF:" + n.encode() + b"-v2" for n in ALL_BUNDLED), "and every copy replaced")
check(read(stamp_path(d)).startswith("apk=1800000000000-1000001-38000001\n"), "stamp follows")
check(d.launch(AUTO)["asset_opens"] == "0", "then skipped again")
d.close()

scenario("bundled drivers: a copy that changed is restored")
d = stamped()
d.launch(AUTO)
os.remove(os.path.join(d.turnip, N_VAUZI))
r = d.launch(AUTO)
check(r["asset_opens"] == "4" and installed(d, N_VAUZI) == b"ELF:" + N_VAUZI.encode(), "a deleted copy comes back")
check(d.launch(AUTO)["asset_opens"] == "0", "and the stamp is valid again")
with open(os.path.join(d.turnip, N_A725), "ab") as f: f.write(b"junk")
r = d.launch(AUTO)
check(r["asset_opens"] == "4" and installed(d, N_A725) == b"ELF:" + N_A725.encode(), "a copy of another size is replaced")
os.remove(os.path.join(d.turnip, N_A8XX)); os.makedirs(os.path.join(d.turnip, N_A8XX))
r = d.launch(SYSTEM)
check(r["rc"] == 0 and r["asset_opens"] == "4", "a directory in place of a copy is not taken for one")
check(not os.path.exists(stamp_path(d)), "and no stamp vouches for it")
d.close()

scenario("bundled drivers: damaged stamp")
d = stamped()
d.launch(AUTO)
good = read(stamp_path(d))
for label, text in (("empty", ""), ("garbage", "garbage"), ("only the APK line", good.splitlines()[0] + "\n"),
                    ("another APK", good.replace("apk=1700000000000", "apk=1600000000000")),
                    ("a driver missing", "\n".join(good.splitlines()[:-1]) + "\n"),
                    ("drivers reordered", "\n".join([good.splitlines()[0]] + good.splitlines()[:0:-1]) + "\n"),
                    ("trailing text", good + "extra\n"), ("no final newline", good[:-1]),
                    ("wrong size", good.replace("=%d:" % len("ELF:" + N_VAUZI), "=1:")),
                    ("bad number", good.replace("=%d:" % len("ELF:" + N_VAUZI), "=x:")),
                    ("spaces", good.replace("=", " = ")), ("huge", good + "x" * 5000)):
    with open(stamp_path(d), "w") as f: f.write(text)
    r = d.launch(AUTO)
    check(r["rc"] == 0 and r["asset_opens"] == "4", f"{label}: compared again ({r['asset_opens']})")
    check(read(stamp_path(d)) == good, f"{label}: stamp rewritten")
d.close()

scenario("bundled drivers: imports force a comparison")
d = stamped()
d.launch(AUTO)
# Same name and size as a bundled driver, other bytes: the size check alone would miss it.
with open(os.path.join(d.imports, N_BUNDLED), "wb") as f: f.write(b"X" * len("ELF:" + N_BUNDLED))
r = d.launch(AUTO)
check(r["asset_opens"] == "4", "an import is followed by a comparison")
check(installed(d, N_BUNDLED) == b"ELF:" + N_BUNDLED.encode(), "the bundled slot follows the APK")
check(d.launch(AUTO)["asset_opens"] == "0", "one launch later it is skipped again")
with open(os.path.join(d.imports, "broken.zip"), "wb") as f: f.write(b"not a zip")
check(d.launch(AUTO)["asset_opens"] == "4", "a rejected import too")
d.close()

scenario("bundled drivers: no stamp without a usable APK stamp")
d = Device()
check(d.launch(AUTO)["asset_opens"] == "4" and d.launch(AUTO)["asset_opens"] == "4", "no APK stamp: compared on every launch")
check(not os.path.exists(stamp_path(d)), "and no stamp file")
for bad in ("", "two\nlines", "cr\rhere", "x" * 300):
    d.extra_env["SHADOWRECOMP_APK_STAMP"] = bad
    check(d.launch(AUTO)["asset_opens"] == "4" and not os.path.exists(stamp_path(d)), f"unusable APK stamp {bad[:10]!r} ignored")
# The stamp appears with an update of the app: compared once more, then skipped.
d.extra_env["SHADOWRECOMP_APK_STAMP"] = "5"
check(d.launch(AUTO)["asset_opens"] == "4" and d.launch(AUTO)["asset_opens"] == "0", "skipping starts once there is one")
# A stamp left by a launch that had one is not trusted by a launch that has none.
del d.extra_env["SHADOWRECOMP_APK_STAMP"]
check(d.launch(AUTO)["asset_opens"] == "4" and not os.path.exists(stamp_path(d)), "and stops without")
d.close()

scenario("bundled drivers: APK without them")
d = stamped(assets=False)
r = d.launch(AUTO, ok=False)
check(r["custom"] == "0" and r["asset_opens"] == "4", "looked for once")
check(read(stamp_path(d)).splitlines()[1:] == [n + "=-" for n in ALL_BUNDLED], "recorded as not shipped")
check(d.launch(AUTO, ok=False)["asset_opens"] == "0", "not looked for again")
# A copy left by an earlier APK that did ship drivers is neither required nor removed.
with open(os.path.join(d.turnip, N_BUNDLED), "wb") as f: f.write(b"old")
r = d.launch(BUNDLED)
check(r["asset_opens"] == "0" and r["driver"] == N_BUNDLED, "a leftover copy stays usable")
d.close()

scenario("bundled drivers: unreadable asset, unwritable stamp")
d = stamped()
d.extra_env["TEST_ASSET_READ_FAILS"] = "1"
r = d.launch(AUTO, ok=False)
check(r["rc"] == 0 and r["custom"] == "0", "unreadable assets: system driver, no crash")
check(not os.path.exists(stamp_path(d)), "nothing is recorded as compared")
check("Failed to read the bundled Vulkan driver" in r["log"], "and the log says why")
del d.extra_env["TEST_ASSET_READ_FAILS"]
r = d.launch(AUTO)
check(r["asset_opens"] == "4" and r["driver"] == N_BUNDLED and os.path.isfile(stamp_path(d)), "next launch provisions them")
os.remove(stamp_path(d)); os.makedirs(stamp_path(d) + ".tmp")
r = d.launch(AUTO)
check(r["rc"] == 0 and r["driver"] == N_BUNDLED and not os.path.exists(stamp_path(d)), "stamp cannot be written: the launch is unaffected")
check(d.launch(AUTO)["asset_opens"] == "4", "and the comparison simply runs again")
d.close()

scenario("bundled drivers: a failed startup has the copies compared again")
d = stamped()
d.launch(BUNDLED)
# Damage the copy without changing its size, as a bad flash block would.
with open(os.path.join(d.turnip, N_BUNDLED), "r+b") as f: f.write(b"BAD!")
r = d.launch(BUNDLED, ok=False)
check(r["asset_opens"] == "0" and os.path.isfile(stamp_path(d)), "the damage goes unnoticed while the stamp holds")
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "0" and r["asset_opens"] == "0", "recovery launch: system driver, drivers left alone")
check(not os.path.exists(stamp_path(d)), "but the stamp is gone")
r = d.launch(BUNDLED, ok=True)
check(r["custom"] == "1" and r["asset_opens"] == "4", "the launch after compares")
check(installed(d, N_BUNDLED) == b"ELF:" + N_BUNDLED.encode(), "and repairs the copy")
d.close()

# ---------------------------------------------------------------------------
# Render health. A driver that refuses to build the game's pipelines renders
# garbage or nothing (system drivers on several Adreno devices in the field).
# The next launch then runs the other driver path automatically, without the
# configuration being rewritten; a proven override replaces the report, and a
# healthy run of the configured selection clears it.

def recovery_path(device):
    return os.path.join(device.internal, "vulkan_render_recovery.txt")

scenario("render recovery: a broken system driver is replaced by the bundled one")
d = stamped()
d.launch(AUTO)  # baseline: bundled custom driver is what the config gives
r = d.launch(SYSTEM, health="broken:376")
check(r["custom"] == "0" and r["driver"] == "-", "the broken launch itself ran the system driver")
marker = read(recovery_path(d))
check("broken_kind=system" in marker and "broken_refused=376" in marker and "broken_driver=-" in marker,
      f"the failure is recorded:\n{marker}")
r = d.launch(SYSTEM)  # the next launch is forced onto the custom driver
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, f"forced onto the bundled driver: {r['driver']}")
check("override=custom" in read(recovery_path(d)) and "override_pending=1" in read(recovery_path(d)),
      "that attempt is tracked")
r = d.launch(SYSTEM)  # an unproven attempt counts as failed: the override stops
check(r["custom"] == "0" and r["driver"] == "-", "both broken: the configured driver runs again")
check("both_broken=1" in read(recovery_path(d)) and "override=-" in read(recovery_path(d)),
      "the report says both failed and forces nothing")
check(d.launch(SYSTEM)["custom"] == "0", "it stays that way")
d.close()

scenario("render recovery: a proven override stays in use")
d = stamped()
d.launch(AUTO)
d.launch(SYSTEM, health="broken:100")
r = d.launch(SYSTEM, health="sustained")  # the forced custom run renders fine for a while
check(r["custom"] == "1", "the override runs")
marker = read(recovery_path(d))
check("override=custom" in marker and "override_pending=0" in marker and "both_broken=0" in marker
      and "broken_kind=system" in marker and "broken_refused=100" in marker, f"proven:\n{marker}")
r = d.launch(SYSTEM, health="sustained")  # and stays in use
check(r["custom"] == "1", "still on the override")
os.remove(recovery_path(d))  # the launcher notice can be dismissed by deleting the file
r = d.launch(SYSTEM)
check(r["custom"] == "0" and not os.path.exists(recovery_path(d)), "dismissed: the setting decides again")
d.close()

scenario("render recovery: a proven override survives a short run")
d = stamped()
d.launch(AUTO)
d.launch(SYSTEM, health="broken:100")
d.launch(SYSTEM, health="sustained")  # the override is proven
check("override_proven=1" in read(recovery_path(d)), "the proof is recorded")
r = d.launch(SYSTEM)  # closed before the renderer could report: no health note
check(r["custom"] == "1", "the override runs")
check("override_pending=1" in read(recovery_path(d)), "tracked as unreported")
r = d.launch(SYSTEM)  # the next boot finds it unreported, but proven from earlier
check(r["custom"] == "1", "the override is kept")
marker = read(recovery_path(d))
check("both_broken=0" in marker and "override=custom" in marker and "override_proven=1" in marker,
      f"not a failure (the pending bit is this run's own tracker):\n{marker}")
d.close()

scenario("render recovery: a driver the renderer cannot be created on")
d = stamped()
d.launch(AUTO)
r = d.launch(SYSTEM, health="failed:device")  # the old-Mali failure: "backend allocation failure"
check(r["custom"] == "0", "the failed launch ran the system driver")
marker = read(recovery_path(d))
check("broken_kind=system" in marker and "broken_reason=device" in marker and "broken_refused=0" in marker,
      f"the failure is recorded with its reason:\n{marker}")
r = d.launch(SYSTEM, health="sustained")  # the next launch is forced onto the bundled driver
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, f"forced onto the bundled driver: {r['driver']}")
check("override_pending=0" in read(recovery_path(d)) and "override_proven=1" in read(recovery_path(d)),
      "proven by the sustained run")
d.close()

scenario("render recovery: a driver without the features, and no alternative")
d = Device(props=MALI, assets=False)
r = d.launch(AUTO, health="failed:requirements")
check(r["custom"] == "0", "Mali runs the system driver")
marker = read(recovery_path(d))
check("broken_kind=system" in marker and "broken_reason=requirements" in marker and "override=-" in marker,
      f"recorded, nothing to offer:\n{marker}")
d.close()

scenario("render recovery: a black screen on the override means both failed")
d = stamped()
d.launch(AUTO)
d.launch(SYSTEM, health="broken:60")
r = d.launch(SYSTEM, health="failed:blackscreen")  # the forced bundled driver never shows a frame
check(r["custom"] == "1", "the override ran")
marker = read(recovery_path(d))
check("both_broken=1" in marker and "override=-" in marker and "broken_reason=blackscreen" in marker,
      f"both directions failed:\n{marker}")
d.close()

scenario("render recovery: a healthy configured selection clears the report")
d = stamped()
d.launch(SYSTEM, health="broken:100")
check("broken_kind=system" in read(recovery_path(d)), "the failure is recorded")
r = d.launch(AUTO, health="healthy")  # the selection changed; the bundled driver is healthy
check(r["custom"] == "1", "the new selection runs")
check(not os.path.exists(recovery_path(d)), "the report for the old selection is retired")
d.close()

scenario("render recovery: a broken custom driver falls back to the system one")
d = stamped()
r = d.launch(AUTO, health="broken:212")
check(r["custom"] == "1", "the broken launch ran the bundled driver")
marker = read(recovery_path(d))
check("broken_kind=custom" in marker and "broken_driver=" + N_BUNDLED in marker,
      f"the report names the bundled driver:\n{marker}")
r = d.launch(AUTO, health="sustained")  # forced onto the system driver; it renders fine
check(r["custom"] == "0" and r["driver"] == "-", "forced onto the system driver")
marker = read(recovery_path(d))
check("override=system" in marker and "override_pending=0" in marker and "both_broken=0" in marker,
      "the override is proven and stays")
d.close()

scenario("render recovery: a broken override run means both directions failed")
d = stamped()
d.launch(SYSTEM, health="broken:50")
r = d.launch(SYSTEM, health="broken:50")  # the forced custom run renders badly too
check(r["custom"] == "1", "the override ran")
marker = read(recovery_path(d))
check("both_broken=1" in marker and "override=-" in marker and "override_pending=0" in marker,
      f"both directions failed:\n{marker}")
r = d.launch(SYSTEM)
check(r["custom"] == "0", "nothing is forced any more")
d.close()

scenario("render recovery: nothing imported or bundled means no override")
d = Device(props=MALI, assets=False)  # a GPU family without bundled drivers, and no APK assets
r = d.launch(AUTO, health="broken:90")
check(r["custom"] == "0" and r["driver"] == "-", "Mali runs the system driver")
marker = read(recovery_path(d))
check("broken_kind=system" in marker and "override=-" in marker, f"reported, but nothing to offer:\n{marker}")
r = d.launch(AUTO)
check(r["custom"] == "0" and "override=-" in read(recovery_path(d)), "the next launch is unchanged")
d.close()

scenario("render recovery: an imported driver is offered as the alternative on Mali")
d = Device(props=MALI)
os.makedirs(d.imports)
with open(os.path.join(d.imports, "vulkan.panvk_test.so"), "wb") as f: f.write(b"PANVK")
r = d.launch(SYSTEM, health="broken:77")
check("broken_kind=system" in read(recovery_path(d)), "the system driver failed")
r = d.launch(SYSTEM)  # the next launch imports the driver and is forced onto it
check(r["custom"] == "1" and r["driver"] == "vulkan.panvk_test.so", f"and used: {r['driver']}")
check("override=custom" in read(recovery_path(d)) and "override_pending=1" in read(recovery_path(d)), "tracked")
d.close()

scenario("render recovery: a crash of the driver routes the next launch to the other one")
# The game's crash handler (logger_android.cpp) counts a fatal signal against the running
# driver by calling os::android::NoteCrash() before the process is re-raised into the
# system tombstone; the harness calls the same hook. A crash must do as much damage to a
# selection as a broken renderer: one launch later the other driver path runs.
d = stamped()
d.launch(AUTO)
r = d.launch(SYSTEM, health="crash")  # the process would die here on a device
check(r["rc"] == 0, "the harness survives; on the device this launch is a crash")
marker = read(recovery_path(d))
check("broken_kind=system" in marker and "broken_reason=device" in marker,
      f"the crash is recorded against the system driver:\n{marker}")
r = d.launch(SYSTEM, health="sustained")
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, f"the next launch is forced onto the bundled driver: {r['driver']}")
check("override_proven=1" in read(recovery_path(d)), "and a sustained run proves it")
d.close()

scenario("render recovery: a crash of the bundled driver falls back to the system one")
d = stamped()
r = d.launch(AUTO, health="crash")
check(r["custom"] == "1", "the crashing launch ran the bundled driver")
marker = read(recovery_path(d))
check("broken_kind=custom" in marker and "broken_driver=" + N_BUNDLED in marker,
      f"the crash is recorded against the bundled driver:\n{marker}")
r = d.launch(AUTO, health="sustained")
check(r["custom"] == "0" and r["driver"] == "-", "forced onto the system driver")
check("override=system" in marker or "override_proven=1" in read(recovery_path(d)), "the override stays")
d.close()

scenario("render recovery: an invalid record is removed")
d = stamped()
d.launch(AUTO)
with open(recovery_path(d), "w") as f:
    f.write("version=99\noverride=custom\n")
r = d.launch(AUTO)
check(r["custom"] == "1" and r["driver"] == N_BUNDLED, "the configured driver runs")
check(not os.path.exists(recovery_path(d)), "the invalid record is gone")
# A record naming a driver name that cannot be safe has the name dropped, not trusted.
with open(recovery_path(d), "w") as f:
    f.write("version=1\nconfigured_driver=0\nconfigured_render_mode=0\n"
            "broken_kind=custom\nbroken_refused=40\nbroken_driver=../evil.so\noverride=system\noverride_pending=0\nboth_broken=0\n")
r = d.launch(AUTO)
check(r["custom"] == "0" and r["driver"] == "-", "the override forces the system driver")
check("evil" not in read(recovery_path(d)), "the tampered name did not survive")
d.close()

scenario("readme is written only when it differs")
d = Device()
d.launch(AUTO)
readme = os.path.join(d.imports, "readme.txt")
media_readme = os.path.join(d.media, "driver_import", "readme.txt")
before = (os.stat(readme).st_ino, os.stat(media_readme).st_ino)
d.launch(AUTO)
check((os.stat(readme).st_ino, os.stat(media_readme).st_ino) == before, "unchanged readme files are left alone")
text = read(readme)
with open(readme, "w") as f: f.write(text + "edited")
with open(media_readme, "w") as f: f.write("")
d.launch(AUTO)
check(read(readme) == text and read(media_readme) != "", "changed and emptied ones are written again")
d.close()

scenario("files that are not what their name says")
d = stamped()
d.launch(AUTO)
# A directory where a file belongs. What seeking to the end of one reports depends on the
# file system (ext4: 2^63 - 1), and must never become the size of a buffer.
os.remove(stamp_path(d)); os.makedirs(stamp_path(d))
r = d.launch(AUTO)
check(r["rc"] == 0 and r["asset_opens"] == "4" and r["driver"] == N_BUNDLED, "a directory in place of the stamp: compared, launch unaffected")
check(d.launch(AUTO)["asset_opens"] == "4", "and compared again on every launch while it is there")
os.rmdir(stamp_path(d))
check(d.launch(AUTO)["asset_opens"] == "4" and d.launch(AUTO)["asset_opens"] == "0", "once it is gone: one more comparison, then none")
readme = os.path.join(d.imports, "readme.txt")
os.remove(readme); os.makedirs(readme)
r = d.launch(AUTO)
check(r["rc"] == 0 and r["driver"] == N_BUNDLED and os.path.isdir(readme), "a directory in place of the readme: launch unaffected")
# A file far too large to be a driver is turned down by its size, not read to find out.
with open(os.path.join(d.imports, "vulkan.huge.so"), "wb") as f: f.truncate(300 * 1024 * 1024)
r = d.launch(AUTO)
check(r["rc"] == 0 and r["driver"] == N_BUNDLED, f"an oversized .so is not installed: {r['driver']}")
check(os.path.isfile(os.path.join(d.imports, "rejected", "vulkan.huge.so")), "it lands in rejected/")
check(not os.path.exists(os.path.join(d.turnip, "vulkan.huge.so")), "and not among the drivers")
d.close()

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
