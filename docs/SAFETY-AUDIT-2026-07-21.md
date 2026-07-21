# NextGen Disk Cache safety audit

**Audit date:** 2026-07-21  
**Audited package:** `NextGen Disk Cache.zip`  
**Compared against:** `Disk-Cache-Enabler-master.zip` and `DiskCacheEnabler-1.2.zip`  
**Audited source version:** 1.3.1  
**Prepared safety rework:** 1.4.0 source-only

## Verdict

NextGen Disk Cache 1.3.1 is substantially safer than the abandoned original, but I would not ship its current default profile as a normal long-term modlist component.

The direct risk of the plugin corrupting save contents is **low** because it does not parse or write Skyrim saves, and 1.3.1 already leaves write/delete, overlapped, write-through, save, cosave, log, and temporary-file opens untouched. However, the process-wide hook still modifies unknown read-only synchronous file opens, so compatibility risk remains for unrelated SKSE DLLs. The aggressive default warmer is also a credible cause of the reported gameplay stutter.

The 1.4.0 safety rework closes the identified policy gaps in source, but it is **not a release binary yet**. It still requires a Windows/MSVC build, CI validation, and in-game testing before public distribution.

## What changed from the abandoned original

The original `DiskCacheEnabler` hooks `CreateFileA` from `DllMain` and rewrites flags for every intercepted open. It removes `FILE_FLAG_NO_BUFFERING` and `FILE_FLAG_SEQUENTIAL_SCAN`, then adds `FILE_FLAG_RANDOM_ACCESS`, without classifying paths or protecting save/state operations.

NextGen 1.3.1 improves this substantially:

- hook setup occurs through the SKSE load path rather than doing Detours work directly in `DllMain`;
- both `CreateFileA` and `CreateFileW` are covered;
- saves, cosaves, logs, and temporary files retain their original flags;
- write/delete intent, overlapped I/O, and write-through I/O retain their original flags;
- optional warm-cache and DirectStorage logic are separated from normal file opens;
- shutdown avoids loader-lock file I/O.

## Findings against the four public concerns

### 1. Hook scope

**Concern is valid for 1.3.1, but only for read-only synchronous opens.**

Version 1.3.1 no longer modifies write-capable, overlapped, write-through, save, cosave, log, or temporary handles. That part of the criticism is outdated. However, unknown extensions opened synchronously for read-only access can still lose `FILE_FLAG_NO_BUFFERING`, because the strip occurs outside the known-asset classification gate.

This remains a real compatibility category because the hook covers the entire Skyrim process, including file opens made by unrelated SKSE plugins.

### 2. Random-access classification

**Concern is valid.**

Version 1.3.1 applies `FILE_FLAG_RANDOM_ACCESS` to a broad group that includes archives, plugins, loose meshes/textures, streamed audio, scripts, configuration files, JSON, BIN/DAT, and database files. The Windows flags are caching hints, not universal speed switches. Archive containers are the strongest case for random access; audio and small configuration/state files are much weaker cases.

### 3. Warm cache

**Concern is valid and is the most plausible source of reported stutter.**

The uploaded 1.3.1 high-end profile enables the warmer by default after 12 seconds. It can scan hundreds of root-level archives/plugins, choose large files first, use multiple readers, and let auto-tuning expand the total budget to several gigabytes on high-memory systems. The warmer cannot know which archives the active save will use, and it ignores recursive loose assets.

That can create a concentrated disk, memory-mapping, page-fault, decompression-adjacent, and standby-list workload shortly after game startup. Low I/O priority reduces contention but does not make the work free.

### 4. External validation

**Concern is valid, with one correction.**

The uploaded package does contain prebuilt DLLs for versions 1.0.0 through 1.3.1. The statement that the package has no prebuilt DLL is false for the archive supplied for this audit.

The broader criticism remains: there are no public controlled frametime, loading-time, disk-I/O, memory-pressure, save-cycle, or large-modlist compatibility results sufficient to call the plugin validated. Static source review cannot replace those tests.

## Binary hygiene review of the uploaded 1.3.1 DLL

**DLL SHA-256:** `7e3708bb80c8e59d590a94c4588603ae4426bd7541f3bb943b9eb80a5f5971c3`

Observed properties:

- PE32+ x86-64 DLL with nine ordinary sections;
- exports only `SKSEPlugin_Load`, `SKSEPlugin_Query`, and `SKSEPlugin_Version`;
- static imports are limited to `SETUPAPI.dll`, `KERNEL32.dll`, and `SHELL32.dll`;
- no static WinHTTP, WinINet, Winsock, registry-write, process-spawn, or shell-execution imports were found;
- no Authenticode signature is present;
- section entropy and layout do not look like a conventional packed payload.

This is not proof that a binary is harmless. It is a positive static-hygiene result consistent with the inspected source.

## 1.4.0 safety rework

The supplied source rework changes the default policy to:

- modify only `.bsa` and `.ba2` opens;
- require `OPEN_EXISTING`;
- require no write/delete intent;
- reject overlapped, write-through, and delete-on-close handles;
- never modify unknown extensions;
- leave plugins, loose assets, audio, INI/JSON, saves, state databases, logs, and temporary files untouched by default;
- apply `FILE_FLAG_RANDOM_ACCESS` only to BSA/BA2 archives;
- run an internal policy self-test before attaching hooks;
- disable warm cache and DirectStorage by default;
- disable hardware probing and process-wide power/memory hints by default;
- parse invalid booleans and numbers fail-closed, preserving the current safe value;
- clamp all bounded numeric settings;
- limit the opt-in warmer to BSA/BA2 archives, at most two workers in code, and one worker in the supplied experimental profile;
- prevent auto-tune from expanding the configured warm-cache budget;
- ship an experimental profile capped at 512 MB, 8 MB per archive, one low-priority thread, and a 60-second delay.

## Validation completed

- source/package validator: **PASS**;
- staged package INI is verified byte-for-byte against `SafeDefault`;
- version and FOMOD metadata checks: **PASS**;
- prohibited generated-file checks: **PASS**;
- internal source tokens for archive-only scope, creation/write/async guards, policy self-test, and bounded warmer: **PASS**;
- manual comparison against the original and 1.3.1 source: completed;
- static binary hygiene check of uploaded 1.3.1 DLL: completed.

## Validation still required before release

1. Build 1.4.0 with the repository's Windows/MSVC workflow.
2. Confirm the DLL loads on Skyrim 1.5.97, 1.6.640, and 1.6.659 GOG.
3. Run at least one large-modlist compatibility session with `bLogEveryOpen=1` briefly, then confirm unknown extensions and save/cosave paths are unchanged.
4. Run save-cycle testing with manual saves, quick saves, autosaves, RaceMenu cosaves, and S.L.A.C.K. cosaves where applicable.
5. Compare disabled-plugin, SafeDefault, and ExperimentalWarmCache runs using the same save and route.
6. Record frametimes, loading times, disk active time, read throughput, hard faults, commit charge, and available memory.
7. Test both cold-cache and already-warm-cache starts. Do not mix them in one average.
8. Verify clean detach/shutdown and repeated game launches.

## Immediate recommendation for 1.3.1 users

Do not use the uploaded high-end default while investigating stutter. Disable the warmer and DirectStorage probe immediately. For the strictest temporary safety, disable the file hooks as well or uninstall 1.3.1 until a compiled 1.4.0 build is tested.

The included `NextGenDiskCache-1.3.1-Emergency-Disable.ini` is intentionally a no-op safety configuration. It keeps logging available but disables the hook, process hints, hardware probing, warm cache, and DirectStorage.
