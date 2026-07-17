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

## 1.1.0 — modern hardware support

The plugin now builds a **hardware profile** at startup and adapts to it:

| Tech | What 1.1.0 does |
|------|-----------------|
| **AMD X3D (3D V-Cache)** | Detects via CPU brand + L3 topology. Logs it. On **dual-CCD** X3D parts, optional `bPreferFastCores=1` soft-prefers the V-Cache CCD via CPU sets (default off — Windows/chipset drivers usually handle it). |
| **Intel hybrid (P/E cores)** | Detected via CPU-set efficiency classes; same optional P-core preference. |
| **Resizable BAR** | Probes the GPU's largest PCI BAR (SetupAPI/cfgmgr32). ≥1 GB ⇒ **ACTIVE**, 256 MB window ⇒ tells you to enable ReBAR / Smart Access Memory in BIOS. Detection only — ReBAR itself is BIOS-level. |
| **DDR5 / DDR4 speed** | Reads SMBIOS (Type 17): DDR generation + running MT/s. Warns if DDR5 runs at JEDEC 4800 when XMP/EXPO is likely off. CAS (CL30) lives in SPD and is not readable from user mode — no fake numbers. |
| **NVMe / SATA SSD / HDD** | Seek-penalty + bus-type probe on the game drive. Warm cache auto-scales: NVMe gets 4 parallel readers + 64 MB/file, SATA 2 + 32 MB, HDD stays single-threaded sequential and capped at 256 MB total. |
| **SSD DRAM cache** | Warm reads prime the OS page cache *and* the drive's own DRAM/SLC layer. Reads are tagged **IoPriorityHintLow** so the game always outranks them. |
| **32–64 GB RAM builds** | Auto warm budget: 8 GB (64 GB+ RAM), 4 GB (32 GB), 1.5 GB (16 GB) — always capped at ¼ of *free* RAM at warm time. |
| **Win8+ prefetch** | Warm cache uses `MapViewOfFile` + `PrefetchVirtualMemory` (kernel-optimal I/O sizes) instead of a ReadFile loop, with automatic fallback. |

Everything is INI-switchable (`[Hardware]`, `[WarmCache]`). The profile is logged to
`NextGenDiskCache.log` so you can verify what was detected.

**Honesty note:** X3D, ReBAR, and XMP/EXPO are silicon/BIOS features. Software cannot
enable them — this plugin *detects* them, *adapts* its caching to them, and tells you
in the log when a BIOS switch looks like it's still off.

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
