# DeckEmu

Touch + gamepad frontend for a shelf of libretro cores, built for Windows handhelds
(an ONEXPLAYER, 1920x1200 7" touchscreen, Ryzen 6800U). One app, many machines: pick a
system, pick a game, play. Descended from [SnesDeck](https://github.com/sp00nznet/snesdeck)
and [N64Deck](https://github.com/sp00nznet/n64deck), which were this same frontend with
one system each.

DeckEmu emulates nothing itself. Every machine is a libretro core loaded when you pick a
game, the way MAME/MESS puts many machines behind one program — except the machines here
are other people's emulators, each the best one for its job.

- **Systems** — what your library actually contains, with a count each. Not a menu of
  everything DeckEmu could run.
- **Library** — big touch rows, drag to scroll with momentum, tap to play. D-pad/stick + A
  also work, and the shoulder buttons jump a letter at a time.
- **Game** — the pad mapped to that machine's controller. **Single tap** anywhere opens the
  menu, **double tap** toggles fullscreen.
- **In-game menu** — resume, save state, load state, reset, back to library.

Starts fullscreen. Cartridge saves, save states and core data live under
`%APPDATA%\DeckEmu\`, split per system, and are always written locally — never back to
the rom share, so a dropped connection mid-game can't corrupt them.

## What actually works

Every row below was run through `--selftest` against a rom from the real library and the
resulting frame **looked at**, not just counted. Exit codes lie: a core's own error screen
and a fully transparent frame both pass a naive check, and both did at one point.

### Confirmed, picture on screen

| System | Core | Verified with |
| --- | --- | --- |
| Super Nintendo | snes9x | `Super Mario World` hack, from a `.zip` |
| Nintendo NES | nestopia | `10-Yard Fight`, from a `.zip` |
| Nintendo 64 | mupen64plus-next | `Ocarina of Time`, loose `.z64`, hardware GLideN64 |
| Sega Genesis | genesis_plus_gx | `3 Ninjas Kick Back`, from a `.zip` |
| Sega Master System | genesis_plus_gx | No-Intro `.7z` |
| Sega Game Gear | genesis_plus_gx | No-Intro `.7z` |
| Game Boy | gambatte | loose `.gb` |
| Game Boy Advance | mgba | `007 - Everything or Nothing`, from a `.zip` |
| TurboGrafx-16 | mednafen_pce | `1943 Kai`, No-Intro `.7z` |
| Commodore 64 | vice_x64 | No-Intro `.7z`, boots to the BASIC prompt |
| Atari 2600 | stella2014 | `3-D Tic-Tac-Toe`, No-Intro `.7z` |
| WonderSwan | mednafen_wswan | No-Intro `.7z` |
| MAME / MESS | mame (0.289) | `pacman.zip`, attract mode |

That covers loose roms, `.zip` and `.7z`, memory-loaded and full-path cores, software and
hardware rendering — the paths that matter are all exercised.

### Not working yet

| System | Why |
| --- | --- |
| Neo Geo / Arcade (fbneo) | The romsets here are for a different FBNeo revision — it reports the game as known but every CRC as wrong. Use MAME for these, or re-dat the sets. |
| Dreamcast, Sega CD, 3DO, PlayStation, PSP, Saturn | Need a BIOS this machine does not have yet. 3DO says so outright (`no BIOS ROM found`); PSP gets as far as creating 480x272 framebuffers and then crashes. |
| Everything else in the table | Core is installed and the rom resolves, but no game from that system has been run yet. |

### Known gaps

- **Vertical arcade games display sideways.** `SET_ROTATION` is not honoured, so Pac-Man
  and every other upright cabinet renders on its side.
- PS3, Xbox 360, PS Vita and N-Gage folders in the library are ignored on purpose —
  nothing in libretro runs them.

## The cores

Run `getcores.ps1` once. It reads the core names out of `systems.c` and pulls each from
the [libretro buildbot](https://buildbot.libretro.com/nightly/windows/x86_64/latest/)
into `cores\`. Re-run it after adding a system; it only fetches what is missing, or
everything with `-Force`.

```
.\getcores.ps1
```

A system whose core is missing simply says so when you try to play it. Nothing else
breaks.

Some systems need a BIOS the core cannot ship: Sega CD, Saturn, Dreamcast, PlayStation,
3DO, PSP and the MAME machines all want one. Drop those in `%APPDATA%\DeckEmu\system\`
under the names the core expects.

## Which core runs what

`systems.c` is the whole table — display name, core dll, extensions, and the folder names
that mean that machine. Adding a system is one row plus a re-run of `getcores.ps1`.

Extension alone cannot decide: most of a real collection is `.zip`, `.7z`, `.chd`, `.bin`
or `.iso`, none of which name a machine. So **the folder decides first** and the extension
only breaks the tie when it belongs to exactly one system. Folder names are matched with
punctuation and case thrown away, so `Nintendo SNES`, `Super_Nintendo` and `snes` are all
the same thing. That is why a library organised one-directory-per-system just works.

MAME is the catch-all at the bottom of the table: the machines nothing else emulates —
Apple II, the Macs, the TI — resolve to it.

## Archives

Roms are usually packed, so DeckEmu unpacks them. Cores that take content in memory get
the file from inside the archive directly; cores that insist on a real path (PPSSPP,
flycast, MAME, DOSBox) get it unpacked into `%APPDATA%\DeckEmu\tmp\`, kept there while
you play and cleared on exit.

This shells out to **7-Zip** rather than linking a decompressor — one small module covers
zip, 7z and rar. Install 7-Zip, or drop `7za.exe` next to `DeckEmu.exe`. Without it, loose
roms still work and archives say what is missing.

## Rom library

Tap **Rom folder** on the systems screen to pick your library with the normal Windows
folder picker — no keyboard needed. The choice is remembered in
`%APPDATA%\DeckEmu\romdir.txt`. You can also drop a folder onto the window, or pass one on
the command line. Failing all three, it looks in `roms\` next to the exe. Scans 6 levels
deep, which covers `set\system\letter\game`.

Mapped drives (`Z:\roms`) and UNC paths (`\\nas\share\roms`) both work. If the drive isn't
up yet when DeckEmu starts, it keeps your library setting rather than silently falling
back, and says so — connect the drive and tap the message to rescan. Scanning runs on a
worker thread, so an unreachable share (~20s to time out) leaves the window responsive.

## Controls

The face buttons go by position, so an Xbox pad lands on a SNES pad the right way up. The
N64 is the one exception in the table, because its core does its own remapping.

| Action | Gamepad | Keyboard |
| --- | --- | --- |
| B / A / Y / X (libretro) | A / B / X / Y | Z / X / A / S |
| L / R | LB / RB | Q / W |
| L2 / R2 | triggers | E / R |
| Select / Start | View / Menu | RShift / Enter |
| Analog stick | left stick | — |
| In-game menu | Left stick click | Esc, or a single tap |
| Back a screen | B | Esc |
| Jump a letter | LB / RB | — |
| Rom folder picker | Y, on the systems screen | tap "Rom folder" |
| Save / load state | via menu | F5 / F8 |
| Fullscreen | — | F11, or a double tap |

## Build

Needs MSVC, CMake 3.21+, and vcpkg with `sdl2` and `sdl2-ttf`.

```
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Output, the SDL runtime DLLs and `cores\` land in `build\Release\`. Text uses the system
Segoe UI, so no font ships with the app.

### Single exe build

A static vcpkg triplet links SDL2, SDL2_ttf, freetype and the CRT in, so the result is one
self-contained `deckemu.exe` with no VC++ redist needed on the target. The cores still ship
alongside — those are loaded at runtime by design.

```
vcpkg install sdl2:x64-windows-static sdl2-ttf:x64-windows-static
cmake -B build-static -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-static --config Release
```

### Checking a build

```
deckemu.exe --selftest [rom] [frames]
```

With no rom it inventories the shelf: every system, its core, and whether that core is
actually installed — the quickest way to see what can be played right now. Given a rom it
also works out which machine that is, loads the core, boots it, runs the frames and checks
the core's output came back through the render target.

Exit 0 pass, 2 no core, 3 the rom would not load, 4 nothing reached the render target.
`selftest.log` gets the core's own logging and, if it dies, the faulting module;
`selftest.bmp` gets the last frame, which is the quickest way to see whether a game is
actually drawing.
