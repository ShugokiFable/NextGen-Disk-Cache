# NextGen Disk Cache 2.0.0

A conservative, configurable SKSE64 derivative of **Disk Cache Enabler** by **Archost** (original Nexus upload by **enpinion**).

**Safe is the recommended profile.**

> A conservative SKSE64 file-cache plugin that modifies only eligible read-only BSA/BA2 archive opens. Includes Safe, Minimal and clearly labelled Experimental profiles. Saves, plugins and loose assets are never modified. Performance gains are not guaranteed.

Nexus: [NextGen Disk Cache](https://www.nexusmods.com/skyrimspecialedition/mods/185563)  
Source: [github.com/ShugokiFable/NextGen-Disk-Cache](https://github.com/ShugokiFable/NextGen-Disk-Cache)

---

## What this mod actually does

NextGen Disk Cache changes the Windows file-caching flags used for a very limited group of Skyrim archive reads.

When Skyrim opens an eligible **`.bsa` or `.ba2` archive** with `FILE_FLAG_NO_BUFFERING`, the plugin can remove that flag so Windows is allowed to use its normal file cache.

An archive open is eligible only when it is all of the following:

- A **`.bsa` or `.ba2`** archive
- Opened for reading only
- Opened synchronously
- Using `OPEN_EXISTING`
- Not opened with write-through, overlapped, delete-on-close, create, or truncate behaviour

The recommended **Safe** profile limits interception to file-open calls made through **SkyrimSE.exe's own import table**. File operations originating from unrelated SKSE DLLs do not pass through the Safe hook.

---

## What this mod does not modify

Outside the modification policy in every supplied profile:

- Skyrim save files
- SKSE cosaves
- Save backups
- Journals and database state files
- ESP, ESM and ESL plugins
- Loose meshes, textures, and audio
- INI and JSON configuration files
- Logs, crash dumps and temporary files
- Unknown file extensions
- Any handle opened for writing or deletion
- Any create, truncate, overlapped, write-through or delete-on-close operation

The plugin does not parse, edit, or store information inside your save files.

---

## Installer profiles

The FOMOD contains three mutually exclusive profiles.

### 1. Safe — recommended

Normal-play profile.

- Hooks only the relevant imports belonging to SkyrimSE.exe
- Considers only eligible read-only synchronous BSA/BA2 opens
- Removes `FILE_FLAG_NO_BUFFERING`
- Applies `FILE_FLAG_RANDOM_ACCESS` to eligible archives
- Warm cache disabled
- Hardware profiling disabled
- Automatic tuning disabled
- Process-wide interception disabled
- Working-set expansion disabled
- Power-throttling changes disabled

The random-access flag is a Windows caching hint, not a guaranteed performance switch. It is used only for eligible archives because archive access may involve seeking between internal records.

### 2. Minimal — troubleshooting

Same narrow executable-only hook and the same safety gates as Safe.

It only removes `FILE_FLAG_NO_BUFFERING` from eligible archive reads. It does **not** add the random-access hint and does not enable any experimental process, hardware, or warm-cache features.

Choose Minimal when:

- You want the smallest possible behavioural change
- You are investigating a compatibility concern
- You want to compare stripping NO_BUFFERING alone against Safe

### 3. Experimental — unproven

Includes the archive policy used by Safe but also enables features that have **not been demonstrated to improve Skyrim performance**.

It enables:

- **Process-wide Detours interception** rather than the narrow SkyrimSE.exe import hook
- **Bounded speculative warm cache**
- **Hardware profiling and automatic warmer reduction**
- **EcoQoS execution-speed throttling disabled** for the Skyrim process

Supplied warm-cache limits:

- 512 MB maximum total read budget
- 8 MB maximum per archive
- 128 archives maximum
- One low-priority worker thread
- 60-second startup delay

The warmer cannot know which archives your current save will need. Background reads may provide no benefit and may introduce additional disk activity or microstutter.

Automatic tuning can only reduce the configured warmer limits. It does not scale the warmer beyond the values listed above.

**Benchmark Experimental against Safe on the same save, route and conditions. If you cannot measure a repeatable improvement, use Safe.**

---

## DirectStorage status

The Microsoft DirectStorage runtime is **not included** in the public release package (Nexus or the GitHub FOMOD zip).

DirectStorage probing and DirectStorage warm reads remain disabled in every supplied profile.

This release makes no DirectStorage performance claim.

(The source tree may still compile optional DirectStorage backend code for development; it is not shipped and is not enabled by any profile.)

---

## Performance and testing disclosure

This mod changes file-open caching policy. It does not guarantee:

- Higher FPS
- Faster loading screens
- Elimination of traversal stutter
- Lower memory usage
- Better performance on every storage device or mod list

Results may vary depending on storage hardware, available system memory, Windows file-cache state, archive layout, mod-list composition, and the save and route being tested.

Version 2.0.0 has source-level validation and standalone hook-policy testing.

At the time of publication:

- No public comparative in-game performance benchmark has been completed
- No complete Skyrim runtime compatibility matrix has been completed
- No universal compatibility claim is made for specific mod lists or SKSE plugins

Any performance improvement should be treated as a possibility to test, not a promised outcome.

---

## Requirements

- Skyrim Special Edition or Anniversary Edition on 64-bit Windows
- SKSE64

Do not install together with the original Disk Cache Enabler DLL, another version of NextGen Disk Cache, or any duplicate `NextGenDiskCache.dll`.

---

## Installation

Install with Vortex or Mod Organizer 2.

1. Choose **Safe** for normal play
2. Choose **Minimal** only for troubleshooting or controlled comparison
3. Choose **Experimental** only when you intend to benchmark it

The installer places `NextGenDiskCache.dll` and the selected `NextGenDiskCache.ini` in `Data\SKSE\Plugins`. Launch Skyrim through SKSE64.

---

## Log file

`Documents\My Games\Skyrim Special Edition\SKSE\NextGenDiskCache.log`

The log reports the selected hook scope and whether the hook was installed.

For temporary diagnostics, set `bLogEveryOpen=1` in the INI. This may produce a large amount of logging; return it to `0` after testing.

When reporting an issue, include the complete log, selected installer profile, Skyrim executable version, SKSE version, storage type, and whether the issue occurs without this plugin.

---

## Updating or removing

Only one copy of the DLL and INI should be installed. When changing profiles, reinstall the FOMOD and replace the previous INI rather than merging settings manually.

The plugin does not store data in Skyrim saves. To remove it, uninstall the mod and confirm that `Data\SKSE\Plugins\NextGenDiskCache.dll` is no longer present.

---

## Version history

### 2.0.0

- Unified Nexus, DLL and GitHub version numbering
- Added Safe, Minimal and Experimental installer profiles
- Made the narrow SkyrimSE.exe import-table hook the recommended default
- Restricted modifications to eligible read-only synchronous BSA/BA2 archive opens
- Saves, cosaves, plugins, loose assets, configuration files and unknown extensions remain untouched
- Added a Minimal profile without the random-access hint
- Kept the process-wide hook and speculative warmer inside the clearly labelled Experimental profile
- Warm cache, hardware profiling and automatic tuning remain disabled in Safe and Minimal
- Removed the DirectStorage runtime from the public package
- Added runtime policy validation and corrected diagnostic counters
- Restored all required license and attribution files
- Rewrote performance and compatibility statements to reflect the current testing status

### Legacy version mapping

- **Nexus 1.1.2** was an unsupported experimental build corresponding approximately to repository version 1.3.0
- **Nexus 1.1.3** was an unsupported narrow safety build whose exact source was not published

Detailed developer notes for pre-2.0.0 work remain in `CHANGELOG.txt`.

---

## AI-assisted development disclosure

AI tools assisted portions of development, auditing and documentation. AI assistance does not replace verification; this description is limited to behaviour represented by the published source, distributed configuration and current testing record. No performance result is presented as proven without supporting testing.

---

## Verifying a build

Every public release is built by GitHub Actions from the tagged commit when published. The PDB is **not** inside the FOMOD zip; symbols are published separately as CI artifacts when available.

```
dumpbin /exports NextGenDiskCache.dll   →  only SKSEPlugin_Load/Query/Version
dumpbin /imports NextGenDiskCache.dll   →  SETUPAPI, KERNEL32, SHELL32 only
```

MSVC output is not bit-identical across compiler versions; local builds may differ in hash while matching imports, exports and behaviour.

---

## Build

```powershell
.\build.ps1            # builds Release x64 (may fetch DirectStorage SDK headers for compile only)
.\package-release.ps1  # produces the public FOMOD zip + SHA-256 (no DirectStorage runtime)
```

---

## Credits and licensing

- **Archost** — original Disk Cache Enabler and ISC-licensed source
- **enpinion** — original Nexus upload
- **Microsoft** — Detours
- **SKSE Team** — SKSE64 plugin interface and notices

This project is a modified derivative of Disk Cache Enabler. The original Archost copyright and ISC permission notice are included in the archive, along with the project license, Microsoft Detours license and SKSE64 notice.
