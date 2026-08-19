# Using Cartload

## The rom library

Tap **Rom folder** on the systems screen to pick your library with the normal Windows
folder picker — no keyboard needed. The choice is remembered in
`%APPDATA%\Cartload\romdir.txt`. You can also drop a folder onto the window, or pass one on
the command line. Failing all three, it looks in `roms\` next to the exe. Scans 6 levels
deep, which covers `set\system\letter\game`.

Mapped drives (`Z:\roms`) and UNC paths (`\nas\share\roms`) both work. If the drive isn't
up yet when Cartload starts, it keeps your library setting rather than silently falling
back, and says so — connect the drive and tap the message to rescan. Scanning runs on a
worker thread, so an unreachable share (~20s to time out) leaves the window responsive.

A library of 86,000 roms across 27 machines scans in about four seconds over SMB and
scrolls at 60fps, because the list is one flat array and only the visible rows ever become
textures.

## Cores

The **Cores** screen lists every machine in the table with the core that runs it and
whether that core is on this box. Tap a row to download it from the
[libretro nightly buildbot](https://buildbot.libretro.com/nightly/windows/x86_64/latest/);
tap an installed one to update it. Downloads queue one at a time and the row counts the
seconds, so a 250MB MAME core does not look like a hang.

`getcores.ps1` does the same thing from a desktop, which is still the way to fill `cores\`
before copying the folder to a handheld:

```
.\getcores.ps1              # fetch the missing ones
.\getcores.ps1 -Force       # re-fetch everything
```

A system whose core is missing simply says so when you try to play it. Nothing else
breaks.

## Archives

Roms are usually packed, so Cartload unpacks them. Cores that take content in memory get
the file from inside the archive directly; cores that insist on a real path (PPSSPP,
flycast, MAME, DOSBox) get it unpacked into `%APPDATA%\Cartload\tmp\`, kept there while you
play and cleared on exit.

This shells out to **7-Zip** rather than linking a decompressor — one small module covers
zip, 7z and rar. Install 7-Zip, or drop `7za.exe` next to `cartload.exe`. Without it, loose
roms still work and archives say what is missing.

## Saves

Cartridge saves, save states and core data live under `%APPDATA%\Cartload\`, split per
system, and are always written locally — never back to the rom share, so a dropped
connection mid-game can't corrupt them.

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
| In-game menu | left stick click | Esc, or a single tap |
| Back a screen | B | Esc |
| Jump a letter | LB / RB | — |
| Cores screen | X, on the systems screen | C |
| Rom folder picker | Y, on the systems screen | tap "Rom folder" |
| Save / load state | via the menu | F5 / F8 |
| Fullscreen | — | F11, or a double tap |
