<p align="center">
  <img src="docs/images/logo.svg" width="72" height="72" alt="NextGen Disk Cache mark">
</p>

<h1 align="center">NextGen Disk Cache</h1>

<p align="center"><strong>Let Windows cache eligible Skyrim archives. Safe is the default.</strong></p>

<p align="center">
  Conservative SKSE64 derivative of Disk Cache Enabler (Archost / enpinion, ISC).<br>
  Only eligible read-only BSA/BA2 opens. Performance gains are not guaranteed.
</p>

<p align="center">
  <a href="https://github.com/ShugokiFable/NextGen-Disk-Cache/actions/workflows/build-release.yml"><img src="https://github.com/ShugokiFable/NextGen-Disk-Cache/actions/workflows/build-release.yml/badge.svg" alt="Build"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-ISC-e8b86d?labelColor=0d0f11" alt="ISC License"></a>
  <a href="https://github.com/ShugokiFable/NextGen-Disk-Cache/releases/tag/v2.1.0"><img src="https://img.shields.io/badge/release-v2.1.0-e8b86d?labelColor=0d0f11" alt="v2.1.0"></a>
  <img src="https://img.shields.io/badge/SKSE-SE%20%2F%20AE-8f9aa6?labelColor=0d0f11" alt="SKSE SE/AE">
</p>

<p align="center">
  <a href="#install">Install</a>
  ·
  <a href="#installer-profiles">Profiles</a>
  ·
  <a href="#build">Build</a>
  ·
  <a href="#honest-status">Honest status</a>
  ·
  <a href="https://www.nexusmods.com/skyrimspecialedition/mods/185563">Nexus</a>
</p>

## Why it exists

When Skyrim opens an eligible **`.bsa` or `.ba2`** with `FILE_FLAG_NO_BUFFERING`, Windows cannot use its normal file cache. This plugin can strip that flag on a narrow set of read-only archive opens.

That is the job. It does not rewrite saves, plugins, or loose assets.

## What it actually does

An archive open is eligible only when it is all of:

- A **`.bsa` or `.ba2`** archive
- Opened for reading only
- Opened synchronously
- Using `OPEN_EXISTING`
- Not opened with write-through, overlapped, delete-on-close, create, or truncate behaviour

The recommended **Safe** profile hooks only the relevant imports on **SkyrimSE.exe**. File operations from unrelated SKSE DLLs do not pass through that hook.

If your runtime never sets `FILE_FLAG_NO_BUFFERING`, Safe correctly does nothing, and the log says so.

## What it does not modify

Outside the modification policy in every supplied profile:

- Skyrim save files, SKSE cosaves, save backups
- Journals and database state files
- ESP, ESM, and ESL plugins
- Loose meshes, textures, and audio
- INI and JSON configuration files
- Logs, crash dumps, and temporary files
- Unknown file extensions
- Any handle opened for writing or deletion
- Any create, truncate, overlapped, write-through, or delete-on-close operation

The plugin does not parse, edit, or store information inside your save files.

## Installer profiles

The FOMOD contains three mutually exclusive profiles.

| Profile | Hook | `FILE_FLAG_NO_BUFFERING` | `FILE_FLAG_RANDOM_ACCESS` | Everything else |
| --- | --- | --- | --- | --- |
| **Safe** (recommended) | SkyrimSE.exe import table | Strip if present | **Off** since 2.1.0 | Off |
| **Minimal** | Same as Safe | Same as Safe | Off | Quiet log; warm-cache budgets hard-zeroed |
| **Experimental** | Process-wide Detours | Strip if present | On (for A/B) | Warmer, profiling, EcoQoS off — **unproven** |

**Why random-access is off in Safe.** `FILE_FLAG_RANDOM_ACCESS` disables cache-manager read-ahead; the `FILE_FLAG_SEQUENTIAL_SCAN` hint it displaces enlarges it. A real 1.6.1170 session logged `opens=26162 patched=6481 no_buffering_stripped=0` — the engine never set `FILE_FLAG_NO_BUFFERING`, so the hint was the only live effect, applied thousands of times per minute, with no benchmark behind it. Set `bPreferRandomAccessOnArchives=1` yourself if you can measure a repeatable win, or install Experimental.

Experimental also enables:

- Bounded speculative warm cache (512 MB total, 8 MB per archive, 128 archives, one low-priority thread, 60 s delay)
- Hardware profiling and automatic warmer **reduction** (it does not scale beyond those caps)
- EcoQoS execution-speed throttling disabled for the Skyrim process

The warmer cannot know which archives the current save will need. **Benchmark Experimental against Safe on the same save, route, and conditions. If you cannot measure a repeatable improvement, use Safe.**

Since 2.1.0, Minimal is behaviourally identical to Safe except for logging and hard-zeroed warmer budgets.

## DirectStorage status

Microsoft DirectStorage runtime DLLs are **not** in the public Nexus or GitHub FOMOD zip.

The plugin still contains an optional, dynamically resolved DirectStorage backend compiled against SDK headers. Probing and DirectStorage warm reads stay **disabled in every supplied profile**. No DirectStorage performance claim. The SDK code-license notice is included because those headers are used at compile time.

## Requirements

- 64-bit Windows
- Skyrim Special Edition or Anniversary Edition with SKSE64

DLL metadata lists runtimes **1.5.97**, **1.6.640**, and **GOG 1.6.659**. Other versions are not claimed as tested. No complete runtime compatibility matrix has been published.

Do not install together with original Disk Cache Enabler, another NextGen Disk Cache, or a duplicate `NextGenDiskCache.dll`.

## Install

Install with Vortex or Mod Organizer 2 from [Releases](https://github.com/ShugokiFable/NextGen-Disk-Cache/releases) or [Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/185563).

1. Choose **Safe** for normal play.
2. Choose **Minimal** only for troubleshooting or a quiet install.
3. Choose **Experimental** only when you intend to benchmark it.

The installer places `NextGenDiskCache.dll` and the selected `NextGenDiskCache.ini` in `Data\SKSE\Plugins`. Launch through SKSE64.

Log (Steam): `Documents\My Games\Skyrim Special Edition\SKSE\NextGenDiskCache.log`  
Log (GOG): `Documents\My Games\Skyrim Special Edition GOG\SKSE\NextGenDiskCache.log`

For temporary diagnostics, `bLogEveryOpen=1` — then set it back to `0`. When reporting an issue, include the complete log, profile, Skyrim executable version, SKSE version, storage type, and whether it happens without this plugin.

Only one DLL/INI copy. Changing profiles: reinstall the FOMOD; do not merge INIs by hand. Uninstall by removing the mod and confirming `Data\SKSE\Plugins\NextGenDiskCache.dll` is gone. Saves are untouched.

## Build

```powershell
.\build.ps1            # Release x64 (may fetch DirectStorage SDK headers for compile only)
.\package-release.ps1  # public FOMOD zip + SHA-256 (no DirectStorage runtime)
```

The 2.1.0 plugin DLL in the public package is the GitHub Actions build from tag `v2.1.0`. Later compliance revisions update FOMOD docs, licensing, and package metadata only. The PDB is not inside the FOMOD zip.

```text
dumpbin /exports NextGenDiskCache.dll   →  only SKSEPlugin_Load/Query/Version
dumpbin /imports NextGenDiskCache.dll   →  SETUPAPI, KERNEL32, SHELL32 only
```

MSVC output is not bit-identical across compiler versions. Local builds may differ in hash while matching imports, exports, and behaviour.

## Project map

```text
src/                 SKSE plugin (IAT hook, DirectStorage backend stub)
profiles/            Safe / Minimal / Experimental INI
package/             staged SKSE/Plugins
fomod/               installer
deps/                Detours + DirectStorage SDK headers (compile)
tools/               package / validation helpers
LICENSE.txt          ISC (Archost Disk Cache Enabler derivative)
```

## Honest status

This mod changes file-open caching policy. It does not guarantee higher FPS, faster loading screens, less traversal stutter, or lower memory use.

Verified in this tree:

- Version **2.1.0**
- Safe / Minimal / Experimental profiles as documented
- Runtime file-policy self-test and source/package validation (policy invariants, not an in-game benchmark)
- DirectStorage backend compiled but disabled in every shipped profile

Not claimed:

- A public comparative in-game performance benchmark
- A complete Skyrim runtime compatibility matrix
- Universal compatibility with specific mod lists or SKSE plugins
- That Safe does anything on a runtime that never sets `FILE_FLAG_NO_BUFFERING`

AI tools assisted portions of development, auditing, and documentation. That does not replace verification. Behaviour here is limited to the published source, shipped INIs, and current testing record.

## Credits

- **Archost** — original Disk Cache Enabler and ISC-licensed source
- **enpinion** — original Nexus upload
- **Microsoft** — Detours and DirectStorage SDK headers
- **SKSE Team** — SKSE64 plugin interface and notices

This is a modified derivative of Disk Cache Enabler. The archive keeps Archost's copyright and ISC notice, this project's license, Microsoft Detours, the DirectStorage SDK code-license notice, and the SKSE64 notice.

## License

[ISC](LICENSE.txt) — original Disk Cache Enabler terms retained. See also `LICENSE.Archost-DiskCacheEnabler.txt`, `LICENSE.detours.txt`, `LICENSE.DirectStorage-Code.txt`, and `LICENSE.SKSE64.txt`.
