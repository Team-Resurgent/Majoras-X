#!/usr/bin/env python3
"""Majoras-X: the original Xbox build of libultraship and 2Ship2Harkinian.

setup.py runs this once the archives are in data/; run on its own, it rebuilds the game from them.

    python tools/build.py [-k] [-j N] [--only GROUP[,GROUP]] [--file PATTERN]
                          [--no-link] [--clean] [--cmd PATTERN] [--no-lto]
                          [--pgo-gen | --pgo-merge SAVE [SAVE ...] | --no-pgo]

Compiles with the RXDK's clang and the flags its Rxdk.Cli uses (copied from
`Rxdk.Cli build --verbose`), then links, runs imagebld and packs the disc the
way Rxdk.Cli does:

    build/obj/<group>/...      objects, rebuilt when a source, a header it
                               read (clang's depfile) or its command changes
    build/obj-lto/<group>/...  the same for the groups that are ThinLTO bitcode
                               (LTO_GROUPS; --no-lto builds them native, as a
                               quicker link or to compare)
    build/log/<group>/...      each failed file's compiler output
    build/Majoras-X.exe        the linked PE (with line tables, for symbols)
    build/Majoras-X.xbe        the title
    out/Release/Build/Majoras-X/       the disc's files: default.xbe, the
                               archives (data/*.o2r) and the menu font, as
                               copied to a hard disk
    out/Release/XISO/Majoras-X.iso     the disc image of them

Groups, in link order: deps (spdlog, tinyxml2, zlib, monocypher, stb),
lus (libultraship minus ImGui, SDL, scripting, MPQ), game (2Ship's C: the
decomp and 2s2h/*.c), port (2Ship's C++), xbox (xbox/src: the backends and
main). Every object is linked (not archived), so 2Ship's self-registering
hooks stay in; --gc-sections drops what nothing reaches.

-k keeps going past failures and lists them all (the porting loop);
--cmd prints the compile command for the files matching PATTERN.

There is one build. The test tools (xbox/include/lusx_debug.h: the log and
black box files, the LED signal and the overlay) are in it, off until the port
menu's Debug page turns them on (Logging, Test overlay).

Profile-guided optimisation, in three steps:

    build.py --pgo-gen          the training build: every function counts its
                                branches (out/PGO/XISO/Majoras-X-PGO.iso).
                                Play it (the slow scenes most); it saves the
                                counts to T:\\lusx_pgo.bin every 30 seconds
                                (tools/pull_logs.ps1 copies it off)
    build.py --pgo-merge F...   those saves into xbox/pgo/majoras-x.profdata
                                (Visual Studio's llvm-profdata)
    build.py                    every build uses the profile while it's there
                                (--no-pgo: without it)

A source changed since the training run builds that function without its
counts (clang matches each function by a hash of its branches); the rest keep
theirs. Train again when much has changed.
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LUS = os.path.join(ROOT, "libultraship")
SHIP = os.path.join(ROOT, "2ship2harkinian")
MM = os.path.join(SHIP, "mm")
DEPS = os.path.join(ROOT, "deps")
XBOX = os.path.join(ROOT, "xbox")

RXDK = os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")
LLVM = os.path.join(RXDK, "llvm", "xbox-windows-x64")
CLANG = os.path.join(LLVM, "bin", "clang.exe")
SDK_INC = os.path.join(RXDK, "sdk", "include")
SDK_LIB = os.path.join(RXDK, "sdk", "lib")
LIBCXX_INC = os.path.join(SDK_INC, "c++", "v1")

TITLE = "Majoras-X"  # the files' names (build/Majoras-X.xbe, out/*/XISO/Majoras-X.iso)
TITLE_NAME = "Majora's-X"  # the name the dashboard shows (the XBE's title name, at most 40 characters), as the menu
# 2Ship2Harkinian's commit the 2ship2harkinian/ tree is (with the Xbox port's changes), for its build stamp
SHIP_COMMIT = "fd9fb03"
# Team Resurgent's (TR, 0x5452) number 0x0021 in its list (the TitleID-DB repo). Each title ID has its own T: and U:
# drives, so this one's settings and saves are apart from the test builds'.
TITLE_ID = "0x54520021"
# The ID the Lus-X test builds had (LX-001): listed as an alternate ID so the game may mount that title's drive and copy
# its settings and saves across once (xbox/src/xbox_main.cpp, CopyFromOldTitle)
OLD_TITLE_ID = "0x4C580001"
STACK = 262144
LIBS = ["libd3d8.lib", "libxgraphics.lib", "libdsound.lib", "libxapi.lib", "libc.lib", "libcpp.lib", "libkernel.lib"]

# ThinLTO, as Melee-X and Prime-X release (+3.5% fps alone on Melee-X's console, more with its PGO). It also saves RAM:
# seeing across files it finds the C++ that can't throw, which .eh_frame then leaves out (2.5 MB -> 1 MB). deps and xbox
# stay native: xbox holds what the code generator calls on its own (lusx_string.c), and in bitcode LTO would drop it.
LTO_GROUPS = ("lus", "game", "port")
LTO = True
# lld imports a function into its callers up to this size; its default of 100 grew Prime-X's .text 37%, which a 64 MB
# console pays for and a 16 KB L1 doesn't repay
LTO_IMPORT_LIMIT = 10

# Profile-guided optimisation (Melee-X: +11.6% with LTO, its biggest compiler win): the groups that count their branches
# in the training build and are built with the counts after it. deps is cold; of port (2Ship's C++, -Os, mostly cold)
# only its hot files (the group's "hot" list) count. The training build has to run on a stock 64 MB console.
PGO_GROUPS = ("lus", "game", "port", "xbox")
PGO_HOT_ONLY = ("port",)
PGO_PROFILE = os.path.join(XBOX, "pgo", "majoras-x.profdata")
PGO = None  # "gen" (the training build), "use" (the profile is there) or None


def variant():
    """The training build's files apart from the normal build's (objects, libraries, the exe, the disc)."""
    return "-pgogen" if PGO == "gen" else ""


_pgo_stamp = None


def pgo_flags(g, src=None):
    """The profile's flags for a source of g (None: g's precompiled header)."""
    global _pgo_stamp
    if PGO is None or g["name"] not in PGO_GROUPS:
        return []
    if g["name"] in PGO_HOT_ONLY and (src is None or not any(re.search(h, rel(src)) for h in g.get("hot", []))):
        return [] # (a hot file takes its headers as text, not from the PCH: the PCH stays without)
    # static functions named without the file's path: the profile matches whichever folder builds it
    common = ["-mllvm", "-static-func-full-module-prefix=false"]
    if PGO == "gen":
        # no value profiling: it calls into compiler-rt's runtime, which the RXDK doesn't have
        return ["-fprofile-generate", "-mllvm", "-disable-vp"] + common
    if _pgo_stamp is None:
        with open(PGO_PROFILE, "rb") as fh:
            _pgo_stamp = hashlib.sha1(fh.read()).hexdigest()[:12]
    # the stamp: a new profile recompiles (the path alone wouldn't). Functions changed since the training run are
    # built as without a profile, which clang would warn of in each file
    return ["-fprofile-use=" + PGO_PROFILE, "-DLUSX_PGO_PROFILE=0x" + _pgo_stamp, "-Wno-profile-instr-out-of-date",
            "-Wno-profile-instr-unprofiled", "-Wno-profile-instr-missing", "-Wno-backend-plugin"] + common


def clang_resource_inc():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "include")))
    if not found:
        raise SystemExit("build: no clang resource headers under %s" % LLVM)
    return found[-1]


def builtins_lib():
    found = sorted(glob.glob(os.path.join(LLVM, "lib", "clang", "*", "lib", "windows", "libclang_rt.builtins-i386.a")))
    if not found:
        raise SystemExit("build: no libclang_rt.builtins-i386.a under %s" % LLVM)
    return found[-1]


# ---------------------------------------------------------------- flags

# What Rxdk.Cli passes (RXDK 1.3.9), but -O2 (Rxdk.Cli: -O3; size matters
# here), SSE maths (as Majora-X), function/data sections for --gc-sections,
# and _WIN32 undefined for C as well as C++: libultraship then takes its
# POSIX paths (picolibc), never <windows.h>.
def rxdk_flags(lang):
    f = []
    if lang == "c++":
        # -fno-assume-unique-vtables: clang 24's exact dynamic_cast to a final class
        # with a virtual base (LUS's controller mappings) asserts in codegen on this target
        f += ["-nostdinc++", "-fexceptions", "-frtti", "-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR",
              "-I", LIBCXX_INC, "-fms-compatibility-version=19.20", "-D_GNU_SOURCE", "-fno-assume-unique-vtables"]
    f += ["-U_WIN32", "-U__MINGW32__",
          "-target", "i686-pc-windows-gnu", "-isystem", clang_resource_inc(), "-O2",
          "-fno-sanitize=undefined", "-march=pentium3", "-msse", "-mfpmath=sse", "-mlong-double-64",
          # (not -ffreestanding/-fno-builtin: with them no sqrtf, fabsf, memcpy or memset is ever inlined,
          # a call and picolibc's wrapper each, through all of the decomp's maths and the mixer)
          # nothing reads errno after maths: a sqrtf is then the instruction, with no call behind it for a negative
          "-fno-math-errno",
          "-fno-stack-protector", "-fms-extensions", "-fms-compatibility", "-nostdinc",
          "-include", "picolibc.h", "-include", os.path.join(XBOX, "include", "lusx_prelude.h"), "-D_XBOX", "-DXBOX", "-U_DEBUG", "-D__ASSERT_VERBOSE",
          "-femulated-tls", "-gline-tables-only", "-ffunction-sections", "-fdata-sections",
          # GCC bitfield layout everywhere: the game's and libultraship's GBI structs must agree
          # (Microsoft's, this target's default, makes Gfx 16 bytes); the SDK's bitfields are
          # all one type a struct, which lay out the same either way
          "-mno-ms-bitfields",
          # everything is linked statically: no mingw .refptr indirection for extern data
          "-fno-auto-import",
          "-Wno-c++11-narrowing", "-Wno-address-of-temporary", "-Wno-ignored-pragma-intrinsic", "-Wno-multichar",
          "-Wno-unused-command-line-argument", "-Wno-deprecated-enum-enum-conversion", "-Wno-pragma-pack"]
    f += ["-std=c++20"] if lang == "c++" else ["-std=gnu11"]
    return f


def cmake_sets(path):
    """set(NAME "value") lines of a CMake file, ${VAR}s left for the caller."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for m in re.finditer(r'^\s*set\(\s*(\w+)\s+"([^"]*)"', fh.read(), re.M):
            out.setdefault(m.group(1), m.group(2))
    return out


def cvar_defines():
    # 2Ship's names first (lus-cvars.cmake renames LUS's), then LUS's defaults
    sets = {}
    for p in (os.path.join(SHIP, "CMake", "2ship-cvars.cmake"), os.path.join(SHIP, "CMake", "lus-cvars.cmake"),
              os.path.join(LUS, "cmake", "cvars.cmake")):
        for k, v in cmake_sets(p).items():
            sets.setdefault(k, v)
    for k, v in sets.items():
        sets[k] = re.sub(r"\$\{(\w+)\}", lambda m: sets.get(m.group(1), ""), v)
    return ['-D%s="%s"' % (k, v) for k, v in sorted(sets.items()) if k.startswith("CVAR_")]


# _LANGUAGE_C: LUS's GBI headers define it for _MSC_VER or __GNUC__, and in
# MS-compatibility mode the RXDK's clang has neither
COMMON_DEFS = ["-DNDEBUG", "-D_LANGUAGE_C", "-DNOMINMAX", "-DF3DEX_GBI_2", "-DCONTROLLERBUTTONS_T=uint32_t", "-DSPDLOG_COMPILED_LIB",
               "-DSPDLOG_ACTIVE_LEVEL=2",  # info and up; trace/debug compiled out
               "-DSPDLOG_NO_THREAD_ID", "-DLUS_XBOX", "-DLUS_NO_IMGUI",
               # Fast3D's texture cache: each entry keeps its GPU texture until the slot is reused
               "-DTEXTURE_CACHE_MAX_SIZE=512",
               "-DLUS_VERSION_EPOCH=2", "-DLUS_VERSION_MAJOR=0", "-DLUS_VERSION_MINOR=0", "-DLUS_VERSION_PATCH=0"]

DEP_INCS = [os.path.join(DEPS, "spdlog", "include"), os.path.join(DEPS, "json", "include"),
            os.path.join(DEPS, "tinyxml2"), os.path.join(DEPS, "zlib"), os.path.join(DEPS, "stb"),
            os.path.join(DEPS, "thread-pool", "include"), os.path.join(DEPS, "monocypher", "src"),
            os.path.join(DEPS, "monocypher", "src", "optional"), os.path.join(DEPS, "dr_libs")]

GEN = os.path.join(BUILD, "gen")

LUS_INCS = [os.path.join(XBOX, "include"), os.path.join(GEN, "lus"), os.path.join(LUS, "include"),
            os.path.join(LUS, "src")] + DEP_INCS

MM_INCS = [os.path.join(XBOX, "include"), os.path.join(MM, "assets"), os.path.join(MM, "include"),
           os.path.join(MM, "include", "PR"), os.path.join(MM, "src"), os.path.join(LUS, "include"),
           os.path.join(SHIP, "ZAPDTR", "ZAPD", "resource", "type"), MM, os.path.join(MM, "2s2h"),
           os.path.join(GEN, "mm")] + DEP_INCS

MM_DEFS = ["-D_CONSOLE", "-DNON_MATCHING", "-DNON_EQUIVALENT", "-DLOG_LEVEL_GAME_PRINTS=6", "-DUNICODE", "-D_UNICODE"]

# 2Ship's precompiled headers (mm/CMakeLists.txt), as real clang PCHs
MM_PCH_CXX = ["libultraship/bridge/consolevariablebridge.h", "ship/Context.h", "ship/resource/ResourceManager.h",
              "ship/window/Window.h", "spdlog/spdlog.h", "string", "vector", "map", "unordered_map", "filesystem",
              "fstream", "algorithm", "functional", "variant", os.path.join(MM, "2s2h", "GameInteractor",
                                                                             "GameInteractor.h"),
              os.path.join(MM, "include", "variables.h"), os.path.join(MM, "include", "functions.h"),
              os.path.join(MM, "include", "macros.h"), os.path.join(MM, "include", "z64.h")]
MM_PCH_C = [os.path.join(MM, "include", n) for n in ("ultra64.h", "global.h", "z64.h", "macros.h", "functions.h",
                                                      "variables.h")]


# ---------------------------------------------------------------- sources

def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def gather(base, patterns, exclude):
    out = []
    for pat in patterns:
        out += glob.glob(os.path.join(base, pat), recursive=True)
    ex = [re.compile(e) for e in exclude]
    return sorted(p for p in set(out) if not any(e.search(rel(p)) for e in ex))


# libultraship: what the Xbox does not have or does not want
LUS_EXCLUDE = [
    r"/fast/backends/gfx_(sdl2|opengl|dxgi|direct3d11|direct3d_common|metal|metal_shader)\.cpp$",
    r"/fast/Fast3dGui\.cpp$",
    r"/ship/window/gui/",                       # ImGui
    r"/libultraship/window/gui/",               # ImGui: gfx debugger, input editor
    r"/physicaldevice/SDLAddRemoveDeviceEventHandler\.cpp$",  # a GuiWindow polling SDL events
    r"/audio/(SDL|Wasapi|CoreAudio)AudioPlayer\.cpp$",
    r"/ship/port/mobile/",
    r"/ship/scripting/", r"/ship/security/",    # tcc scripting and its keys
    r"\.mm$", r"/utils/AppleFolderManager",
    r"/ship/resource/archive/OtrArchive\.cpp$",  # MPQ
]

MM_EXCLUDE = [
    r"/mm/src/dmadata/", r"/mm/src/elf_message/", r"/mm/src/libultra/",
    r"/mm/2s2h/Extractor/",
    r"/mm/src/boot/build\.c$",  # written by the PC CMake configure; build/gen/mm/build.c is the Xbox's
    # UltraX's MMX copy of it instead (xbox/src/audio/ux_mixer.c): the same output (tried both: the sound's
    # faults are elsewhere), a third of the CPU in the envelope mixer
    r"/mm/2s2h/mixer\.c$",
    # 2Ship's GUI: menus, editors, trackers, developer tools (the editors whose C API the game
    # calls -- HUD, cosmetics -- stay, their windows compiled out by LUS_NO_IMGUI)
    r"/mm/2s2h/BenGui/(BenGui|BenInputEditorWindow|BenMenu|BenMenuBar|BenModals|InputViewer|Menu|"
    r"ResolutionEditor|UIWidgets)\.cpp$",
    r"/mm/2s2h/DeveloperTools/(ActorViewer|CollisionViewer|DLViewer|EventLog|HookDebugger|MessageViewer|"
    r"SaveEditor|WarpPoint)\.cpp$",
    r"/mm/2s2h/Enhancements/Audio/AudioEditor\.cpp$",
    r"/mm/2s2h/Enhancements/ModMenu/",
    r"/mm/2s2h/Enhancements/Trackers/",
    r"/mm/2s2h/Enhancements/Items/ColorPictograph\.cpp$",  # saves photos as PNG (libpng)
    r"/mm/2s2h/PresetManager/",
    r"/mm/2s2h/Rando/CheckTracker/", r"/mm/2s2h/Rando/Menu\.cpp$",
    # the randomizer's logic tables (4.5 MB of the image): they only fill Rando::Logic::Regions for
    # seed generation, which is started from the menu the Xbox build doesn't have
    r"/mm/2s2h/Rando/Logic/Regions/",
]


def groups():
    stb_impl = os.path.join(GEN, "stb_impl.c")
    g = []
    g.append(dict(
        name="deps", incs=DEP_INCS, defs=COMMON_DEFS, extra=["-w", "-ffreestanding", "-fno-builtin"],
        srcs=[os.path.join(DEPS, "spdlog", "src", n + ".cpp") for n in
              ("spdlog", "stdout_sinks", "color_sinks", "file_sinks", "async", "cfg", "bundled_fmtlib_format")] +
             [os.path.join(DEPS, "tinyxml2", "tinyxml2.cpp")] +
             [os.path.join(DEPS, "zlib", n + ".c") for n in
              ("adler32", "crc32", "inflate", "inffast", "inftrees", "zutil")] +
             [os.path.join(DEPS, "monocypher", "src", "monocypher.c"),
              os.path.join(DEPS, "monocypher", "src", "optional", "monocypher-ed25519.c"), stb_impl]))
    g.append(dict(
        name="lus", incs=LUS_INCS, defs=COMMON_DEFS + cvar_defines(),
        extra=["-Wno-everything"],
        # the display-list interpreter and the NV2A backend are the hottest CPU code at 60 fps: -O3
        hot=[r"/fast/interpreter\.cpp$", r"/fast/backends/gfx_direct3d8\.cpp$"], hot_opt="-O3",
        srcs=gather(os.path.join(LUS, "src"), ["**/*.cpp", "**/*.c"], LUS_EXCLUDE)))
    g.append(dict(
        name="game", incs=MM_INCS, defs=COMMON_DEFS + cvar_defines() + MM_DEFS,
        # as 2Ship builds on an x86 PC: signed char, GCC bitfields; and as its CMake, the decomp's
        # pointer/integer mixing allowed (errors by default in this clang)
        extra=["-w", "-fno-strict-aliasing", "-fwrapv", "-mno-ms-bitfields",
               "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
               "-Wno-error=incompatible-function-pointer-types", "-Wno-error=implicit-function-declaration",
               "-Wno-error=implicit-int", "-Wno-error=return-type"],
        pch=MM_PCH_C,
        srcs=gather(MM, ["src/**/*.c", "2s2h/**/*.c"], MM_EXCLUDE)))
    g.append(dict(
        name="port", incs=MM_INCS, defs=COMMON_DEFS + cvar_defines() + MM_DEFS,
        # -Os: 2Ship's C++ is mostly cold (enhancement hooks, the randomizer, resource loaders) and
        # was 11 MB of the image at -O2; what runs every frame stays -O2 (HOT_O2)
        extra=["-w", "-fno-strict-aliasing", "-mno-ms-bitfields", "-Os"],
        hot=[r"/2s2h/Enhancements/FrameInterpolation/", r"/2s2h/GameInteractor/"],
        pch=MM_PCH_CXX,
        # with xbox/src/port: what the game needs from the GUI files left out
        srcs=gather(MM, ["2s2h/**/*.cpp"], MM_EXCLUDE) + gather(XBOX, ["src/port/**/*.cpp"], [])))
    g.append(dict(
        name="xbox", incs=[os.path.join(XBOX, "src")] + MM_INCS + LUS_INCS, defs=COMMON_DEFS + cvar_defines(),
        # as the game's: the mixer's ADPCM nibble sign-extension overflows int (UB without -fwrapv)
        extra=["-Wno-everything", "-fwrapv", "-fno-strict-aliasing"],
        srcs=gather(XBOX, ["src/**/*.cpp", "src/**/*.c"], [r"^xbox/src/port/"])))
    return g


# ---------------------------------------------------------------- generated

def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = None
    if os.path.isfile(path):
        with open(path, encoding="utf-8", newline="") as fh:
            old = fh.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)


def generate():
    # a header and not a -D, so switching rebuilds only the files that include it: a -D is in every command. 1: the test
    # tools are in every build, off until the Debug page turns them on; 0 here would leave them out
    # LUSX_PGO_GEN: the training build's runtime (xbox/src/lusx_debug.cpp), which saves the counts
    for inc in ("lus", "mm"):
        write_if_changed(os.path.join(GEN, inc, "lusx_debug_config.h"),
                         "#define LUSX_DEBUG_TOOLS 1\n#define LUSX_PGO_GEN %d\n" % (1 if PGO == "gen" else 0))
    write_if_changed(os.path.join(GEN, "stb_impl.c"),
                     "#define STB_IMAGE_IMPLEMENTATION\n#define STBI_NO_THREAD_LOCALS\n#define STBI_NO_SIMD\n#include \"stb_image.h\"\n")
    write_if_changed(os.path.join(GEN, "lus", "ship", "install_config.h"),
                     '#define CMAKE_INSTALL_PREFIX "D:\\\\"\n#define DISABLE_DLL_LOADER\n')
    # mm/src/boot/build.c, from build.c.in (2Ship's CMake writes it into the tree;
    # here it goes to build/gen and the tree's copy, if any, is left alone)
    with open(os.path.join(MM, "src", "boot", "build.c.in"), encoding="utf-8") as fh:
        text = fh.read()
    with open(os.path.join(SHIP, "CMakeLists.txt"), encoding="utf-8") as fh:
        ver = re.search(r"project\(2s2h VERSION (\d+)\.(\d+)\.(\d+)", fh.read())
    subs = {"PROJECT_BUILD_NAME": "Beatrice Alfa", "PROJECT_TEAM": "github.com/2ship2harkinian",
            "CMAKE_PROJECT_VERSION_MAJOR": ver.group(1), "CMAKE_PROJECT_VERSION_MINOR": ver.group(2),
            "CMAKE_PROJECT_VERSION_PATCH": ver.group(3), "CMAKE_PROJECT_GIT_BRANCH": "Majoras-X",
            "CMAKE_PROJECT_GIT_COMMIT_HASH": SHIP_COMMIT,
            "CMAKE_PROJECT_GIT_COMMIT_TAG": ""}
    text = re.sub(r"@(\w+)@", lambda m: subs.get(m.group(1), ""), text)
    write_if_changed(os.path.join(GEN, "mm", "build.c"), text)


def extra_sources():
    return {"game": [os.path.join(GEN, "mm", "build.c")]}


# ---------------------------------------------------------------- compiling

def obj_path(group, src):
    r = os.path.relpath(src, ROOT)
    if r.startswith(".."):
        r = os.path.basename(src)
    # bitcode and native objects apart, so a --no-lto build never recompiles the other's
    obj = ("obj-lto" if LTO and group in LTO_GROUPS else "obj") + variant()
    return os.path.join(BUILD, obj, group, os.path.splitext(r)[0] + ".obj")


def lang_of(src):
    return "c++" if src.endswith((".cpp", ".cc", ".cxx")) else "c"


def compile_cmd(g, src, obj, pch=None):
    lang = lang_of(src)
    cmd = [CLANG] + rxdk_flags(lang) + g["extra"] + g["defs"] + pgo_flags(g, src)
    for inc in g["incs"]:
        cmd += ["-I", inc]
    cmd += ["-I", SDK_INC]
    hot = any(re.search(h, rel(src)) for h in g.get("hot", []))
    if pch:
        # a PCH is made at the group's optimisation: a hot file takes its headers as text
        cmd += ["-include", pch[:-len(".pch")]] if hot else ["-include-pch", pch]
    if hot:
        cmd += [g.get("hot_opt", "-O2")]
    if LTO and g["name"] in LTO_GROUPS:
        cmd += ["-flto=thin"]
    cmd += ["-x", lang, "-c", src, "-o", obj, "-MD", "-MF", obj + ".d"]
    return cmd


def deps_newer(obj, cmd_text):
    d = obj + ".d"
    c = obj + ".cmd"
    if not (os.path.isfile(obj) and os.path.isfile(d) and os.path.isfile(c)):
        return True
    with open(c, encoding="utf-8") as fh:
        if fh.read() != cmd_text:
            return True
    t = os.path.getmtime(obj)
    with open(d, encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\\\n", " ")
    m = re.match(r"^.*?\.obj:\s", text, re.S)
    body = text[m.end():] if m else text
    for dep in body.split():
        dep = dep.replace("\\ ", " ")
        try:
            if os.path.getmtime(dep) > t:
                return True
        except OSError:
            return True
    return False


def run(cmd, log):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        os.makedirs(os.path.dirname(log), exist_ok=True)
        with open(log, "w", encoding="utf-8") as fh:
            fh.write(" ".join(cmd) + "\n\n" + out)
    return r.returncode, out


def build_pch(g, lang):
    headers = g.get("pch")
    if not headers:
        return None
    hdr = os.path.join(GEN, "pch" + variant(), "%s_%s.h" % (g["name"], lang.replace("+", "x")))
    text = "".join('#include "%s"\n' % h.replace("\\", "/") if os.path.isabs(h) else "#include <%s>\n" % h
                   for h in headers)
    write_if_changed(hdr, text)
    out = hdr + ".pch"
    cmd = [CLANG] + rxdk_flags(lang) + g["extra"] + g["defs"] + pgo_flags(g)
    for inc in g["incs"]:
        cmd += ["-I", inc]
    cmd += ["-I", SDK_INC, "-x", lang + "-header", hdr, "-o", out, "-MD", "-MF", out + ".d"]
    text_cmd = "\n".join(cmd)
    if deps_newer_pch(out, text_cmd):
        rc, msg = run(cmd, os.path.join(BUILD, "log", g["name"], "pch_%s.log" % lang))
        if rc != 0:
            print("build: %s's %s precompiled header failed:\n%s" % (g["name"], lang, first_errors(msg)))
            return False
        with open(out + ".cmd", "w", encoding="utf-8") as fh:
            fh.write(text_cmd)
    return out


def deps_newer_pch(out, cmd_text):
    # the .d names the .pch as its target, not an .obj
    d = out + ".d"
    c = out + ".cmd"
    if not (os.path.isfile(out) and os.path.isfile(d) and os.path.isfile(c)):
        return True
    with open(c, encoding="utf-8") as fh:
        if fh.read() != cmd_text:
            return True
    t = os.path.getmtime(out)
    with open(d, encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\\\n", " ")
    body = text.split(": ", 1)[1] if ": " in text else text
    for dep in body.split():
        try:
            if os.path.getmtime(dep.replace("\\ ", " ")) > t:
                return True
        except OSError:
            return True
    return False


def first_errors(text, n=3):
    lines = [l for l in text.splitlines() if " error: " in l or l.startswith("error:") or "fatal error" in l]
    return "\n".join(lines[:n]) if lines else "\n".join(text.splitlines()[-5:])


def compile_group(g, jobs, keep_going, file_filter, show_cmd):
    srcs = g["srcs"] + extra_sources().get(g["name"], [])
    if file_filter:
        srcs = [s for s in srcs if re.search(file_filter, rel(s))]
    pchs = {}
    for lang in ("c", "c++"):
        if g.get("pch") and any(lang_of(s) == lang for s in srcs):
            # each language's PCH holds its own header list: C++ 2Ship's, C the decomp's
            if (lang == "c++") == (g["name"] == "port"):
                p = build_pch(g, lang)
                if p is False:
                    return [], ["%s precompiled header" % lang]
                pchs[lang] = p
    todo, objs = [], []
    for s in srcs:
        o = obj_path(g["name"], s)
        cmd = compile_cmd(g, s, o, pchs.get(lang_of(s)))
        if show_cmd and re.search(show_cmd, rel(s)):
            print(subprocess.list2cmdline(cmd))
        objs.append(o)
        text = "\n".join(cmd)
        if deps_newer(o, text):
            todo.append((s, o, cmd, text))
    failed = []
    if not todo:
        return objs, failed
    print("build: %s: %d of %d to compile" % (g["name"], len(todo), len(srcs)))
    t0 = time.time()
    done = 0

    def one(item):
        s, o, cmd, text = item
        os.makedirs(os.path.dirname(o), exist_ok=True)
        log = os.path.join(BUILD, "log", g["name"], os.path.splitext(rel(s))[0].replace("/", "_") + ".log")
        rc, out = run(cmd, log)
        if rc == 0:
            with open(o + ".cmd", "w", encoding="utf-8") as fh:
                fh.write(text)
            if os.path.isfile(log):
                os.remove(log)
        return s, rc, out

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(one, it) for it in todo]
        for f in concurrent.futures.as_completed(futs):
            s, rc, out = f.result()
            done += 1
            if rc != 0:
                failed.append(s)
                print("  FAIL %s\n%s" % (rel(s), "\n".join("       " + l for l in first_errors(out).splitlines())))
                if not keep_going:
                    for x in futs:
                        x.cancel()
                    break
            if done % 100 == 0:
                print("  %d/%d (%.0f s)" % (done, len(todo), time.time() - t0))
    print("build: %s: %d compiled, %d failed (%.0f s)" % (g["name"], done - len(failed), len(failed),
                                                           time.time() - t0))
    return objs, failed


# ---------------------------------------------------------------- linking

EH_BEGIN = '.section .eh_frame,"dr"\n.globl ___eh_frame_start\n___eh_frame_start:\n'
EH_END = ('.section .eh_frame$z,"dr"\n.globl ___eh_frame_end\n.globl ___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6\n'
          '___eh_frame_end:\n___rxdk_eh_frame_end_requires_rxdk_tools_1_3_6:\n')


def archive(name, objs):
    """deps and libultraship as static libraries, as on the PC: an object is linked only when
    something needs it (2Ship's own stubs.c stands in for several of LUS's libultra files)."""
    lib = os.path.join(BUILD, "lib", name + ("-lto" if LTO and name in LTO_GROUPS else "") + variant() + ".lib")
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    if os.path.isfile(lib) and all(os.path.getmtime(o) <= os.path.getmtime(lib) for o in objs):
        return lib
    if os.path.isfile(lib):
        os.remove(lib)
    rsp = lib + ".rsp"
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    r = subprocess.run([os.path.join(LLVM, "bin", "llvm-ar.exe"), "rcs", lib, "@" + rsp])
    if r.returncode != 0:
        raise SystemExit("build: archiving %s failed" % name)
    return lib


XBE_PRELOAD = 2  # a section header's flag: the kernel loads the section with the title (else XLoadSection does)


def xbe_no_preload(xbe, names):
    """Clears the preload flag of the named sections in an XBE's section headers; returns their total size."""
    with open(xbe, "r+b") as fh:
        d = bytearray(fh.read())
        base, = struct.unpack_from("<I", d, 0x104)
        count, headers = struct.unpack_from("<II", d, 0x11C)
        left = 0
        for i in range(count):
            h = headers - base + 56 * i
            flags, _va, vsize, _raw, _raw_size, name_at = struct.unpack_from("<6I", d, h)
            name = bytes(d[name_at - base:d.index(b"\0", name_at - base)]).decode("ascii", "replace")
            if name in names and flags & XBE_PRELOAD:
                struct.pack_into("<I", d, h, flags & ~XBE_PRELOAD)
                left += vsize
        fh.seek(0)
        fh.write(d)
    return left


def link(objs):
    lk = os.path.join(BUILD, "link")
    os.makedirs(lk, exist_ok=True)
    eh = []
    for name, text in (("rxdk_eh_begin", EH_BEGIN), ("rxdk_eh_end", EH_END)):
        s = os.path.join(lk, name + ".S")
        write_if_changed(s, text)
        o = os.path.join(lk, name + ".o")
        r = subprocess.run([CLANG, "-target", "i686-pc-windows-gnu", "-march=pentium3", "-mlong-double-64", "-c", s,
                            "-o", o])
        if r.returncode != 0:
            raise SystemExit("build: assembling %s failed" % s)
        eh.append(o)
    rsp = os.path.join(lk, "objs.rsp")
    with open(rsp, "w", encoding="utf-8") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs) + "\n")
    exe = os.path.join(BUILD, TITLE + variant() + ".exe")
    cmd = [CLANG, eh[0], "@" + rsp] + [os.path.join(SDK_LIB, l) for l in LIBS] + [builtins_lib(), eh[1],
           "-target", "i686-pc-windows-gnu", "-march=pentium3", "-nostdlib", "-nostartfiles",
           "-Wl,--image-base=0x10000", "-Wl,--gc-sections", "-Wl,--error-limit=0",
           "-Wl,-Map=" + os.path.join(lk, TITLE + variant() + ".map"),
           # xbox_glue.cpp's strlen, which names the caller of a strlen on a bad pointer
           "-Wl,--wrap=strlen",
           "-fuse-ld=lld", "-e", "XapiTitleStartup", "-o", exe]
    if LTO:
        # -emulated-tls as the compiles' -femulated-tls: it's a code generator's option, not kept in the bitcode, and
        # without it LTO quietly makes native TLS (__tls_index). The cache: a file's change recompiles that module.
        cmd += ["-Wl,-mllvm,-emulated-tls", "-Wl,-mllvm,-import-instr-limit=%d" % LTO_IMPORT_LIMIT,
                "-Wl,--thinlto-jobs=%d" % (os.cpu_count() or 4),
                "-Wl,--thinlto-cache-dir=" + os.path.join(BUILD, "lto-cache" + variant()).replace("\\", "/")]
    print("build: linking %d objects" % len(objs))
    rc, out = run(cmd, os.path.join(BUILD, "log", "link.log"))
    if rc != 0:
        undef = sorted(set(re.findall(r"undefined symbol: (.+)", out)))
        print("build: link failed: %d undefined symbols (build/log/link.log)" % len(undef))
        for u in undef[:60]:
            print("  " + u)
        raise SystemExit(1)
    xbe = os.path.join(BUILD, TITLE + variant() + ".xbe")
    # the dashboard's title and save images, made from the title screen's Majora's Mask (tools/make_icons.py)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_icons
    icons = make_icons.main()
    images = ["/TITLEIMAGE:" + icons[0], "/DEFAULTSAVEIMAGE:" + icons[1]] if icons else []
    # the training build: the same title ID (its T:, where it saves the counts, is the game's), a name of its own
    name = TITLE_NAME + (" (PGO run)" if PGO == "gen" else "")
    r = subprocess.run([os.path.join(RXDK, "tools", "imagebld.exe"), "/in:" + exe, "/out:" + xbe, "/nologo",
                        "/stack:%d" % STACK, "/nolibwarn", "/TESTID:" + TITLE_ID, "/TESTALTID:" + OLD_TITLE_ID,
                        "/TESTNAME:" + name, "/TESTVERSION:1"] + images, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: imagebld failed")
    if PGO == "gen":
        # the console needs only the counters (.lprfc): the functions' records and names (.lprfd, .lprfn, ~10 MB) are
        # read on the PC from the exe (--pgo-merge). imagebld keeps every section before .reloc (marked discardable or
        # renamed, they stayed; llvm-objcopy's exe without them it calls corrupt), so the title loads them without
        # their preload flag: the kernel then never reads them in, and nothing on the console touches them
        print("build: the training title leaves out %.1f MB of the profile's records and names" %
              (xbe_no_preload(xbe, (".lprfd", ".lprfn")) / 1048576.0))
    out = os.path.join(ROOT, "out", "PGO" if PGO == "gen" else "Release")
    stage = os.path.join(out, "Build", TITLE)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    shutil.copy(xbe, os.path.join(stage, "default.xbe"))
    shutil.copy(os.path.join(XBOX, "assets", "menu.ttf"), stage)  # the port's menu reads it when it opens
    data = os.path.join(ROOT, "data")
    archives = sorted(glob.glob(os.path.join(data, "*.o2r")))
    for a in archives:
        shutil.copy(a, stage)
    os.makedirs(os.path.join(out, "XISO"), exist_ok=True)
    iso = os.path.join(out, "XISO", TITLE + ("-PGO" if PGO == "gen" else "") + ".iso")
    r = subprocess.run([os.path.join(RXDK, "tools", "xdvdfs.exe"), "pack", stage, iso], capture_output=True,
                       text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: xdvdfs failed")
    print("build: %s (%.2f MB), %s (%.1f MB; archives: %s)" % (
        rel(xbe), os.path.getsize(xbe) / 1048576.0, rel(iso), os.path.getsize(iso) / 1048576.0,
        ", ".join(os.path.basename(a) for a in archives) or "none -- run setup.py: it makes mm.o2r and 2ship.o2r in data/"))
    size = subprocess.run([os.path.join(LLVM, "bin", "llvm-size.exe"), "-A", exe], capture_output=True, text=True)
    sys.stdout.write(size.stdout)


# ---------------------------------------------------------------- the profile

def pe_sections(exe):
    """A PE's sections by name: (virtual address, contents to the virtual size)."""
    with open(exe, "rb") as fh:
        data = fh.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    base, = struct.unpack_from("<I", data, pe + 24 + 28)
    out = {}
    for i in range(count):
        name, vsize, va, raw_size, raw_ptr = struct.unpack_from("<8sIIII", data, pe + 24 + opt_size + 40 * i)
        body = data[raw_ptr:raw_ptr + min(vsize, raw_size)]
        out[name.rstrip(b"\0").decode("ascii", "replace")] = (base + va, body + b"\0" * (vsize - len(body)))
    return out


# The RXDK's clang writes raw profile version 11, whose 32-bit function record is 56 bytes: NameRef, FuncHash (8 each),
# CounterPtr, BitmapPtr, a pointer new in 11, FunctionPointer, Values, NumCounters (4 each), NumValueSites (3 x 2),
# a field new in 11 (2), NumBitmapBytes (4), padding. llvm-profdata 22 (Visual Studio's) reads version 10, 48 bytes:
# the same less the two new fields. CounterPtr is relative to its record, so it's made again for the record's new place.
PGO_RECORD_IN, PGO_RECORD_OUT = 56, 48
PGO_MAGIC_32 = (255 << 56) | (ord("l") << 48) | (ord("p") << 40) | (ord("r") << 32) | (ord("o") << 24) | \
               (ord("f") << 16) | (ord("R") << 8) | 129
PGO_VERSION = (1 << 56) | 10  # an IR-level profile, raw version 10


def pgo_raw(exe, save, out):
    """One T:\\lusx_pgo.bin (lusx_debug.cpp's SavePgo: a header, then the counters as they were) and the training
    build's exe (the records and names) into a .profraw. Returns (saves, seconds) from the header."""
    secs = pe_sections(exe)
    if ".lprfd" not in secs or ".lprfc" not in secs or ".lprfn" not in secs:
        raise SystemExit("build: %s has no profile sections: is it the --pgo-gen build's?" % rel(exe))
    d_va, records = secs[".lprfd"]
    c_va_exe, c_sec = secs[".lprfc"]
    names = secs[".lprfn"][1]
    with open(save, "rb") as fh:
        blob = fh.read()
    magic, c_va, c_bytes, saves, seconds = struct.unpack_from("<8sIIII", blob, 0)
    if magic != b"LXPGO1\0\0":
        raise SystemExit("build: %s isn't a counts file (T:\\lusx_pgo.bin)" % save)
    # the markers: 8 bytes before the counters and 8 after
    if c_va != c_va_exe + 8 or c_bytes != len(c_sec) - 16:
        raise SystemExit("build: %s is from another build than %s (counters at 0x%X, %d bytes; this one's at 0x%X, "
                         "%d): build --pgo-gen again or use that build's save" %
                         (save, rel(exe), c_va, c_bytes, c_va_exe + 8, len(c_sec) - 16))
    counters = blob[24:24 + c_bytes]
    if len(counters) != c_bytes:
        raise SystemExit("build: %s is cut short (%d of %d bytes of counters)" % (save, len(counters), c_bytes))
    if len(records) % PGO_RECORD_IN:
        raise SystemExit("build: .lprfd is %d bytes, not a whole number of %d-byte records: has the RXDK's clang "
                         "changed its profile format?" % (len(records), PGO_RECORD_IN))
    n = len(records) // PGO_RECORD_IN
    delta = n * PGO_RECORD_OUT  # the header's CountersDelta: as if the counters followed the records
    body = bytearray()
    for i in range(n):
        r = records[i * PGO_RECORD_IN:(i + 1) * PGO_RECORD_IN]
        name_ref, func_hash, ctr_rel, _bitmap, _new, fn, values, num_ctrs = struct.unpack_from("<QQiiiIII", r, 0)
        sites = r[40:46]
        offset = (d_va + i * PGO_RECORD_IN + ctr_rel) - c_va
        if offset < 0 or offset + 8 * num_ctrs > c_bytes:
            raise SystemExit("build: record %d's counters lie outside .lprfc" % i)
        body += struct.pack("<QQIIIII6s2xI", name_ref, func_hash, (offset + delta - i * PGO_RECORD_OUT) & 0xFFFFFFFF,
                            0, fn, values, num_ctrs, sites, 0)

    def pad(k):
        return b"\0" * ((-k) % 8)

    header = struct.pack("<16Q", PGO_MAGIC_32, PGO_VERSION, 0, n, 0, c_bytes // 8, (-c_bytes) % 8, 0, 0, len(names),
                         delta, 0, secs[".lprfn"][0], 0, 0, 2)
    with open(out, "wb") as fh:
        fh.write(header + bytes(body) + counters + pad(c_bytes) + names + pad(len(names)))
    return saves, seconds


def find_profdata():
    """llvm-profdata, the newest found: $LLVM_PROFDATA, Visual Studio's Clang tools, the PATH."""
    found = [os.environ.get("LLVM_PROFDATA", "")]
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        found += glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin",
                                        "llvm-profdata.exe"))
    found.append(shutil.which("llvm-profdata") or "")
    best, best_ver = None, (0,)
    for p in found:
        if not p or not os.path.isfile(p):
            continue
        r = subprocess.run([p, "--version"], capture_output=True, text=True)
        m = re.search(r"LLVM version (\d+)\.(\d+)", r.stdout)
        ver = (int(m.group(1)), int(m.group(2))) if m else (0,)
        if best is None or ver > best_ver:
            best, best_ver = p, ver
    if best is None or best_ver < (19,):
        raise SystemExit("build: no llvm-profdata 19 or later: install Visual Studio's \"C++ Clang tools for Windows\" "
                         "or set LLVM_PROFDATA")
    return best


def pgo_merge(saves):
    exe = os.path.join(BUILD, TITLE + "-pgogen.exe")
    if not os.path.isfile(exe):
        raise SystemExit("build: no %s: the counts are read with the training build's exe (build.py --pgo-gen)" %
                         rel(exe))
    profdata = find_profdata()
    raw_dir = os.path.join(BUILD, "pgo")
    os.makedirs(raw_dir, exist_ok=True)
    raws = []
    for i, s in enumerate(saves):
        raw = os.path.join(raw_dir, "save%d.profraw" % i)
        count, seconds = pgo_raw(exe, s, raw)
        print("build: %s: save %d, %d min of play" % (s, count, seconds // 60))
        raws.append(raw)
    os.makedirs(os.path.dirname(PGO_PROFILE), exist_ok=True)
    r = subprocess.run([profdata, "merge", "-o", PGO_PROFILE] + raws, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("build: llvm-profdata merge failed")
    r = subprocess.run([profdata, "show", PGO_PROFILE], capture_output=True, text=True)
    print("build: %s (%d KB, by %s)" % (rel(PGO_PROFILE), os.path.getsize(PGO_PROFILE) >> 10, profdata))
    sys.stdout.write("".join("  " + l + "\n" for l in r.stdout.splitlines() if l.strip()))
    print("build: every build uses it now; build again for the game with it")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--keep-going", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="comma-separated groups to compile (no link)")
    ap.add_argument("--file", help="compile only sources whose path matches this regex (no link)")
    ap.add_argument("--cmd", help="print the compile command of sources matching this regex")
    ap.add_argument("--no-link", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--no-lto", action="store_true", help="native objects: a quicker link, or to compare")
    ap.add_argument("--pgo-gen", action="store_true", help="the training build, which counts (out/PGO)")
    ap.add_argument("--pgo-merge", nargs="+", metavar="SAVE", help="the training run's saves into the profile")
    ap.add_argument("--no-pgo", action="store_true", help="build without the profile")
    a = ap.parse_args()
    global LTO, PGO
    LTO = not a.no_lto
    if a.pgo_merge:
        pgo_merge(a.pgo_merge)
        return
    PGO = "gen" if a.pgo_gen else "use" if os.path.isfile(PGO_PROFILE) and not a.no_pgo else None
    if PGO == "use":
        print("build: with the profile %s (--no-pgo: without)" % rel(PGO_PROFILE))
    if a.clean and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    generate()
    # its "built" line is __DATE__ and __TIME__: compiled every time, so a log says which build it's from
    os.utime(os.path.join(XBOX, "src", "xbox_main.cpp"))
    only = set(a.only.split(",")) if a.only else None
    all_objs, all_failed, libs = [], [], []
    for g in groups():
        if only and g["name"] not in only:
            continue
        objs, failed = compile_group(g, a.jobs, a.keep_going, a.file, a.cmd)
        if g["name"] in ("deps", "lus") and not failed and not (a.no_link or only or a.file):
            libs.append(archive(g["name"], objs))
        else:
            all_objs += objs
        all_failed += failed
        if failed and not a.keep_going:
            break
    if all_failed:
        print("build: %d files failed (logs in build/log)" % len(all_failed))
        raise SystemExit(1)
    if not (a.no_link or only or a.file):
        # the game's objects, then libultraship and its dependencies as libraries
        link(all_objs + libs)


if __name__ == "__main__":
    main()
