# NextGen Disk Cache

SKSE plugin for **Skyrim Special Edition / Anniversary Edition** that helps the game use the **Windows file cache** properly.

This is a **modified derivative** of [Disk Cache Enabler](https://www.nexusmods.com/skyrimspecialedition/mods/100975).

| Role | Name |
|------|------|
| **Original created by** | **Archost** |
| **Original uploaded by** | **enpinion** |
| **License** | **ISC** — Copyright 2023 Archost (see `LICENSE.txt`) |

## What it does (plain English)

**Legacy Disk Cache Enabler** stops Skyrim from opening files in a way that *bypasses* Windows’ normal disk cache.

This project keeps that idea and adds:

- Hooks **CreateFileA and CreateFileW**
- Smarter handling for game assets vs log files
- Optional background warm-read of large archives after launch
- Optional process power-throttling / working-set hints
- INI config + log file

**Remove** the old `diskCacheEnabler.dll` if you install this — don’t run both.

## Requirements

- [SKSE64](https://skse.silverlock.org/) matching your game version
- Visual Studio 2022/2026 + CMake (to build from source)

No Address Library required (signature / no-struct plugin).

## Build (Windows)

```powershell
cd source   # if you cloned the whole tree, use the repo root
.\build.ps1
```

Or manually:

```powershell
# From a VS x64 developer environment:
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output DLL path is set in `CMakeLists.txt` (versioned package folder next to this tree when building in the local workspace). For a standalone clone, change `OUT_DIR` in `CMakeLists.txt` to e.g. `package/SKSE/Plugins`.

## Install (prebuilt)

Ship layout:

```
Data/SKSE/Plugins/NextGenDiskCache.dll
Data/SKSE/Plugins/NextGenDiskCache.ini
```

Log (after launch via SKSE):

`Documents\My Games\Skyrim Special Edition\SKSE\NextGenDiskCache.log`

## Credits

- **Archost** — original Disk Cache Enabler
- **enpinion** — Nexus upload of the original
- **Microsoft Detours** — API hooking (`LICENSE.detours.txt`)
- **SKSE team** — plugin API headers (`LICENSE.SKSE64.txt`)

## License

ISC License — Copyright 2023 Archost. Full text in `LICENSE.txt`.  
This derivative retains that notice on all copies as required by ISC.
