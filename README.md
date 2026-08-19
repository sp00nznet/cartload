# Cartload

**One app, a cartload of machines.** Pick a system, pick a game, play — on a 7"
touchscreen, with a gamepad, without ever seeing a file manager or a config dialog.

![The library screen](docs/screenshots/library.png)

*The library: big touch rows, drag to scroll with momentum, tap to play. The stick and
d-pad work too, and the shoulder buttons jump a letter at a time.*

Cartload is a touch + gamepad frontend for a shelf of libretro cores, built for Windows
handhelds (an ONEXPLAYER: 1920x1200 7" screen, Ryzen 6800U). It emulates nothing itself.
Every machine is somebody else's emulator, loaded as a libretro core the moment you pick a
game — the way MAME/MESS puts many machines behind one program, except each machine here
gets the core that is best at it.

**This is a menu on top of decades of other people's work.** snes9x, nestopia,
mupen64plus-next, Genesis Plus GX, gambatte, mGBA, Mednafen, MAME, DOSBox and the rest do
the emulation; libretro makes them all speak one language; SDL draws the window and 7-Zip
opens the archives. Cartload draws lists and hands them a file.
[Credit where it is due](docs/credits.md).

- **86,000 roms across 27 machines scan in about four seconds** over SMB, and the list
  scrolls at 60fps with momentum.
- **The folder decides the machine, not the extension** — half a real collection is `.zip`
  and `.7z`, which name nothing. A library organised one-directory-per-system just works.
- **Cores install themselves.** Missing an emulator? Tap it.
- **Saves never go back to the share.** Cartridge saves and states are written locally, so
  a dropped wifi connection mid-game cannot corrupt them.
- **One 2.8MB exe.** No installer, no redist, no SDL DLLs, no Python, no web view.

## The screens

**Systems** — what your library actually contains, with a count each. Not a menu of
everything Cartload could run.

![Systems](docs/screenshots/systems.png)

**Cores** — every machine, the core that runs it, and whether it is installed. Tap a row
to download it from the libretro buildbot; the row counts the seconds, so a 250MB MAME
core doesn't look like a hang.

![Cores](docs/screenshots/cores.png)

**Game** — the pad mapped to that machine's controller, the frame at the aspect the core
asks for. Single tap opens the menu, double tap toggles fullscreen.

![A game running](docs/screenshots/game.png)

**In-game menu** — resume, save state, load state, reset, back to the library. Big enough
to hit with a thumb while the game waits underneath.

![In-game menu](docs/screenshots/menu.png)

## Quick start

1. [Build it](docs/build.md), or copy a built folder to the handheld — keep `cores\` next
   to `cartload.exe`.
2. Install 7-Zip, or drop `7za.exe` beside the exe. Most roms arrive packed; without it,
   only loose files load.
3. Run `cartload.exe`. It starts fullscreen on the systems screen.
4. Tap **Rom folder** and point it at your library — a mapped drive or a UNC path both
   work. It remembers.
5. Tap **Cores** and grab whatever is missing.

## Two things to know before you run it

**Windows will warn about the exe.** It is unsigned — a code signing certificate costs
more per year than this project has ever cost — so SmartScreen shows "Windows protected
your PC" on first run. *More info* → *Run anyway*, or build it yourself from source and
skip the question. The version resource is filled in properly, which is what keeps
Defender's heuristics off a statically linked binary, but it is no substitute for a
signature.

**Cores are third-party binaries, downloaded on request.** Tapping a row on the Cores
screen fetches an unsigned DLL over HTTPS from the libretro nightly buildbot and loads it
into this process, which is exactly as much trust as `getcores.ps1` always asked for, and
the same trust RetroArch's own core downloader asks for. If that is not a trade you want,
build the cores yourself and drop them in `cores\` — nothing else changes.

## What actually runs

Thirteen machines are confirmed working with a **frame that was looked at**, not just an
exit code: SNES, NES, N64, Genesis, Master System, Game Gear, Game Boy, GBA,
TurboGrafx-16, Commodore 64, Atari 2600, WonderSwan and MAME. Another dozen are wired up
and waiting on a BIOS or a first test.

The honest, per-system version of that — including what is broken and why — is in
[docs/systems.md](docs/systems.md).

## Controls

| | Gamepad | Touch |
| --- | --- | --- |
| Open / play | A | tap |
| Back a screen | B | the header button |
| Jump a letter | LB / RB | drag the right edge |
| Cores screen | X | tap **Cores** |
| Rom folder | Y | tap **Rom folder** |
| In-game menu | left stick click | single tap |
| Fullscreen | — | double tap |

Keyboard equivalents and the full libretro mapping are in [docs/usage.md](docs/usage.md).

## Docs

- [Using Cartload](docs/usage.md) — rom library, cores, archives, saves, full controls
- [What runs](docs/systems.md) — verified systems, what's broken, how a rom finds its machine
- [Building](docs/build.md) — cmake, the single-exe build, and the selftest
- [Credits](docs/credits.md) — every core, and who actually wrote the emulator

## License

Cartload itself is MIT — see [LICENSE](LICENSE). `libretro.h` is included unmodified under
its own MIT license, © 2010-2024 The RetroArch team.

The cores are **not** part of this project. Each has its own license — several are GPL,
and some, snes9x among them, are not free for commercial use — and each is downloaded from
the libretro buildbot at your request, never bundled here. Bring your own roms.
