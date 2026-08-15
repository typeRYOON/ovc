# osu! Version Control

Version history for osu! beatmaps. A small desktop tracker that snapshots your mapset every
time you save, and a web viewer that diffs and restores that history **semantically**, it
parses `.osu`, so instead of "line 412 changed" you get *"OD 8 → 9," "+3 notes at 01:24,"
"slider reshaped."*

- **Viewer:** <https://ryoon.moe/ovc>
- **Write-up:** <https://ryoon.moe/blog/ovc>
- **YouTube Explanation:** <https://www.youtube.com/watch?v=bQdShXsNiWQ>

> Windows only for now. Reads osu! **stable**; lazer works through a staging folder (see the write-up).

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

## License

Licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0). Qt is used under the
LGPL via dynamic linking, and libgit2 is GPLv2 with a linking exception, so neither constrains this
choice.

## Credits

[Qt](https://www.qt.io/) · [libgit2](https://libgit2.org/) · [miniz](https://github.com/richgel999/miniz)
