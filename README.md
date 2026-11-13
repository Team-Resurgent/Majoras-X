# Majora's Mask for the original Xbox

A native original-Xbox port of *The Legend of Zelda: Majora's Mask*, built from
[2Ship2Harkinian](https://github.com/HarbourMasters/2ship2harkinian) and its engine,
[libultraship](https://github.com/Kenix3/libultraship). The game is 2Ship2Harkinian's,
compiled for the Xbox's Pentium III; what it runs on is in `xbox/` and libultraship's Xbox
backends.

**No game data is included.** You need your own Majora's Mask ROM. Everything taken from
it is made on your own machine.

| ROM | SHA-1 (`.z64`) | |
| --- | --- | --- |
| USA 1.0 (N64) | `d6133ace5afaa0882cf214cf88daba39e266c078` | tested |

2Ship2Harkinian also extracts the USA GameCube and Japanese GameCube versions; the build
accepts them, but only the N64 USA ROM has been tested on the Xbox.

## Features

* The full game, with 2Ship2Harkinian's fixes and enhancements
* 720p, 480p or 480i, following the dashboard's video settings, in 16:9
* Up to 60 frames a second: the game runs at its own 20 ticks a second and the frames in
  between are drawn from 2Ship2Harkinian's frame interpolation, with a frame rate limit
  (Auto, 40, 30, 20) for a steadier picture
* An optional widescreen HUD, at the edges of the 16:9 screen
* An in-game menu on **Back**: fully remappable controls, options and cheats
* Options for autosave, owl saves that last, skipping cutscenes, faster text, songs and
  chests, fast mask transformations, D-pad item equips, enemy health bars, the clock's
  style, motion blur, sound volumes and many more of 2Ship2Harkinian's enhancements
* Each save file is its own Xbox save game, managed from the dashboard
* Rumble, with full, medium and low strengths
* Runs on a stock 64 MB console

## Requirements

* Windows 10 or 11
* the RXDK toolchain: in Visual Studio, **RXDK > RXDK Window > Install Prerequisites**
  (or RXDK's own installer), in `C:\ProgramData\RXDK`
* [Python](https://www.python.org/downloads/) 3.8 or later
* [Git](https://git-scm.com/download/win)
* [Visual Studio](https://visualstudio.microsoft.com/) 2022 or later with **Desktop
  development with C++** (it includes **C++ CMake tools for Windows**, which build
  2Ship2Harkinian's asset extractor; the free Build Tools edition is enough)
* an internet connection the first time you build: 2Ship2Harkinian's asset definitions and
  its extractor (ZAPDTR, OTRExporter, and the PC libultraship it is built with) and the
  libraries the game is built with are downloaded, each at one exact commit, so nothing that
  changes upstream changes this build

`python setup.py --check` lists what is there and what is missing.

## Building

1. Download this repository (**Code > Download ZIP**, or `git clone`).
2. Put your ROM in its folder, named `baserom.z64` (a byte-swapped `.v64` or `.n64` is put
   in order for you).
3. Double-click **`build.bat`**, or in a console:

```bash
python setup.py
```

The result is **`out\Release\XISO\Majoras-X.iso`**. The first build takes a while (the
extractor and the libraries it needs, the extraction and the game are all built); later
ones only redo what changed.

The build runs, in order:

1. checks the ROM against the versions 2Ship2Harkinian extracts and puts it in `.z64` order
   (`build/rom/`);
2. fetches 2Ship2Harkinian at the commit this repository's `2ship2harkinian/` tree is, with
   its extractor's submodules (`build/upstream/2ship2harkinian`), and the libraries (`deps/`);
3. builds the extractor, ZAPD, with Visual Studio from 2Ship2Harkinian's own CMake project
   (`build/extract/`; the first time, its vcpkg downloads and builds the PC libraries the
   extractor needs);
4. extracts the game from your ROM into `data/mm.o2r`, and packs 2Ship2Harkinian's own
   extras into `data/2ship.o2r`;
5. builds the game with the RXDK (`tools/build.py`), makes the dashboard pictures from the
   title screen's Majora's Mask, and packs the disc.

```bash
python setup.py --check         # what is present and what is missing
python setup.py --skip-build    # everything but the game's compile
```

The build also has what a test run needs on a retail console with nothing listening, off
until it's turned on from the port menu's Debug page:

* **Logging**: a log file and a black box on the hard disk that survive a crash or a
  power-off, and a report of the last run's end at the next start. Off, nothing is written.
  `tools\pull_logs.ps1` copies the files off the console over its FTP server.
* **Test overlay**: the frame rate, memory and timings on screen.
* **Debug mode**: Warp (any area and entrance) and Edit save.

## Running

* **Disc image:** boot `out\Release\XISO\Majoras-X.iso` on a modded console or in
  [xemu](https://xemu.app/).
* **Hard disk:** copy the folder `out\Release\Build\Majoras-X\` (`default.xbe`, the two
  `.o2r` archives and the menu font) to your Xbox and launch `default.xbe`.

Each of the three save files is its own Xbox save game, named after its slot and player
("File 1 - Link"), which the dashboard lists under **Memory > Majora's-X** and can copy or
delete on its own; erasing or copying a file on the file select does the same to its save
game. The settings are kept apart, in the game's own folder (`E:\TDATA\54520021`), and
survive deleting a save. Settings and saves from the earlier Lus-X test builds are brought
over into these places the first time the game starts.

Under xemu, turn **GPU vertex buffers** off (Options > Display): xemu draws them slower than
a console does. The change takes full effect the next time the game is launched.

## What is in this repository

* `2ship2harkinian/` -- 2Ship2Harkinian's game code (`mm/src`, `mm/2s2h`, `mm/include`, the
  asset name headers), with the Xbox port's changes; not its asset definitions, extractor
  settings or extra files, which `setup.py` fetches
* `libultraship/` -- libultraship at the commit 2Ship2Harkinian uses, with its Xbox port:
  the Direct3D 8 renderer, DirectSound, the Xbox controllers and file system
* `xbox/` -- the Xbox side: `main`, the port's menu, saves, the test tools, UltraX's MMX
  audio mixer
* `tools/` -- `build.py` (the game's build with the RXDK's clang), `fetch_deps.py`,
  `make_icons.py`, `pull_logs.ps1`
* `setup.py`, `build.bat` -- from the ROM to the disc

## Credits

* [2Ship2Harkinian](https://github.com/HarbourMasters/2ship2harkinian) by HarbourMasters,
  from the [Majora's Mask decompilation](https://github.com/zeldaret/mm) by ZeldaRET
* [libultraship](https://github.com/Kenix3/libultraship),
  [ZAPDTR](https://github.com/HarbourMasters/ZAPDTR) and
  [OTRExporter](https://github.com/HarbourMasters/OTRExporter)
* UltraX, for the MMX audio mixer
* the RXDK, by Team Resurgent
