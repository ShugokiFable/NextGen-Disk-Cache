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

## 1.3.0 - manual game-drive override

RAID arrays, Storage Spaces, and some USB enclosures report a generic bus type, so the game drive can be misclassified — a striped NVMe RAID0, for example, looks like a SATA SSD (or even an HDD if the controller reports a seek penalty). New optional INI setting `iGameDriveClass` under `[Hardware]`: `0` auto-detect (default), `1` HDD, `2` SATA SSD, `3` NVMe SSD. It steers only the warm-cache auto-tune (budget cap, per-file MB, reader threads); the CreateFile cache policy is identical for every drive class. Check the log's `Game drive:` line first — most users should leave this at 0.

## 1.2.1 - save/cosave compatibility hotfix

Save-related files (`.ess`, `.bak`, `.skse`, `.cosave`) were classified as ordinary game assets: their `FILE_FLAG_NO_BUFFERING` was stripped and `FILE_FLAG_RANDOM_ACCESS` was forced on their handles. Durability-focused writers such as S.L.A.C.K. (Save & Load Accelerator for SKSE Cosaves) deliberately open cosaves unbuffered and write-through. Save games, cosaves, logs, and temp files now keep their caller-selected flags exactly; the cache policy (no-buffering strip + random-access preference) applies only to real game assets.

## 1.2.0 - high-end scheduler and storage path

Version 1.2 keeps the mod's high-end-PC focus while removing placebo behavior:

- High-end auto-tune is the default profile. NVMe systems can use up to eight low-priority workers and larger adaptive archive windows, with a hard free-RAM reserve.
- Large archives use strided `PrefetchVirtualMemory` windows across the file rather than warming only the first block.
- Warm workers can be placed on the non-V-Cache CCD of a dual-CCD X3D processor, or on efficiency-class cores of a hybrid CPU. The Skyrim process itself is not pinned by default.
- Working-set expansion is disabled by default. Skyrim's process working set is not the Windows system file cache.
- DirectStorage 1.3 is dynamically detected. An actual file-to-memory backend exists, but remains experimental and off by default because raw DirectStorage reads are not equivalent to populating the normal Windows page cache.
- Initialization, Detours attachment, logging, and thread creation are moved out of `DllMain` to avoid doing complex work under the Windows loader lock.
- Release builds no longer put PDBs in the public mod directory. CI stores symbols as a separate artifact and creates the FOMOD ZIP.

X3D cache, ReBAR/SAM, XMP/EXPO, DDR5 timings, and an SSD's onboard DRAM are hardware or firmware features. The plugin can detect relevant topology and choose better software behavior, but it cannot switch those features on or manufacture a benefit where the Windows/Skyrim I/O path does not use them.


## 1.1.0 — modern hardware support

The plugin now builds a **hardware profile** at startup and adapts to it:

| Tech | What 1.1.0 does |
|------|-----------------|
| **AMD X3D (3D V-Cache)** | Detects via CPU brand + L3 topology. Logs it. On **dual-CCD** X3D parts, optional `bPreferFastCores=1` soft-prefers the V-Cache CCD via CPU sets (default off — Windows/chipset drivers usually handle it). |
| **Intel hybrid (P/E cores)** | Detected via CPU-set efficiency classes; same optional P-core preference. |
| **Resizable BAR** | Probes the GPU's largest PCI BAR (SetupAPI/cfgmgr32). ≥1 GB ⇒ **ACTIVE**, 256 MB window ⇒ tells you to enable ReBAR / Smart Access Memory in BIOS. Detection only — ReBAR itself is BIOS-level. |
| **DDR5 / DDR4 speed** | Reads SMBIOS (Type 17): DDR generation + running MT/s. Warns if DDR5 runs at JEDEC 4800 when XMP/EXPO is likely off. CAS (CL30) lives in SPD and is not readable from user mode — no fake numbers. |
| **NVMe / SATA SSD / HDD** | Seek-penalty + bus-type probe on the game drive. Warm cache auto-scales: NVMe gets 4 parallel readers + 64 MB/file, SATA 2 + 32 MB, HDD stays single-threaded sequential and capped at 256 MB total. |
| **SSD DRAM cache** | Warm reads prime the Windows file cache. They may also interact with an SSD's firmware-managed DRAM/SLC cache, but that cache is not directly controllable or generically detectable from this plugin. |
| **32–64 GB RAM builds** | Auto warm budget: 8 GB (64 GB+ RAM), 4 GB (32 GB), 1.5 GB (16 GB) — always capped at ¼ of *free* RAM at warm time. |
| **Win8+ prefetch** | Warm cache uses `MapViewOfFile` + `PrefetchVirtualMemory` (kernel-optimal I/O sizes) instead of a ReadFile loop, with automatic fallback. |

Everything is INI-switchable (`[Hardware]`, `[WarmCache]`). The profile is logged to
`NextGenDiskCache.log` so you can verify what was detected.

**Honesty note:** X3D, ReBAR, and XMP/EXPO are silicon/BIOS features. Software cannot
enable them — this plugin *detects* them, *adapts* its caching to them, and tells you
in the log when a BIOS switch looks like it's still off.

## Requirements

- [SKSE64](https://skse.silverlock.org/) matching your game version
- Visual Studio 2022 + CMake + NuGet (to build from source)

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
