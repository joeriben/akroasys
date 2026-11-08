#!/usr/bin/env python3
"""Does a built bundle carry a WORKING Csound of its own?

The whole point of bundling Csound is the machine that does NOT have Csound
installed, and on the machine that does, a broken bundle is indistinguishable
from a good one: the loader silently falls through to the system copy, the
library loads, the orchestra compiles, the sound plays. A green run here would
then certify nothing — which is exactly the trap this file exists to avoid.

    macOS    tools/verify_csound_bundle.py build_clean/T5ynth_artefacts/Release/Standalone/akroasys.app
    Windows  tools/verify_csound_bundle.py build\\T5ynth_artefacts\\Release\\Standalone
    Linux    tools/verify_csound_bundle.py build/T5ynth_artefacts/Release/Standalone

Every platform ends in the same place: load the SHIPPED library, prove the plugin
opcode modules came with it, then compile and PLAY a real LRO orchestra in the
host's own scaffold (backend/lco_write.wrap) and require non-silent samples.
They differ in how the fall-through is ruled out, because the loaders differ:

  macOS    the runtime half runs under `sandbox-exec` with every Homebrew Csound
           path denied, and FIRST proves the denial actually bites.
  Windows  there is no sandbox, and none is needed for the question at hand: the
           DLL is loaded by absolute path, `GetModuleFileNameW` is read back to
           prove the loaded module IS the shipped file, and the opcode directory is
           set explicitly. The payload's own dependencies are the OS plus the MSVC
           runtime (`VCRUNTIME140`, `MSVCP140`, the UCRT) — measured from the PE
           headers, and the same runtime our own MSVC build already requires, so
           this run does not certify anything about a machine without it.
  Linux    the build machine HAS Csound installed (that is where the bundle comes
           from), so the same trap applies as on macOS and there is no sandbox-exec
           to answer it with. Instead the bundled library is loaded by absolute path
           and /proc/self/maps is read back to prove the mapping is the bundled file
           and not /usr/lib — which is the exact statement, not an approximation of
           it. The static half reads every bundled ELF's rpath and resolved
           dependencies and refuses anything outside the artefact.

What it does not do: launch the Standalone. That test is BJ's fourth requirement
and needs a person or a driven GUI; this file is the part that can run on every
build, and it covers everything between the shipped bytes and audible samples.
"""
import ctypes
import os
import struct
import subprocess
import sys
from pathlib import Path

WINDOWS = sys.platform == "win32"
LINUX = sys.platform.startswith("linux")

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "backend"))

# What the child may not read. Not a list of Csound paths — the WHOLE of both
# Homebrew prefixes, plus the system-wide framework location. A named-paths list
# only denies the ways I thought of; the bundle's dependency tree is libsndfile,
# ogg, vorbis, FLAC, opus, mpg123, lame and gettext as well, and any one of them
# resolving back to Homebrew would make a green run mean nothing. This is
# affordable because the child is /usr/bin/python3, which links nothing outside
# /usr/lib and /System (measured 2026-07-26).
DENIED_PREFIXES = (
    "/opt/homebrew",
    "/usr/local",
    "/Library/Frameworks/CsoundLib64.framework",
)

# The paths that must be gone for the test to mean anything, checked by name so
# the failure says which one survived. The Cellar matters most: the library's
# baked-in opcode path points straight into it.
HOMEBREW_CSOUND = (
    "/opt/homebrew/Cellar/csound",
    "/opt/homebrew/opt/csound",
    "/opt/homebrew/bin/csound",
    "/opt/homebrew/lib/libcsnd6.6.0.dylib",
    "/opt/homebrew/Frameworks/CsoundLib64.framework",
    "/opt/homebrew/lib/libsndfile.1.dylib",
    "/usr/local/opt/csound",
    "/Library/Frameworks/CsoundLib64.framework",
)

# A quarter second at 44.1 kHz, like the authoring gate's own performance check.
PERF_SECONDS = 0.25
SILENCE_FLOOR = 1e-4      # a peak below this is silence, not a quiet sound


def fail(msg):
    print(f"FAIL  {msg}")
    sys.exit(1)


def ok(msg):
    print(f"ok    {msg}")


# ── static half: does the bundle name anything outside itself ────────────────

def mach_o_files(*roots):
    """Every Mach-O under the given paths. Not `find -perm -u+x`: dylibbundler
    writes its copies 0644, and that filter hid 9 of the 10 bundled libraries from
    this check. os.walk rather than parsing find's output, because a path with a
    space in it silently split into two nonexistent ones."""
    for root in roots:
        root = Path(root)
        paths = [root] if root.is_file() else \
                [Path(d) / f for d, _, fs in os.walk(root) for f in fs]
        for path in paths:
            if path.is_symlink() or not path.is_file():
                continue
            kind = subprocess.run(["file", "-b", str(path)],
                                  capture_output=True, text=True).stdout
            if "Mach-O" in kind:
                yield path


def foreign_references(path):
    """Everything the file names that is not inside the bundle (@loader_path,
    @rpath, @executable_path) and not the OS's own (/usr/lib, /System). A
    whitelist, deliberately: the old blacklist of /opt/homebrew, /usr/local and
    /Cellar/ passes anything a differently-configured build machine records —
    /Users/…, /sw, a Nix store path — which is the same defect wearing a
    different prefix.

    /System/Volumes is carved back out of the /System exemption: on APFS the data
    volume is also reachable as /System/Volumes/Data/opt/homebrew/…, so a whole
    Homebrew tree can be spelled as if it were the OS's own."""
    load = subprocess.run(["otool", "-L", str(path)],
                          capture_output=True, text=True).stdout
    refs = [_first_field(l) for l in load.splitlines()[1:] if l.startswith("\t")]
    refs += rpaths_of(path)
    return [r for r in refs
            if not r.startswith(("@", "/usr/lib/", "/System/"))
            or r.startswith("/System/Volumes/")]


def _first_field(line):
    """otool prints `\\t<path> (compatibility version …)`. Splitting on whitespace
    truncates a path that contains a space, which then gets reported under a name
    that does not exist."""
    return line.strip().rsplit(" (", 1)[0].strip()


def rpaths_of(path):
    out = subprocess.run(["otool", "-l", str(path)],
                         capture_output=True, text=True).stdout
    found, want_path = [], False
    for line in out.splitlines():
        if "cmd LC_RPATH" in line:
            want_path = True
        elif want_path and line.strip().startswith("path "):
            found.append(line.strip()[5:].rsplit(" (offset ", 1)[0].strip())
            want_path = False
    return found


# What third_party/csound/macos-arm64/lib/rawwaves holds, which is STK 5.0.1's
# own set. A floor, like MIN_OPCODE_ENTRIES: a later STK may add files, and that
# must not turn a good build red.
SEEN_RAWWAVES = 41
VENDORED_RAWWAVES = REPO / "third_party" / "csound" / "macos-arm64" / "lib" / "rawwaves"


def check_rawwaves(rawwaves, has_stkops):
    """STK's data directory and the module that reads it travel together.

    Three states, and only the middle one is a build failure:

      module absent, data absent   a build made without the payload and without an
                                   STK install. No STK opcodes, said so, fine.
      one without the other        the failure this exists to catch. A bundle that
                                   carries libstkops and no data has lost 27
                                   opcodes with nothing else going red — the same
                                   shape as the measured case in MIN_OPCODE_ENTRIES
                                   where 22 deleted modules still passed at 1929
                                   entries. tools/bundle_csound_macos.sh keeps the
                                   pair together, so a split here is a real defect.
      both present                 the data must also be COMPLETE. With
                                   RAWWAVE_PATH unset the module declines and says
                                   why, which is a capability loss and nothing
                                   worse; with it set at a directory missing one
                                   file the module wants, stk::StkError leaves the
                                   module as an uncaught C++ exception and
                                   abort()s the process — measured 2026-08-06 with
                                   the directory emptied, and inside a plugin that
                                   process is the host DAW.

    Complete means BY NAME and by size against the payload in the checkout, not by
    count: which file an instrument reaches for is STK's business, so 41 files
    under the wrong names would pass a count and abort at the user's first note.
    Where the checkout has no payload to compare against — a build from a system
    framework on a tree with third_party/csound deleted — the count is the only
    statement left, and it is still enough to catch a half-copied directory."""
    if not rawwaves.is_dir():
        if has_stkops:
            fail("libstkops.dylib is bundled but Contents/libs/rawwaves is not — "
                 "the module reads RAWWAVE_PATH as it loads and will decline to "
                 "register, silently costing all 27 STK opcodes")
        ok("no STK opcodes in this bundle, and no data for them either — "
           "consistent (the LRO's other 2267 opcode entries are unaffected)")
        return False
    if not has_stkops:
        fail(f"{rawwaves} is bundled but libstkops.dylib is not — nothing in the "
             "bundle can read it")
    files = {p.name: p.stat().st_size for p in rawwaves.glob("*.raw")}
    # The absolute count FIRST and unconditionally. Putting it in the fallback
    # branch alone made it dead code wherever the payload exists — and the payload
    # is not itself checked for completeness, so emptying BOTH directories (the
    # obvious way to reproduce the abort this docstring cites) matched perfectly at
    # zero files and printed PASS on the one bundle state that kills the host.
    if len(files) < SEEN_RAWWAVES:
        fail(f"{len(files)} of {SEEN_RAWWAVES} STK rawwave files in {rawwaves} — "
             "an STK opcode that asks for a missing one does not fail to compile, "
             "it abort()s the process")
    if VENDORED_RAWWAVES.is_dir():
        wanted = {p.name: p.stat().st_size for p in VENDORED_RAWWAVES.glob("*.raw")}
        missing = sorted(set(wanted) - set(files))
        wrong = sorted(n for n in set(wanted) & set(files)
                       if wanted[n] != files[n])
        if missing or wrong:
            fail(f"{rawwaves} does not match {VENDORED_RAWWAVES}: "
                 + (f"missing {', '.join(missing)}. " if missing else "")
                 + (f"wrong size: {', '.join(wrong)}. " if wrong else "")
                 + "An STK opcode that asks for a file that is not there does not "
                   "fail to compile, it abort()s the process")
        ok(f"{len(files)} STK rawwave data files, each matching the payload in "
           f"the checkout by name and size")
    else:
        ok(f"{len(files)} STK rawwave data files (counted only — this checkout has "
           f"no {VENDORED_RAWWAVES.name} payload to compare names against)")
    return True


def check_bundle_contents(bundle):
    lib = bundle / "Contents" / "libs" / "CsoundLib64"
    opcodes = bundle / "Contents" / "libs" / "Opcodes64"
    licences = bundle / "Contents" / "Resources" / "licenses" / "csound"

    if not lib.is_file() or lib.is_symlink():
        fail(f"no bundled Csound library at {lib}"
             + (" (it is a symlink)" if lib.is_symlink() else ""))
    if not opcodes.is_dir() or not any(opcodes.glob("*.dylib")):
        fail(f"no bundled plugin opcodes at {opcodes}")
    # A symlink anywhere under Contents/libs is invisible to every check below:
    # os.walk does not descend a symlinked directory and this file skips symlinked
    # files, so a Contents/libs that LOOKS complete can be links to files that
    # exist on this machine only. Measured: with Opcodes64 a symlink, the sweep
    # dropped from 35 Mach-O files to 11 and still said PASS.
    links = sorted({p for p in [opcodes] + [Path(d) / n
                                            for d, ds, fs in os.walk(bundle / "Contents" / "libs")
                                            for n in ds + fs]
                    if p.is_symlink()})
    if links:
        fail("symlink(s) under Contents/libs, which every check below walks past: "
             + ", ".join(str(p.relative_to(bundle)) for p in links))
    if not (opcodes / "libscansyn.dylib").is_file():
        fail("libscansyn.dylib missing — scanu/scanu2/scans would be gone")
    required = ["NOTICE.txt", "LGPL-2.1.txt"]
    if check_rawwaves(bundle / "Contents" / "libs" / "rawwaves",
                      (opcodes / "libstkops.dylib").is_file()):
        required.append("STK.txt")
    for name in required:
        if not (licences / name).is_file():
            fail(f"{name} must ship in the bundle ({licences})")
    ok(f"{lib.name} + {len(list(opcodes.glob('*.dylib')))} plugin opcode modules "
       f"+ licence texts present")

    # Scope: what the bundling step is responsible for — Contents/libs and the
    # bundle's own binary. Not the whole bundle: a release .app also carries the
    # PyInstaller backend, hundreds of Mach-Os that are another concern's
    # business, and a check that pulls them in would grow exceptions until it
    # stops meaning anything.
    checked = 0
    problems = []
    for m in mach_o_files(bundle / "Contents" / "libs", bundle / "Contents" / "MacOS"):
        checked += 1
        rel = m.relative_to(bundle)
        # `codesign --verify --deep --strict` on the bundle is NOT enough, and
        # this is measured: it returns 0 with an unsigned dylib in Contents/libs,
        # because that directory is sealed as resources — bytes checked, nested
        # signatures not. An install_name_tool'd library whose signature was not
        # renewed does not merely fail to load, it KILLS the process.
        #
        # The bundle's own executable is the exception: codesign --verify on it
        # verifies the whole bundle seal, so anything added to Contents/Resources
        # after signing — which is exactly what CI does with the PyInstaller
        # backend (.github/workflows/build.yml) — reports here as "the executable
        # has a broken signature". Run this AFTER the bundle is re-sealed; the
        # message says so rather than sending someone after the wrong file.
        if subprocess.run(["codesign", "--verify", str(m)],
                          capture_output=True).returncode != 0:
            problems.append(
                f"{rel}: unsigned or broken signature"
                + (" — note that verifying the bundle's executable verifies the "
                   "whole bundle seal, so this also fires when something was "
                   "copied into the bundle after signing; re-sign, then re-run"
                   if m.parent.name == "MacOS" else ""))
        for ref in foreign_references(m):
            problems.append(f"{rel}: {ref}")
        # dyld aborts at launch on a duplicate LC_RPATH — before main(), with a
        # crash report that names no file this sweep would otherwise flag.
        seen = rpaths_of(m)
        for dup in {r for r in seen if seen.count(r) > 1}:
            problems.append(f"{rel}: LC_RPATH {dup} appears {seen.count(dup)} "
                            "times — dyld kills the process at launch")
    if problems:
        fail("bundle is not self-contained:\n      " + "\n      ".join(problems))
    ok(f"{checked} Mach-O files: each signed, no duplicate rpath, none naming "
       f"anything outside the bundle but /usr/lib and /System")
    return lib, opcodes


# ── static half, Windows ────────────────────────────────────────────────────

def _pe_imports(path):
    """(normal imports, delay-loaded imports) of a PE file, as lower-case DLL
    names. Written out rather than shelled to dumpbin: dumpbin needs a Visual
    Studio environment that a plain CI step does not have, and this is the one
    property that decides whether the VST3 loads in a host at all."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fail(f"{path.name} is not a PE file")
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    is_pe32_plus = magic == 0x20B
    dirs = opt + (112 if is_pe32_plus else 96)
    n_dirs = struct.unpack_from("<I", data, opt + (108 if is_pe32_plus else 92))[0]
    n_sections, size_opt = (struct.unpack_from("<H", data, pe + 6)[0],
                            struct.unpack_from("<H", data, pe + 20)[0])
    sections = []
    for i in range(n_sections):
        s = pe + 24 + size_opt + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        # rawsize, NOT max(vsize, rawsize): a section whose VirtualSize exceeds its
        # raw data has an uninitialised tail with no bytes in the file, and mapping
        # an RVA there would silently read the NEXT section's raw bytes instead.
        sections.append((vaddr, min(vsize, rawsize) or rawsize, rawptr))

    def at(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        return None

    def name_at(rva):
        off = at(rva)
        if off is None:
            return ""
        end = data.find(b"\0", off)
        if end < 0:                     # unterminated: a malformed file, not a name
            return ""
        return data[off:end].decode("ascii", "replace").lower()

    def walk(dir_index, entry_size, name_field):
        # A PE may declare fewer than 16 data directories; indexing past the count
        # would read the section table and call it an import table.
        if dir_index >= n_dirs:
            return []
        rva, size = struct.unpack_from("<II", data, dirs + dir_index * 8)
        if not rva or not size:
            return []
        off, out = at(rva), []
        if off is None:
            return []
        while True:
            entry = data[off:off + entry_size]
            if len(entry) < entry_size or not any(entry):
                return out
            field = struct.unpack_from("<I", entry, name_field)[0]
            if field:
                out.append(name_at(field))
            off += entry_size

    return walk(1, 20, 12), walk(13, 32, 4)   # IMAGE_IMPORT_DESCRIPTOR / ImgDelayDescr


# ── static half, Linux ──────────────────────────────────────────────────────

# The core runtime every ELF on the machine shares. tools/bundle_csound_linux.sh
# deliberately does NOT copy these — a bundled libc or libstdc++ is how a
# "self-contained" build becomes one that dies on a newer kernel or loader.
LINUX_SYSTEM_LIBS = (
    # every glibc library that can appear as a NEEDED entry (the same set
    # tools/bundle_csound_linux.sh never copies — a libcsound64 built with GCC 15
    # NEEDs libmvec.so.1 directly) ...
    "libc.so", "libm.so", "libmvec.so", "libdl.so", "libpthread.so", "librt.so",
    "libresolv.so", "libutil.so", "libanl.so", "libnss_", "libBrokenLocale.so",
    "libthread_db.so", "ld-linux",
    # ... and the GCC runtime.
    "libgcc_s.so", "libstdc++.so", "libatomic.so", "linux-vdso.so",
)


def _is_system_lib(name):
    return any(name.startswith(p) for p in LINUX_SYSTEM_LIBS)


def _resolved_deps(path):
    """What the loader actually binds this file to, as absolute paths. `ldd`
    rather than `readelf -d`, because DT_NEEDED gives SONAMEs and the question
    here is which FILE each one resolves to on this machine."""
    out = subprocess.run(["ldd", str(path)], capture_output=True, text=True).stdout
    deps, missing = [], []
    for line in out.splitlines():
        if "not found" in line:
            missing.append(line.strip().split(" ", 1)[0])
        elif "=>" in line:
            target = line.split("=>", 1)[1].strip().rsplit(" (", 1)[0].strip()
            if target.startswith("/"):
                deps.append(target)
    return deps, missing


def check_linux_payload(root):
    """`root` is the directory the binary lives in — the Standalone directory, or
    Contents/x86_64-linux inside the VST3."""
    libs = root / "libs"
    opcodes = libs / "Opcodes64"
    licences = root / "licenses" / "csound"

    shipped = sorted(libs.glob("libcsound64.so*")) if libs.is_dir() else []
    if not shipped:
        fail(f"no bundled Csound library under {libs}")
    lib = shipped[0]
    if not opcodes.is_dir() or not any(opcodes.glob("*.so")):
        fail(f"no bundled plugin opcodes at {opcodes}")
    for name in ("NOTICE.txt", "LGPL-2.1.txt"):
        if not (licences / name).is_file():
            fail(f"LGPL 2.1 requires {name} beside the library ({licences})")
    ok(f"{lib.name} + {len(list(opcodes.glob('*.so')))} plugin opcode modules "
       f"+ licence texts present")

    binaries = [p for p in root.iterdir()
                if p.is_file() and os.access(p, os.X_OK) and p.suffix in ("", ".so")]
    if not binaries:
        fail(f"no akróasys binary in {root} — nothing to check, and a green result "
             "here would be meaningless")

    # The scope is per file, and the difference is not cosmetic. For the files the
    # bundler COPIED, "outside the artefact" is always a failure: it copied every
    # non-system dependency recursively, so one still pointing at /usr/lib is a copy
    # that did not happen or an rpath that did not take. The BINARY is not one of
    # those files — it is the application, and it legitimately links the
    # distribution's ALSA, fontconfig, freetype and curl, none of which is bundled or
    # should be. Holding it to the payload's rule failed CI on all 36 of them, not
    # one of them Csound's. What it owes is narrower, and is asserted positively
    # below: the Csound library, and anything else the payload put in libs/, must
    # come from libs/. Same rule, same reason, as tools/bundle_csound_linux.sh — and
    # it has to be stated in both places, because either one alone fails the build.
    payload = {p.name for p in libs.glob("*.so*")}

    problems = []
    for f in binaries + sorted(libs.glob("*.so*")) + sorted(opcodes.glob("*.so")):
        rel = f.relative_to(root)
        is_app = f in binaries
        deps, missing = _resolved_deps(f)
        for name in missing:
            problems.append(f"{rel}: {name} not found")
        for dep in deps:
            if _is_system_lib(Path(dep).name):
                continue
            if Path(dep).resolve().is_relative_to(root.resolve()):
                continue
            if is_app and Path(dep).name not in payload:
                continue          # a system library the bundler never touched
            problems.append(f"{rel}: still resolves {dep}")

    # ...and the point of the whole exercise, stated as a requirement rather than
    # left to fall out of the loop above. The negative form could only catch this by
    # accident; the positive one also covers a SONAME/filename mismatch, where the
    # binary looks for a name the copy does not carry.
    for f in binaries:
        deps, _ = _resolved_deps(f)
        if not any(Path(d).name == lib.name
                   and Path(d).resolve().is_relative_to(root.resolve()) for d in deps):
            problems.append(f"{f.relative_to(root)}: does not resolve {lib.name} "
                            f"inside {libs}")
    if problems:
        fail("artefact is not self-contained:\n      " + "\n      ".join(problems))
    ok(f"{len(binaries) + len(list(libs.glob('*.so*'))) + len(list(opcodes.glob('*.so')))} "
       "ELF files: none resolving outside the artefact but the core runtime")
    return lib, opcodes


def prove_this_is_the_bundled_so(lib_path):
    """Linux has no sandbox-exec, and the build machine is exactly the machine that
    HAS Csound in /usr/lib — so ask the loader which file it actually mapped."""
    mapped = set()
    with open("/proc/self/maps", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split(maxsplit=5)
            if len(parts) == 6 and parts[5].startswith("/") and "csound" in parts[5]:
                mapped.add(parts[5])
    wanted = str(lib_path.resolve())
    stray = sorted(m for m in mapped if m != wanted)
    if wanted not in mapped:
        fail(f"the bundled library is not mapped in this process — mapped instead: "
             f"{', '.join(sorted(mapped)) or 'nothing'}")
    if stray:
        fail("a second Csound is mapped in this process, so this run proves "
             "nothing: " + ", ".join(stray))
    ok(f"the loaded library is the bundled one ({wanted})")


def check_windows_payload(root):
    """`root` is the directory the module lives in — beside akroasys.exe for the
    standalone, Contents/x86_64-win inside the VST3."""
    dll = root / "csound64.dll"
    plugins = root / "plugins64"
    licences = root / "licenses" / "csound"

    if not dll.is_file():
        fail(f"no shipped Csound library at {dll}")
    if not plugins.is_dir() or not any(plugins.glob("*.dll")):
        fail(f"no shipped plugin opcodes at {plugins}")
    if not (plugins / "scansyn.dll").is_file():
        fail("scansyn.dll missing — scanu/scanu2/scans would be gone")
    for name in ("NOTICE.txt", "LGPL-2.1.txt"):
        if not (licences / name).is_file():
            fail(f"LGPL 2.1 requires {name} beside the library ({licences})")
    ok(f"{dll.name} + {len(list(plugins.glob('*.dll')))} plugin opcode modules "
       f"+ licence texts present")

    # THE Windows-specific defect, and it is invisible at build time: Windows
    # resolves a module's imports against the HOST process's search path, not
    # against the directory the module itself sits in. A VST3 with a normal
    # import of csound64.dll is therefore looked for beside the DAW's .exe and
    # fails to load in most hosts — while the standalone, whose .exe IS beside
    # the DLL, works perfectly and hides it.
    modules = sorted(root.glob("*.exe")) + sorted(root.glob("*.vst3"))
    if not modules:
        # Measured against a fabricated layout: without this the whole check
        # reported PASS on a directory containing no module at all, because an
        # empty loop finds no problems. A gate that is green when there is nothing
        # to test is worse than no gate.
        fail(f"no akróasys module (.exe/.vst3) in {root} — nothing to check, and a "
             "green result here would be meaningless")
    problems = []
    for module in modules:
        imports, delayed = _pe_imports(module)
        if "csound64.dll" in imports:
            problems.append(f"{module.name} imports csound64.dll normally — it must "
                            "be delay-loaded (/DELAYLOAD:csound64.dll), or it will be "
                            "searched for beside the HOST's executable")
        elif "csound64.dll" not in delayed:
            problems.append(f"{module.name} does not reference csound64.dll at all — "
                            "was it built without Csound?")
    if problems:
        fail("\n      ".join(problems))
    ok(f"{len(modules)} module(s) delay-load csound64.dll, so it resolves from "
       "their own directory")
    return dll, plugins


# ── runtime half, inside the sandbox ────────────────────────────────────────

def prove_homebrew_is_unreachable():
    """If the sandbox did not actually cut Csound off, every result after this is
    worthless — the bundle could be reaching straight back into Homebrew."""
    reachable = [p for p in HOMEBREW_CSOUND + DENIED_PREFIXES
                 if os.path.exists(p) and _readable(p)]
    if reachable:
        fail("the sandbox did not deny Homebrew — still readable: "
             + ", ".join(reachable) + "\n      "
             "(this test proves nothing while that is true)")
    ok("neither Homebrew prefix is readable in this process")


def _readable(path):
    try:
        if os.path.isdir(path):
            os.listdir(path)
        else:
            with open(path, "rb") as fh:
                fh.read(4)
        return True
    except OSError:
        return False


def prove_this_is_the_shipped_dll(lib, lib_path):
    """Windows has no sandbox to deny a system Csound with, so prove the point
    directly: ask the loader which file the handle we are holding came from. If a
    Csound was already in the process under the same base name, this is where it
    shows."""
    buf = ctypes.create_unicode_buffer(32768)
    ctypes.windll.kernel32.GetModuleFileNameW(
        ctypes.c_void_p(lib._handle), buf, len(buf))
    loaded = Path(buf.value).resolve()
    if loaded != lib_path.resolve():
        fail(f"the loaded csound64.dll is {loaded}, not the shipped "
             f"{lib_path.resolve()} — this run would prove nothing")
    ok(f"the loaded library is the shipped one ({loaded})")


def load_bundled(lib_path, opcodes):
    try:
        lib = ctypes.WinDLL(str(lib_path)) if WINDOWS else ctypes.CDLL(str(lib_path))
    except OSError as exc:
        fail(f"the bundled library will not load: {exc}")
    if WINDOWS:
        prove_this_is_the_shipped_dll(lib, lib_path)
    for name, restype, argtypes in (
        ("csoundCreate", ctypes.c_void_p, [ctypes.c_void_p]),
        ("csoundSetOpcodedir", None, [ctypes.c_char_p]),
        ("csoundSetOption", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csoundCompileCsdText", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
        ("csoundStart", ctypes.c_int, [ctypes.c_void_p]),
        ("csoundPerformKsmps", ctypes.c_int, [ctypes.c_void_p]),
        ("csoundSetControlChannel", None,
         [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]),
        ("csoundGetSpout", ctypes.POINTER(ctypes.c_double), [ctypes.c_void_p]),
        ("csoundGetKsmps", ctypes.c_uint32, [ctypes.c_void_p]),
        ("csoundGetNchnls", ctypes.c_uint32, [ctypes.c_void_p]),
        ("csoundGetSr", ctypes.c_double, [ctypes.c_void_p]),
        ("csoundNewOpcodeList", ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p]),
        ("csoundDestroy", None, [ctypes.c_void_p]),
    ):
        fn = getattr(lib, name)
        fn.restype = restype
        fn.argtypes = argtypes
    if LINUX:
        prove_this_is_the_bundled_so(lib_path)
    lib.csoundSetOpcodedir(str(opcodes).encode())

    # STK's data, the same sibling rule as the opcode directory but through the
    # environment, because that is the only handle Csound's STK module offers.
    # POPPED when the bundle has none: an ambient RAWWAVE_PATH on the build
    # machine would otherwise let the STK probe below pass against data this
    # bundle does not carry — the same fall-through this whole file exists to
    # rule out, wearing a different hat.
    rawwaves = lib_path.parent / "rawwaves"
    if rawwaves.is_dir():
        os.environ["RAWWAVE_PATH"] = str(rawwaves) + os.sep
    else:
        os.environ.pop("RAWWAVE_PATH", None)
    ok(f"loaded {lib_path.name} with its own {opcodes.name}"
       + (" and rawwaves" if rawwaves.is_dir() else ""))
    return lib


# One opcode per bundled module that matters, written the way an orchestra would
# write it. A module that did not load takes its opcodes with it, and Csound says
# so only when something tries to compile them — which is why this is a compile
# per module rather than a count. The presence of a FILE named libscansyn.dylib
# proves nothing: with a different Mach-O under that name the file check passed
# while `scanu` was gone.
MODULE_PROBES = (
    ("fractalnoise", "asig fractalnoise 0.2, 2\nout asig"),
    ("mixer",        "asig oscili 0.2, 220\nMixerSend asig, 1, 1, 0.5\nout asig"),
    ("doppler",      "asig oscili 0.2, 220\nadop doppler asig, 1, 2\nout adop"),
    ("padsynth",     'gitab ftgen 0, 0, 262144, "padsynth", 261.6, 10, 1, 1, 1\n'
                     "asig oscili 0.2, 220, gitab\nout asig"),
    ("pvsops",       "asig oscili 0.2, 220\nfsig pvsanal asig, 1024, 256, 1024, 1\n"
                     "ftr pvstrace fsig, 20\naout pvsynth ftr\nout aout"),
    ("trigenvsegs",  "ktrig metro 2\n"
                     "kenv trigexpseg ktrig, 0.001, 0.1, 1, 0.2, 0.001\n"
                     "asig oscili 0.2 * kenv, 220\nout asig"),
)

# Not every module exists everywhere, and the differences are upstream's, not ours.
# urandom is /dev/urandom, so Windows has no such module. scansyn is the opposite
# case and the one worth knowing: Debian and Ubuntu ship NO scanned synthesis at
# all — not in libcsound64-6.0 and not in the separate csound-plugins package — so
# scanu/scanu2/scans are reachable on macOS and Windows and not on Linux. Nothing
# in backend/lco_library.json uses them. Recorded in third_party/csound/README.md
# and in docs/CSOUND_INTEGRATION.md; probed exactly where the module exists, so a
# platform that HAS it can never lose it quietly.
PLATFORM_PROBES = {
    "urandom": ("urandom", "ax urandom\nout ax * 0.1"),
    "scansyn": ("scansyn", "asig scans 0.5, 220, 1, 1\nout asig"),
}
PLATFORM_ONLY = {
    "darwin": ("urandom", "scansyn"),
    "win32":  ("scansyn",),
    "linux":  ("urandom",),
}

# 1917 entries register with no plugin modules at all, 2267 with the 24 the macOS
# payload carries, and 2294 once RAWWAVE_PATH lets libstkops register its 27 STK
# opcodes as well. A floor rather than the exact figure, so a Csound update does
# not fail a build over a few new entries — but far enough above 1917 that a
# bundle which lost its modules cannot pass. Measured: with 22 of the 24 modules
# deleted, the old check still said PASS at 1929 entries.
#
# Windows ships 16 modules and Linux ships whatever the distribution built. Neither
# figure can be measured on a Mac, but the macOS counterparts of the Windows set
# can: they register 2062 entries against the same 6.18.1 core (0 modules → 1917,
# 1 → 1928, all 24 → 2267). 2000 is that minus a margin — loose enough not to turn a
# good build red, tight enough that a payload down to two or three modules cannot
# pass. The exact count is printed on every run; the first green CI run on each
# platform is what replaces this with the measured figure.
MIN_OPCODE_ENTRIES = 2200 if not (WINDOWS or LINUX) else 2000


def check_plugin_opcodes(lib):
    """The measured trap: a bundle without Opcodes64 still compiles core opcodes
    and silently drops to the 1917 built-in entries, 350 fewer than with the
    modules we ship — no scanu/scans, no fractalnoise, no GEN padsynth."""
    cs = lib.csoundCreate(None)
    try:
        for opt in (b"-n", b"-d", b"-m0"):
            lib.csoundSetOption(cs, opt)
        entries = ctypes.c_void_p()
        count = lib.csoundNewOpcodeList(cs, ctypes.byref(entries))
    finally:
        lib.csoundDestroy(cs)
    if count < MIN_OPCODE_ENTRIES:
        fail(f"only {count} opcode entries register — 2267 with the bundled plugin "
             "modules, 1917 with none of them, so modules are missing or did not "
             "load")

    extra = tuple(PLATFORM_PROBES[n]
                  for n in PLATFORM_ONLY.get(
                      "linux" if LINUX else sys.platform, ()))
    # stkops is asked for only where its DATA shipped, which is not a platform
    # question: the module is in the macOS payload and not in the Windows one, and
    # a macOS build made without third_party/csound may have neither. Without the
    # data the module refuses to register — correctly — so probing for it would
    # fail a build over a capability it never claimed. load_bundled has already
    # decided this, and popped the variable if the bundle carries nothing.
    #
    # Compile only, like every probe here, and here it is also the SAFE choice: STK
    # opens its data file at instrument INIT, and a file it cannot find raises
    # through the module and abort()s this process — which would take the checks
    # after this one with it instead of failing one of them. What the files are is
    # answered by name and size in check_rawwaves, before anything is loaded.
    if os.environ.get("RAWWAVE_PATH"):
        extra += (("stkops", "asig STKBowed 0.4, 220, 1, 60, 2, 30, 4, 20, 11, 0\n"
                             "out asig"),)
    probes = MODULE_PROBES + extra
    missing = []
    for module, body in probes:
        cs = lib.csoundCreate(None)
        try:
            for opt in (b"-n", b"-d", b"-m0"):
                lib.csoundSetOption(cs, opt)
            csd = ("<CsoundSynthesizer>\n<CsInstruments>\nsr=44100\nksmps=64\n"
                   f"nchnls=1\n0dbfs=1\ninstr 1\n{body}\nendin\n</CsInstruments>\n"
                   "<CsScore>\ne 0\n</CsScore></CsoundSynthesizer>\n")
            if lib.csoundCompileCsdText(cs, csd.encode()) != 0:
                missing.append(module)
        finally:
            lib.csoundDestroy(cs)
    if missing:
        fail("plugin modules that did not deliver their opcodes: "
             + ", ".join(missing)
             + "\n      (the author WRITES Csound — an opcode that is not there "
               "is a capability it cannot reach)")
    ok(f"{count} opcode entries, and an opcode out of each of "
       f"{len(probes)} bundled modules compiles")


def play_real_orchestra(lib):
    """Compile and PLAY the host's own scaffold around a real library body, with
    the voice channels set to a played note — the same thing the engine does."""
    import json
    import lco_write

    library = json.loads((REPO / "backend" / "lco_library.json").read_text())
    # The library's own instruments, as the author reads them: a list of
    # {key, why, code}. Prefer a physical model over a plain waveform — a `mode`
    # bank exercises far more of the library than a single vco2 line would.
    entries = {e["key"]: e for e in library["instruments"] if e.get("code")}
    body = name = None
    for key in ("struck_bar", "bell", "string", "additive", "saw"):
        if key in entries:
            body, name = entries[key]["code"], key
            break
    if body is None and entries:
        name, entry = next(iter(entries.items()))
        body = entry["code"]
    if body is None:
        fail("no Csound body found in backend/lco_library.json to play")

    csd = lco_write.wrap(body).replace("%SR%", "44100")
    cs = lib.csoundCreate(None)
    try:
        for opt in (b"-n", b"-d", b"-m0"):
            lib.csoundSetOption(cs, opt)
        if lib.csoundCompileCsdText(cs, csd.encode()) != 0:
            fail(f"the library idiom '{name}' did not compile in the bundled Csound")
        if lib.csoundStart(cs) != 0:
            fail(f"csoundStart failed for '{name}'")
        for chan, value in (("freq", 220.0), ("gate", 1.0), ("vel", 0.8),
                            ("trig", 1.0)):
            lib.csoundSetControlChannel(cs, f"{chan}1".encode(), value)

        ksmps = lib.csoundGetKsmps(cs)
        nchnls = lib.csoundGetNchnls(cs)
        sr = lib.csoundGetSr(cs)
        spout = lib.csoundGetSpout(cs)
        peak = 0.0
        energy = 0.0
        frames = 0
        while frames < int(PERF_SECONDS * sr):
            if lib.csoundPerformKsmps(cs) != 0:
                break
            for i in range(ksmps):                    # channel 1 == voice 1
                s = spout[i * nchnls]
                peak = max(peak, abs(s))
                energy += s * s
            frames += ksmps
        if frames == 0:
            fail(f"'{name}' rendered nothing at all")
        rms = (energy / frames) ** 0.5
        if peak < SILENCE_FLOOR:
            fail(f"'{name}' played SILENCE (peak {peak:.2e}) — the bundled Csound "
                 "compiles but does not sound")
        ok(f"played '{name}' for {frames / sr:.2f} s: peak {peak:.4f}, "
           f"rms {rms:.4f} — audible")
    finally:
        lib.csoundDestroy(cs)


# ── driver ──────────────────────────────────────────────────────────────────

SANDBOX_PROFILE = """(version 1)
(allow default)
{denies}
"""


SYSTEM_PYTHON = "/usr/bin/python3"


def run_child_under_sandbox(bundle):
    denies = "\n".join(f'(deny file-read* (subpath "{p}"))' for p in DENIED_PREFIXES)
    profile = Path(os.environ.get("TMPDIR", "/tmp")) / "deny_homebrew_csound.sb"
    profile.write_text(SANDBOX_PROFILE.format(denies=denies))
    # The child is the SYSTEM python, not ours: this interpreter comes from the
    # .venv and links a Homebrew libpython, so it could not even start inside a
    # sandbox that denies the whole prefix — and a sandbox narrow enough for it to
    # start is a sandbox the bundle's dependency tree can slip through. Measured:
    # /usr/bin/python3 names nothing outside /usr/lib and /System, and the parts
    # of backend/lco_write.py this test uses are standard library only.
    if not os.path.exists(SYSTEM_PYTHON):
        fail(f"{SYSTEM_PYTHON} is missing — the check needs an interpreter that "
             "does not itself depend on Homebrew")
    print(f"      denying {', '.join(DENIED_PREFIXES)} via {profile}")
    return subprocess.run(["/usr/bin/sandbox-exec", "-f", str(profile),
                           SYSTEM_PYTHON, str(Path(__file__).resolve()),
                           str(bundle), "--child"]).returncode


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    bundle = Path(args[0]).resolve()
    if not bundle.is_dir():
        fail(f"not a bundle: {bundle}")

    if WINDOWS or LINUX:
        # No sandbox and no second process: the library is loaded by absolute path
        # and the loader is asked to confirm which file it mapped, which is a
        # stronger statement than "nothing else was reachable" and needs no
        # privilege. On Linux that matters twice over — the build machine is exactly
        # the machine that has Csound in /usr/lib.
        print(f"verifying {bundle}")
        payload, plugins = (check_windows_payload if WINDOWS
                            else check_linux_payload)(bundle)
        lib = load_bundled(payload, plugins)
        check_plugin_opcodes(lib)
        play_real_orchestra(lib)
        print("PASS  the shipped files play the LRO with no Csound on the system")
        return 0

    child = "--child" in sys.argv
    if not child:
        print(f"verifying {bundle}")
        check_bundle_contents(bundle)
        rc = run_child_under_sandbox(bundle)
        if rc == 0:
            print("PASS  the bundle plays the LRO without any Csound on the system")
        return rc

    prove_homebrew_is_unreachable()
    lib = load_bundled(bundle / "Contents" / "libs" / "CsoundLib64",
                       bundle / "Contents" / "libs" / "Opcodes64")
    check_plugin_opcodes(lib)
    play_real_orchestra(lib)
    return 0


if __name__ == "__main__":
    sys.exit(main())
