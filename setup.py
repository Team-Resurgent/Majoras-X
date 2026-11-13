#!/usr/bin/env python3
"""One command from your Majora's Mask ROM to an Xbox disc image.

    python setup.py                 # everything -> out/Release/XISO/Majoras-X.iso
    python setup.py --check         # say what is there and what is missing
    python setup.py --skip-build    # everything but compiling the game (the archives, the tools)

Needs: Python 3.8+ (standard library only), Git, Visual Studio 2022 or later
with "Desktop development with C++" (its CMake builds 2Ship2Harkinian's asset
extractor), the RXDK (it builds the game), an internet connection the first
time, and your Majora's Mask ROM in this folder as baserom.z64. A .v64 / .n64
(byte-swapped) dump is accepted and put in order.

Nothing that comes from the ROM is in this repository, and nothing on this
PC but the above is used. In order, into folders .gitignore keeps out:

  1. the ROM, checked against the versions 2Ship2Harkinian extracts, and put
     in big-endian order: build/rom/baserom.z64
  2. fetched, each at one exact commit, so a change upstream changes nothing
     here: 2Ship2Harkinian at the commit this repository's 2ship2harkinian/
     tree is, with its asset extractor (ZAPDTR, OTRExporter) and the PC
     libultraship it is built with (build/upstream/2ship2harkinian), and the
     libraries the game is built with (deps/, tools/fetch_deps.py)
  3. 2Ship2Harkinian's asset extractor, ZAPD, built with Visual Studio as
     2Ship's own Windows build makes it (its CMake fetches the libraries it
     needs through vcpkg the first time): build/extract/
  4. data/mm.o2r, extracted from the ROM, and data/2ship.o2r, 2Ship's own
     extras packed
  5. the game, built with the RXDK (tools/build.py), with the dashboard's
     images made from the title screen's Majora's Mask: out/<config>/XISO/
     Majoras-X.iso and the same files as a folder, out/<config>/Build/
     Majoras-X/, to copy to an Xbox's hard disk
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
NAME = "Majoras-X"
BUILD = os.path.join(ROOT, "build")
DATA = os.path.join(ROOT, "data")
ROM_NAMES = ("baserom.z64", "baserom.v64", "baserom.n64", "mm.z64", "mm.v64", "mm.n64")
ROM = os.path.join(BUILD, "rom", "baserom.z64")
TESTED_SHA1 = "d6133ace5afaa0882cf214cf88daba39e266c078"  # USA 1.0 (N64): the one the port is tested with
# the Majora's Mask ROMs 2Ship2Harkinian extracts, by the CRC in their header (OTRExporter/rom_info.py)
KNOWN_ROMS = {
    "5354631C": "USA 1.0 (N64)",
    "DA6983E7": "USA 1.0 (N64, decompressed)",
    "B443EB08": "USA (GameCube)",
    "8473D0C1": "Japan (GameCube)",
}

# 2Ship2Harkinian: the commit this repository's 2ship2harkinian/ tree is (with the Xbox port's changes), fetched for
# what isn't in the tree: its asset definitions (mm/assets/xml), extractor settings (mm/assets/extractor), its own extra
# files (mm/assets/custom) and the CMake project its extractor is built with
SHIP_URL = "https://github.com/HarbourMasters/2ship2harkinian.git"
SHIP_REV = "fd9fb03bdeb1f89541d1d3bf99cdb0677f21edc6"
SHIP_SRC = os.path.join(BUILD, "upstream", "2ship2harkinian")
# its submodules, at the commits that commit has: the extractor and the PC libultraship it links
SUBMODULES = (
    ("ZAPDTR", "https://github.com/HarbourMasters/ZAPDTR.git", "ee3397a365c5f350a60538c88f0643f155944836"),
    ("OTRExporter", "https://github.com/HarbourMasters/OTRExporter.git", "32e088e28c8cdd055d4bb8f3f219d33ad37963f3"),
    ("libultraship", "https://github.com/kenix3/libultraship.git", "7f9b86a593c526fc42261d7fe197100cecf57178"),
)

TOOLS_BUILD = os.path.join(BUILD, "extract")
ARCHIVES_STAMP = os.path.join(DATA, "made-from.txt")


def say(msg):
    print("setup: " + msg, flush=True)


def die(msg):
    print("\nsetup: " + msg, flush=True)
    raise SystemExit(1)


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        die("failed (exit %d): %s" % (r.returncode, " ".join('"%s"' % c if " " in c else c for c in cmd)))


def git_head(path):
    if not os.path.exists(os.path.join(path, ".git")):
        return None
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def remove_tree(path):
    """shutil.rmtree, through the read-only files git leaves in its object store."""
    def retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=retry)


def ship_version():
    with open(os.path.join(ROOT, "2ship2harkinian", "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(2s2h VERSION (\d+\.\d+\.\d+)", f.read())
    return m.group(1)


def upstream_ok():
    return git_head(SHIP_SRC) == SHIP_REV and all(
        git_head(os.path.join(SHIP_SRC, path)) == rev for path, _url, rev in SUBMODULES)


def zapd_exe():
    # (ZAPD's CMake puts it in the source tree's x64/Release, beside 2Ship's other outputs, not the build folder)
    p = os.path.join(SHIP_SRC, "x64", "Release", "ZAPD.exe")
    return p if os.path.isfile(p) else None


# ---------------------------------------------------------------- the ROM

def find_rom():
    for n in ROM_NAMES:
        p = os.path.join(ROOT, n)
        if os.path.isfile(p):
            return p
    return None


def rom_in_order(data):
    """The ROM in .z64 (big-endian) order: a .v64 swaps each pair of bytes, a .n64 each four."""
    head = data[:4]
    if head == b"\x80\x37\x12\x40":
        return data
    if head == b"\x37\x80\x40\x12":
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if head == b"\x40\x12\x37\x80":
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    return None


def rom_ok():
    """(sha1, version name) for a ROM in order at build/rom/baserom.z64, or (None, why not)."""
    src = find_rom()
    if src is None:
        return None, "no ROM: put your Majora's Mask ROM in %s as baserom.z64" % ROOT
    with open(src, "rb") as f:
        data = rom_in_order(f.read())
    if data is None:
        return None, "%s is not an N64 ROM (its first bytes are none of the three byte orders)" % os.path.basename(src)
    crc = data[16:20].hex().upper()
    if crc not in KNOWN_ROMS:
        return None, ("%s is not a Majora's Mask ROM 2Ship2Harkinian extracts (header CRC %s); the USA 1.0 one's SHA-1"
                      " is %s" % (os.path.basename(src), crc, TESTED_SHA1))
    sha = hashlib.sha1(data).hexdigest()
    have = None
    if os.path.isfile(ROM):
        with open(ROM, "rb") as f:
            have = hashlib.sha1(f.read()).hexdigest()
    if have != sha:
        os.makedirs(os.path.dirname(ROM), exist_ok=True)
        with open(ROM, "wb") as f:
            f.write(data)
    return sha, KNOWN_ROMS[crc] + ("" if sha == TESTED_SHA1 else " (not the tested dump: SHA-1 %s)" % sha)


# ---------------------------------------------------- Visual Studio, CMake

def find_vs_install():
    """The newest Visual Studio (2022 or later) with the x64 C++ tools, by vswhere, which every Visual Studio installer
    puts in the same place: (its folder, its major version -- 17 for 2022, 18 for 2026), or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    r = subprocess.run([vswhere, "-latest", "-products", "*", "-version", "[17.0,",
                        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-format", "json", "-utf8"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        found = json.loads(r.stdout) if (r.returncode == 0 and r.stdout) else []
    except ValueError:
        found = []
    if not found:
        return None
    path = found[0].get("installationPath", "")
    try:
        major = int(found[0].get("installationVersion", "0").split(".")[0])
    except ValueError:
        major = 0
    return (path, major) if os.path.isdir(path) else None


def vs_generator(cmake, major):
    """The Visual Studio generator of `major` this CMake has ("Visual Studio 17 2022"), or None."""
    r = subprocess.run([cmake, "--help"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in r.stdout.splitlines():
        name = line.split("=")[0].strip().lstrip("*").strip()
        if name.endswith("[arch]"):
            name = name[:-len("[arch]")].strip()
        if name.startswith("Visual Studio %d " % major):
            return name
    return None


def find_cmake():
    """(cmake, generator) able to build with the Visual Studio found -- its own CMake first, then one on the PATH -- or
    (a cmake, None) when none knows it, or (None, None) with no CMake at all."""
    vs = find_vs_install()
    cands = []
    if vs:
        p = os.path.join(vs[0], "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        if os.path.isfile(p):
            cands.append(p)
    c = shutil.which("cmake")
    if c and all(os.path.normcase(c) != os.path.normcase(o) for o in cands):
        cands.append(c)
    if vs:
        for c in cands:
            gen = vs_generator(c, vs[1])
            if gen:
                return c, gen
    return (cands[0] if cands else None), None


def rxdk_root():
    return os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")


# ---------------------------------------------------------------- --check

def check():
    ok = True
    py = sys.version_info >= (3, 8)
    say("Python: %d.%d%s" % (sys.version_info[0], sys.version_info[1], "" if py else " (3.8 or later needed)"))
    ok &= py
    git = shutil.which("git")
    say("Git: %s" % (git or "missing (install Git for Windows: https://git-scm.com/download/win)"))
    ok &= git is not None
    rx = rxdk_root()
    clang = os.path.join(rx, "llvm", "xbox-windows-x64", "bin", "clang.exe")
    imagebld = os.path.join(rx, "tools", "imagebld.exe")
    rx_ok = os.path.isfile(clang) and os.path.isfile(imagebld)
    say("RXDK: %s" % (rx if rx_ok else "missing in %s (RXDK > RXDK Window > Install Prerequisites)" % rx))
    ok &= rx_ok
    vs = find_vs_install()
    say("Visual Studio C++ tools: %s" % (vs[0] if vs else
                                          "missing (Visual Studio 2022 or later, Desktop development with C++)"))
    ok &= vs is not None
    cm, gen = find_cmake()
    if cm is None:
        say("CMake: missing (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    elif gen is None:
        say("CMake: %s does not know this Visual Studio (install a newer CMake)" % cm)
    else:
        say("CMake: %s (%s)" % (cm, gen))
    ok &= gen is not None
    if find_rom() is None:
        say("ROM: missing (put your Majora's Mask ROM here as baserom.z64)")
        ok = False
    else:
        sha, what = rom_ok()
        say("ROM: %s, %s" % (os.path.basename(find_rom()), what) if sha else "ROM: " + what)
        ok &= sha is not None
    say("2Ship2Harkinian (assets, extractor): %s" % ("fetched" if upstream_ok() else
                                                    "not fetched yet (the build fetches it)"))
    for label, path in (("asset extractor", zapd_exe()), ("data/mm.o2r", os.path.join(DATA, "mm.o2r")),
                        ("data/2ship.o2r", os.path.join(DATA, "2ship.o2r"))):
        say("%s: %s" % (label, "built" if path and os.path.isfile(path) else "not built yet"))
    say("everything needed is here" if ok else "something is missing (above)")
    return ok


# ------------------------------------------------------------------ steps

def fetch_at(url, rev, dst):
    """dst as a checkout of url at exactly rev (shallow)."""
    if git_head(dst) == rev:
        return
    remove_tree(dst)
    os.makedirs(dst)
    run(["git", "init", "-q", dst])
    run(["git", "-C", dst, "remote", "add", "origin", url])
    run(["git", "-C", dst, "fetch", "-q", "--depth", "1", "origin", rev])
    run(["git", "-C", dst, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"])
    head = git_head(dst)
    if head != rev:
        die("%s is commit %s, not %s" % (url, head, rev))


def step_fetch():
    if shutil.which("git") is None:
        die("Git not found (install Git for Windows: https://git-scm.com/download/win)")
    if git_head(SHIP_SRC) != SHIP_REV:
        say("fetching 2Ship2Harkinian (its asset definitions, extras and the extractor's CMake project)")
        fetch_at(SHIP_URL, SHIP_REV, SHIP_SRC)
    for path, url, rev in SUBMODULES:
        dst = os.path.join(SHIP_SRC, path)
        if git_head(dst) != rev:
            say("fetching %s" % path)
            fetch_at(url, rev, dst)
    say("fetching the libraries (deps/)")
    run([sys.executable, os.path.join(ROOT, "tools", "fetch_deps.py")])


def step_tools(jobs):
    vs = find_vs_install()
    if vs is None:
        die("Visual Studio 2022 or later with the C++ tools not found (Desktop development with C++)")
    cmake, gen = find_cmake()
    if cmake is None:
        die("CMake not found (Visual Studio's C++ CMake tools for Windows, or cmake.org)")
    if gen is None:
        die("%s is too old for Visual Studio %d: install a newer CMake, or Visual Studio's C++ CMake tools" %
            (cmake, vs[1]))
    cache = os.path.join(TOOLS_BUILD, "CMakeCache.txt")
    if os.path.isfile(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if ("CMAKE_GENERATOR:INTERNAL=%s\n" % gen) not in text:
            say("build/extract was set up for another Visual Studio: starting it again")
            remove_tree(TOOLS_BUILD)
    say("configuring 2Ship2Harkinian's CMake project with %s, for its asset extractor (the first time, vcpkg downloads "
        "and builds the libraries it needs: this takes a while)" % gen)
    # CMAKE_POLICY_VERSION_MINIMUM: CMake 4 (Visual Studio 2026's) refuses a project that says it works with CMake
    # older than 3.5, as some of the libraries do; this is CMake's own way to build them anyway
    run([cmake, "-S", SHIP_SRC, "-B", TOOLS_BUILD, "-G", gen, "-A", "x64",
         "-DCMAKE_GENERATOR_INSTANCE=" + vs[0], "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"])
    say("building the extractor (ZAPD)")
    run([cmake, "--build", TOOLS_BUILD, "--config", "Release", "--target", "ZAPD", "--parallel", str(jobs)])
    if zapd_exe() is None:
        die("the extractor (ZAPD.exe) was not built")


def step_archives(rom_sha):
    """data/mm.o2r and data/2ship.o2r, made again when the ROM or the extractor changed."""
    zapd = zapd_exe()
    made_from = "rom %s\nship %s\nzapd %d\n" % (rom_sha, SHIP_REV, int(os.path.getmtime(zapd)))
    have = None
    if os.path.isfile(ARCHIVES_STAMP):
        with open(ARCHIVES_STAMP, encoding="utf-8") as f:
            have = f.read()
    if have == made_from and all(os.path.isfile(os.path.join(DATA, n)) for n in ("mm.o2r", "2ship.o2r")):
        say("data/: the archives are up to date")
        return
    remove_tree(DATA)
    os.makedirs(DATA)
    # as 2Ship's ExtractAssets target: from its mm/ folder (the extractor's settings are relative to it)
    mm = os.path.join(SHIP_SRC, "mm")
    for n in ("mm.o2r", "2ship.o2r"):
        if os.path.isfile(os.path.join(mm, n)):
            os.remove(os.path.join(mm, n))
    say("extracting the game from the ROM into data/mm.o2r, and packing 2Ship's own files into data/2ship.o2r "
        "(a few minutes)")
    # (the custom files' folder with forward slashes, as CMake gives it: OTRExporter names each file by splitting its
    # path on that folder, and with backslashes the split finds nothing and ZAPD crashes before 2ship.o2r is written)
    run([sys.executable, os.path.join(SHIP_SRC, "OTRExporter", "extract_assets.py"), "-z", zapd, ROM,
         "--non-interactive", "--xml-root", "../mm/assets/xml", "--custom-otr-file", "2ship.o2r",
         "--custom-assets-path", os.path.join(mm, "assets", "custom").replace("\\", "/"),
         "--port-ver", ship_version()], cwd=mm)
    for n in ("mm.o2r", "2ship.o2r"):
        if not os.path.isfile(os.path.join(mm, n)):
            die("the extractor did not make %s" % n)
        shutil.move(os.path.join(mm, n), os.path.join(DATA, n))
    with open(ARCHIVES_STAMP, "w", encoding="utf-8") as f:
        f.write(made_from)


def step_build(jobs):
    say("building the game with the RXDK")
    run([sys.executable, os.path.join(ROOT, "tools", "build.py"), "-j", str(jobs)])
    config = "Release"
    iso = os.path.join(ROOT, "out", config, "XISO", NAME + ".iso")
    if not os.path.isfile(iso):
        die("the disc image was not made")
    say("done: %s" % os.path.relpath(iso, ROOT))
    say("      %s (the same files, to copy to a hard disk)" %
        os.path.relpath(os.path.join(ROOT, "out", config, "Build", NAME), ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="say what is there and what is missing, and stop")
    ap.add_argument("--skip-build", action="store_true", help="stop before compiling the game")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if a.check:
        raise SystemExit(0 if check() else 1)
    if sys.version_info < (3, 8):
        die("Python 3.8 or later is needed")
    rom_sha, what = rom_ok()
    if rom_sha is None:
        die(what)
    say("ROM: %s" % what)
    step_fetch()
    step_tools(a.jobs)
    step_archives(rom_sha)
    if a.skip_build:
        say("stopped before the game's build (--skip-build)")
        return
    step_build(a.jobs)


if __name__ == "__main__":
    main()
