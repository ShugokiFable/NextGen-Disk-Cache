# 1.2.0 overlay manifest

Apply with:

```powershell
python .\tools\apply_1_2.py C:\path\to\NextGen-Disk-Cache
```

The upgrader targets the public 1.1.0 `main` source and aborts if an expected anchor differs. It creates a timestamped backup before modification.

## Added

- `src/DirectStorageBackend.h`
- `src/DirectStorageBackend.cpp`
- `package-release.ps1`
- `.github/workflows/build-release.yml`
- `fomod/info.xml`
- `fomod/ModuleConfig.xml`
- `profiles/HighEnd/NextGenDiskCache.ini`
- `profiles/Balanced/NextGenDiskCache.ini`
- `docs/1.2.0-TECHNICAL-NOTES.md`
- `docs/1.2.0-AUDIT.md`

## Modified

- `src/main.cpp`
- `CMakeLists.txt`
- `build.ps1`
- `.gitignore`
- `package/SKSE/Plugins/NextGenDiskCache.ini`
- `README.md`
- `CHANGELOG.txt`
