# Credits

Cartload is a menu. It draws lists, reads a folder, maps a gamepad, and hands a file to
somebody else's emulator. **Every machine it plays is emulated by other people's work** —
decades of it, given away for free — and the whole project would be a blank window without
them.

## The cores

Each of these is loaded at runtime from `cores\`, downloaded from the
[libretro buildbot](https://buildbot.libretro.com/nightly/windows/x86_64/latest/). None of
them are part of this repository, none of their code is in this binary, and each carries
its own license — several are GPL, some have their own terms (snes9x, for one, is not
free for commercial use). If you redistribute cores, read theirs, not this project's.

| Core | Machines it runs here | Upstream |
| --- | --- | --- |
| snes9x | Super Nintendo | [snes9xgit/snes9x](https://github.com/snes9xgit/snes9x) |
| nestopia | NES | [0ldsk00l/nestopia](https://github.com/0ldsk00l/nestopia) |
| mupen64plus-next + GLideN64 | Nintendo 64 | [libretro/mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx), [gonetz/GLideN64](https://github.com/gonetz/GLideN64) |
| gambatte | Game Boy, Game Boy Color | [libretro/gambatte-libretro](https://github.com/libretro/gambatte-libretro) |
| mGBA | Game Boy Advance | [mgba-emu/mgba](https://github.com/mgba-emu/mgba) |
| melonDS | Nintendo DS | [melonDS-emu/melonDS](https://github.com/melonDS-emu/melonDS) |
| Citra | Nintendo 3DS | [libretro/citra](https://github.com/libretro/citra) |
| Genesis Plus GX | Genesis, Master System, Game Gear, Sega CD | [ekeeke/Genesis-Plus-GX](https://github.com/ekeeke/Genesis-Plus-GX) |
| PicoDrive | Sega 32X | [irixxxx/picodrive](https://github.com/irixxxx/picodrive) |
| Beetle Saturn | Saturn | [libretro/beetle-saturn-libretro](https://github.com/libretro/beetle-saturn-libretro) |
| Flycast | Dreamcast | [flyinghead/flycast](https://github.com/flyinghead/flycast) |
| SwanStation | PlayStation | [libretro/swanstation](https://github.com/libretro/swanstation) |
| PPSSPP | PSP | [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp) |
| Beetle PCE / SuperGrafx | TurboGrafx-16, SuperGrafx | [libretro/beetle-pce-libretro](https://github.com/libretro/beetle-pce-libretro) |
| Beetle WonderSwan / NGP / VB | WonderSwan, Neo Geo Pocket, Virtual Boy | [Mednafen](https://mednafen.github.io/), via libretro |
| Stella (stella2014) | Atari 2600 | [stella-emu/stella](https://github.com/stella-emu/stella) |
| a5200 | Atari 5200 | [libretro/a5200](https://github.com/libretro/a5200) |
| ProSystem | Atari 7800 | [libretro/prosystem-libretro](https://github.com/libretro/prosystem-libretro) |
| Atari800 | Atari 8-bit | [atari800/atari800](https://github.com/atari800/atari800) |
| Handy | Atari Lynx | [libretro/libretro-handy](https://github.com/libretro/libretro-handy) |
| Virtual Jaguar | Atari Jaguar | [libretro/virtualjaguar-libretro](https://github.com/libretro/virtualjaguar-libretro) |
| VICE (x64) | Commodore 64 | [VICE](https://vice-emu.sourceforge.io/), via libretro |
| PUAE | Amiga | [libretro/libretro-uae](https://github.com/libretro/libretro-uae) |
| blueMSX | MSX | [libretro/blueMSX-libretro](https://github.com/libretro/blueMSX-libretro) |
| DOSBox Pure | DOS | [schellingb/dosbox-pure](https://github.com/schellingb/dosbox-pure) |
| Opera | 3DO | [libretro/opera-libretro](https://github.com/libretro/opera-libretro) |
| FBNeo | Arcade / Neo Geo | [libretro/FBNeo](https://github.com/libretro/FBNeo) |
| MAME | Arcade, and the machines nothing else runs | [mamedev/mame](https://github.com/mamedev/mame) |

## Everything else Cartload stands on

- **[libretro / RetroArch](https://www.libretro.com/)** — the API that makes one frontend
  able to talk to thirty emulators, the buildbot that publishes the Windows builds, and
  `libretro.h` itself, which is included here unmodified under its own MIT license
  (© 2010-2024 The RetroArch team).
- **[SDL2](https://www.libsdl.org/)** and **SDL_ttf** (with **FreeType**) — window, input,
  audio, and text.
- **[7-Zip](https://www.7-zip.org/)** — every `.zip` and `.7z` in your library is opened by
  Igor Pavlov's tool, not by anything in here.

## And the rest

Emulator authors, the dumping and preservation scene, and the people who wrote the
documentation that made all of the above possible. Cartload is a few thousand lines of
menu code sitting on top of that.
