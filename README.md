# NextGen Disk Cache 2.0.0

A conservative SKSE64 derivative of **Disk Cache Enabler** by **Archost** (original Nexus upload by **enpinion**).

Skyrim opens its `.bsa`/`.ba2` archives with `FILE_FLAG_NO_BUFFERING`, which tells Windows *not* to keep those reads in the system file cache. On a machine with RAM to spare that is usually the wrong trade. This plugin removes that flag from archive reads so Windows can cache them — and, by default, does nothing else.

## Version numbering

**2.0.0 unifies the numbering.** Earlier Nexus uploads (1.1.2, 1.1.3) used a different scheme from the repository tags (1.3.0, 1.4.x), which made bug reports impossible to map to a commit. From here the DLL version, the git tag, and the Nexus file version are always the same number.

| Old Nexus file | Was actually |
|---|---|
| 1.1.2 "Experimental" | repo 1.3.0 (warm cache and hardware tuning on) |
| 1.1.3 "Safe" | a narrow IAT build whose source was never published |

If you compared those two and preferred 1.1.2, you were comparing a fully-featured build against a nearly-inert one. 2.0.0 lets you pick either behaviour from one installer.

## Choose a build (one FOMOD, three options)

### Safe — recommended
Hooks **only** the `CreateFileA`/`CreateFileW` entries in `SkyrimSE.exe`'s own import table. Other SKSE plugins have their own import tables, so their file operations never reach this mod at all — verified by test, not asserted.

Rewrites only opens that are **all** of: an existing `.bsa`/`.ba2`, read-only, synchronous, `OPEN_EXISTING`. For those it removes `FILE_FLAG_NO_BUFFERING` and applies the archive-only `FILE_FLAG_RANDOM_ACCESS` hint.

### Minimal — troubleshooting
Safe, minus the random-access hint. Strips `FILE_FLAG_NO_BUFFERING` from archive reads and does nothing else. Use it to isolate a compatibility problem or if you want the smallest possible change.

### Experimental — unproven, benchmark it
Safe plus three opt-in changes, none of them demonstrated to help:

- **Process-wide hook** (Detours) instead of the executable's import table. File opens from *other* SKSE plugins pass through this mod's policy. The policy declines nearly all of them, but this is a real compatibility surface — it is the reason Safe exists.
- **Bounded warm cache**: 512 MB total, 8 MB per archive, one low-priority thread, starting 60 s after load. It cannot know which archives your save will need, and background disk activity is a plausible microstutter cause.
- **EcoQoS power throttling disabled** for the Skyrim process.

If you cannot measure a difference against Safe on your own save and route, use Safe.

## Never touched, in any profile

Saves (`.ess`), SKSE cosaves, backups, journals and SQLite state files, logs, temp files, plugins, loose meshes/textures/audio, config files, and unknown extensions. Nor any handle opened for write or delete, or with `OVERLAPPED`, `WRITE_THROUGH`, `DELETE_ON_CLOSE`, or a create/truncate disposition. The plugin does not parse or write save data.

A policy self-test runs before the hooks are installed and disables them if any of those invariants fail. It is verified to survive optimization — an earlier release shipped a self-test the compiler had folded into an unconditional success.

## What it will not do

It cannot enable 3D V-Cache, Resizable BAR, XMP/EXPO, or an SSD's onboard DRAM cache — those are silicon, BIOS, or firmware. `bRaiseMemoryPriority` is present for completeness but ships **off**: it sets `MEMORY_PRIORITY_NORMAL`, which Microsoft documents as already the default, so it is a no-op.

DirectStorage is bundled only as an optional FOMOD choice and is disabled in every shipped profile. Its raw-read path fills scratch memory and discards it, which does not populate the Windows cache Skyrim reads from.

## Verifying this build yourself

Every release is built by GitHub Actions from the tagged commit, and the workflow, hashes, and PDB are published with it. To check a DLL you already have:

```
dumpbin /exports NextGenDiskCache.dll   →  only SKSEPlugin_Load/Query/Version
dumpbin /imports NextGenDiskCache.dll   →  SETUPAPI, KERNEL32, SHELL32 only
```

MSVC output is not bit-identical across compiler versions, so a local build with a different Visual Studio release yields a different hash but identical imports, exports and behaviour.

The log at `Documents\My Games\Skyrim Special Edition\SKSE\NextGenDiskCache.log` states the active hook scope on every launch. Set `bLogEveryOpen=1` briefly to see exactly which handles are and are not modified.

## Build

```powershell
.\build.ps1          # fetches the DirectStorage SDK, builds Release x64
.\package-release.ps1 # produces the Nexus/Vortex FOMOD zip + SHA-256
```

## Credits and license

- **Archost** — original Disk Cache Enabler (ISC)
- **enpinion** — original Nexus upload
- Microsoft Detours (MIT), SKSE64 plugin headers

This derivative retains Archost's copyright and ISC notice.
