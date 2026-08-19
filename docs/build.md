# Building Cartload

Needs MSVC, CMake 3.21+, and vcpkg with `sdl2` and `sdl2-ttf`.

```
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Output, the SDL runtime DLLs and `cores\` land in `build\Release\`. Text uses the system
Segoe UI, so no font ships with the app.

## Single exe build

A static vcpkg triplet links SDL2, SDL2_ttf, freetype and the CRT in, so the result is one
self-contained `cartload.exe` with no VC++ redist needed on the target. The cores still ship
alongside — those are loaded at runtime by design.

```
vcpkg install sdl2:x64-windows-static sdl2-ttf:x64-windows-static
cmake -B build-static -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-static --config Release
```

## The version number

It lives in `project(cartload VERSION ...)` in `CMakeLists.txt` and nowhere else. CMake
generates `cartload.rc` from `cartload.rc.in`, and passes the same string to the code as
`CARTLOAD_VERSION`, so the exe's properties, the selftest log and the release tag cannot
drift apart. CI refuses to publish a `vX.Y.Z` tag that disagrees with it.

## CI

`.github/workflows/ci.yml` builds the static single-exe on every push and pull request,
runs `--selftest` on the result, and uploads the zip as an artifact. Pushing a `v*` tag
publishes that zip as a release. vcpkg's binary cache is what keeps a from-source SDL2
build off the critical path.

## The source

Six files, and every one of them is one job:

| File | What lives there |
| --- | --- |
| `main.c` | the screens, input, the scan, the save/state plumbing |
| `core.c` | the libretro side: load a dll, feed it a rom, run frames, GL context |
| `systems.c` | the table — machine, core, extensions, folder aliases, pad map |
| `archive.c` | 7-Zip, for the half of a library that arrives packed |
| `cartload.rc` | version resource (a static exe with none of it trips Defender's heuristics) |
| `libretro.h` | upstream, unmodified |

## Checking a build

```
cartload.exe --selftest [rom] [frames]
cartload.exe --selftest --fetch
cartload.exe --shots <romdir> [system] [row]
cartload.exe --shots <rom> [frames]
```

With no rom, `--selftest` inventories the shelf: every system, its core, and whether that
core is actually installed — the quickest way to see what can be played right now. Given a
rom it also works out which machine that is, loads the core, boots it, runs the frames and
checks the core's output came back through the render target.

Exit 0 pass, 2 no core, 3 the rom would not load, 4 nothing reached the render target.
`selftest.log` gets the core's own logging and, if it dies, the faulting module;
`selftest.bmp` gets the last frame, which is the quickest way to see whether a game is
actually drawing.

`--fetch` sends every missing core through the same download queue the Cores screen taps
into, headless, and logs each one — the check that the downloader still works after the
buildbot moves something.

`--shots` writes the screens to `shot-systems.bmp`, `shot-library.bmp`, `shot-cores.bmp`
and, given a rom, `shot-game.bmp` and `shot-menu.bmp`. That is where the pictures in the
README come from, and it is the only way to look at a layout on a machine nobody is
holding. The optional system name and row say which library to open and where to scroll
it.
