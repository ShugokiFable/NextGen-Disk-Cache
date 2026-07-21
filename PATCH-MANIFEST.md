# NextGen Disk Cache 1.4.0 safety rework

Base: `af0ab6172ae6f19c5f8511d46cd55163948c272a` (1.3.1).

## Core policy changes

- Default hook scope is limited to existing, synchronous, read-only BSA/BA2 archive opens.
- Unknown extensions, loose assets, plugins, saves, cosaves, state databases, logs, temporary files, writes, creates, truncates, overlapped handles, write-through handles, and delete-on-close handles retain the caller's flags.
- `FILE_FLAG_RANDOM_ACCESS` is archive-only.
- An internal policy self-test must pass before hooks attach.
- INI numeric values are clamped.

## Background I/O changes

- Warm cache is disabled by default.
- DirectStorage probing and raw reads are disabled by default.
- The optional warmer is limited to BSA/BA2, 512 MB, 8 MB per archive, one low-priority thread, and a 60-second delay in the supplied experimental profile.
- Auto-tune may reduce load but cannot inflate the configured budget.

## Release layout

- `SafeDefault`: recommended archive-only profile.
- `Minimal`: archive-only with optional process/hardware tuning disabled.
- `ExperimentalWarmCache`: bounded opt-in prefetch profile.

The source bundle intentionally contains no 1.4.0 DLL. Build and Windows CI validation are still required before shipping a public binary.
