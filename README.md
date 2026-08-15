# ovc — osu! Version Control

Version history for osu! beatmaps. A small desktop tracker that snapshots your mapset every
time you save, and a web viewer that diffs and restores that history **semantically** — it
parses `.osu`, so instead of "line 412 changed" you get *"OD 8 → 9," "+3 notes at 01:24,"
"slider reshaped."*

- **Viewer:** <https://ryoon.moe/ovc>
- **Write-up:** <https://ryoon.moe/blog/ovc>

> Windows only for now. Reads osu! **stable**; lazer works through a staging folder (see the write-up).

## What it does

- **Auto-snapshots** every save of the map you have open in the editor (detected from the client).
- **Semantic diffs** — notes, timing, SV, hitsounds, slider shape/length, metadata, breaks, storyboard.
- **Timeline viewer** — scrub any past version with audio and hitsounds, all four modes.
- **Restore** any snapshot back into your Songs folder (your current state is snapshotted first).
- **Collab merges** — exchange `.ovcz` bundles; a per-object three-way merge, with a visual conflict
  resolver on the timeline for the rare collision.

The shadow history lives entirely in `%LOCALAPPDATA%\ovc\`, never inside your Songs folder, and the
desktop app is the only thing that ever writes files.

## Install

Grab the latest `ovc-<version>-win-x64.zip` from [Releases](../../releases), unzip it anywhere, and
run `ovc.exe`. It's self-contained (Qt + MSVC runtime bundled) — no installer, no redist.

## Build from source

Requires **Visual Studio 2022** (Desktop C++), **Qt 6.11** (`msvc2022_64`), and **CMake ≥ 3.21**.
`libgit2` is pulled in automatically by vcpkg on the first configure.

```bat
build.bat Release-x64
```

`build.bat` sets up the MSVC environment itself, configures via the CMake preset, builds, and runs
the test suite. Binaries land in `out\build\release\`.

## Packaging a release

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dist-win.ps1
```

Builds Release, stages `ovc.exe` + `ovc-cli.exe` with their Qt/MSVC runtime via `windeployqt`, and
writes `dist\ovc-<version>-win-x64.zip`. Pass `-SkipBuild` to package an existing build, or
`-QtBin <path>` if Qt isn't at the default location.

## How it works

Each tracked set gets its own shadow git repo (via [libgit2](https://libgit2.org/)), kept separate
from your Songs folder — ovc mirrors your files in on save and commits. Diffs and 3-way merges run on
a canonicalized parse of the `.osu`, so they understand the map rather than the bytes. The merge base
is found by shared **content** (blob hash), not shared commits, so collabs seeded from a plain `.osz`
merge cleanly even across an upload.

## License

**TBD.** (Qt is used under the LGPL via dynamic linking; libgit2 is GPLv2 with a linking exception, so
it doesn't dictate the project license.)

## Credits

[Qt](https://www.qt.io/) · [libgit2](https://libgit2.org/) · [miniz](https://github.com/richgel999/miniz)
