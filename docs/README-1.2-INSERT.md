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
