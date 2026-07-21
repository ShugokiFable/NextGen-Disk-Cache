# NextGen Disk Cache 1.4.0

A conservative SKSE64 derivative of **Disk Cache Enabler** by **Archost** (original Nexus upload by **enpinion**).

## What 1.4.0 actually changes

The plugin hooks `CreateFileA` and `CreateFileW`, but the default policy now rewrites only a narrow class of requests:

- existing `.bsa` and `.ba2` files;
- read-only access;
- synchronous handles;
- no write-through or delete-on-close semantics.

For those archive reads only, it may remove `FILE_FLAG_NO_BUFFERING` and replace `FILE_FLAG_SEQUENTIAL_SCAN` with `FILE_FLAG_RANDOM_ACCESS`.

Everything else is untouched by default, including:

- `.ess`, `.skse`, `.cosave`, backups, journals, and SQLite state files;
- logs and temporary files;
- plugins, meshes, textures, audio, configuration files, and unknown extensions;
- every write-capable, overlapped, write-through, create, truncate, or delete-on-close request.

The plugin does not parse or edit save data.

## Why 1.4.0 exists

Earlier releases had two defensible criticism points:

1. read-only files with unknown extensions could still lose `FILE_FLAG_NO_BUFFERING`;
2. `FILE_FLAG_RANDOM_ACCESS` was forced on many file types where access may be sequential;
3. the default warm-cache profile could start a multi-threaded, multi-gigabyte read shortly after launch.

Version 1.4.0 closes those gaps. Unknown files are never modified, random-access hints are archive-only, and warm cache plus DirectStorage probing ship disabled.

## Profiles

- **Safe Default**: archive-only hook, process/hardware logging enabled, warm cache off.
- **Minimal Compatibility**: archive-only hook and logging, all optional tuning off.
- **Experimental Bounded Warm Cache**: 512 MB maximum, 8 MB per archive, one low-priority thread, 60-second delay. Benchmark before keeping it.

## Optional broad mode

`bConservativeHookScope=0` allows removal of `FILE_FLAG_NO_BUFFERING` from known loose-asset reads. Unknown extensions remain untouched, and `FILE_FLAG_RANDOM_ACCESS` remains limited to BSA/BA2 archives. Broad mode is not recommended as a modlist default.

## Safety guards

- internal policy self-test runs before hooks attach;
- malformed INI numbers are clamped to finite ranges;
- warm-cache auto-tune may reduce pressure but never silently expands the configured budget;
- warm cache scans BSA/BA2 archives only;
- legacy `DiskCacheEnabler.dll` should not be installed at the same time.

## Requirements

- Skyrim SE/AE launched through matching SKSE64
- No Address Library requirement

Supported runtime declarations remain 1.5.97, 1.6.640, and 1.6.659 GOG. Other runtimes require testing even though this plugin uses no game structures.

## Build

From a Visual Studio x64 developer environment:

```powershell
.\build.ps1
```

Nexus-ready packaging:

```powershell
.\package-release.ps1
```

## Credits and license

- Archost: original Disk Cache Enabler, ISC license
- enpinion: original Nexus upload
- Microsoft Detours: MIT license
- SKSE team: plugin API headers

See the included license files. This derivative retains Archost's copyright and ISC notice.
