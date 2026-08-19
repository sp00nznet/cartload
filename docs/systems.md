# What runs, and how a rom finds its machine

## Confirmed, picture on screen

Every row here was run through `--selftest` against a rom from a real library and the
resulting frame **looked at**, not just counted. Exit codes lie: a core's own error screen
and a fully transparent frame both pass a naive check, and both did at one point.

| System | Core | Verified with |
| --- | --- | --- |
| Super Nintendo | snes9x | `Super Mario World` hack, from a `.zip` |
| Nintendo NES | nestopia | `10-Yard Fight`, from a `.zip` |
| Nintendo 64 | mupen64plus-next | `Super Mario 64` and `Ocarina of Time`, loose `.z64`, hardware GLideN64 |
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

## Not working yet

| System | Why |
| --- | --- |
| Neo Geo / Arcade (fbneo) | The romsets tested are for a different FBNeo revision — it reports the game as known but every CRC as wrong. Use MAME for these, or re-dat the sets. |
| Dreamcast, Sega CD, 3DO, PlayStation, PSP, Saturn | Need a BIOS the test machine does not have. 3DO says so outright (`no BIOS ROM found`); PSP gets as far as creating 480x272 framebuffers and then crashes. |
| Everything else in the table | Core is installed and the rom resolves, but no game from that system has been run yet. |

## Known gaps

- **Vertical arcade games display sideways.** `SET_ROTATION` is not honoured, so Pac-Man
  and every other upright cabinet renders on its side.
- PS3, Xbox 360, PS Vita and N-Gage folders in a library are ignored on purpose — nothing
  in libretro runs them.

## Which core runs what

`systems.c` is the whole table — display name, core dll, extensions, and the folder names
that mean that machine. Adding a system is one row, plus the core (tap it on the **Cores**
screen, or re-run `getcores.ps1`).

Extension alone cannot decide: most of a real collection is `.zip`, `.7z`, `.chd`, `.bin`
or `.iso`, none of which name a machine. So **the folder decides first** and the extension
only breaks the tie when it belongs to exactly one system. Folder names are matched with
punctuation and case thrown away, so `Nintendo SNES`, `Super_Nintendo` and `snes` are all
the same thing. That is why a library organised one-directory-per-system just works.

MAME is the catch-all at the bottom of the table: the machines nothing else emulates —
Apple II, the Macs, the TI — resolve to it.

## BIOS files

Some systems need a BIOS the core cannot ship: Sega CD, Saturn, Dreamcast, PlayStation,
3DO, PSP and the MAME machines all want one. Drop those in `%APPDATA%\Cartload\system\`
under the names the core expects.
