#!/usr/bin/env python3
from __future__ import annotations

import configparser
import pathlib
import py_compile
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = "2.0.0"
PROFILES = ["SafeDefault", "Minimal", "ExperimentalWarmCache"]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_cfg(profile: str) -> configparser.ConfigParser:
    cfg = configparser.ConfigParser()
    cfg.optionxform = str
    cfg.read(ROOT / f"profiles/{profile}/NextGenDiskCache.ini", encoding="utf-8")
    return cfg


def main() -> int:
    required = [
        "src/main.cpp",
        "src/DirectStorageBackend.h",
        "src/DirectStorageBackend.cpp",
        "src/IatHook.h",
        "src/IatHook.cpp",
        "CMakeLists.txt",
        "build.ps1",
        "package-release.ps1",
        ".github/workflows/build-release.yml",
        "fomod/info.xml",
        "fomod/ModuleConfig.xml",
        "README.md",
        "CHANGELOG.txt",
        "PACKAGE-NOTICE.txt",
        "LICENSE.txt",
        "LICENSE.Archost-DiskCacheEnabler.txt",
        "LICENSE.detours.txt",
        "LICENSE.SKSE64.txt",
    ] + [f"profiles/{p}/NextGenDiskCache.ini" for p in PROFILES]
    for rel in required:
        require((ROOT / rel).is_file(), f"missing {rel}")

    # Public source bundle must not contain generated compiler output.
    forbidden_suffixes = {".pdb", ".exp", ".ilk", ".obj", ".pyc"}
    skip_top = {"deps", "build", "dist", "package", ".git"}

    def authored(path: pathlib.Path) -> bool:
        parts = path.relative_to(ROOT).parts
        return bool(parts) and parts[0] not in skip_top

    junk = [p for p in ROOT.rglob("*")
            if p.is_file() and p.suffix.lower() in forbidden_suffixes and authored(p)]
    require(not junk, "source bundle contains generated junk: " + ", ".join(map(str, junk)))
    require(not any(authored(p) for p in ROOT.rglob("__pycache__")),
            "source bundle contains __pycache__")

    with tempfile.TemporaryDirectory() as td:
        py_compile.compile(__file__, cfile=str(pathlib.Path(td) / "validate_rc.pyc"), doraise=True)

    info = ET.parse(ROOT / "fomod/info.xml").getroot()
    require(info.findtext("Version") == VERSION, "wrong fomod/info.xml version")
    module = ET.parse(ROOT / "fomod/ModuleConfig.xml").getroot()
    require(module.findtext("moduleName") == f"NextGen Disk Cache {VERSION}",
            "wrong FOMOD module version")
    groups = module.findall(".//group")
    require(len(groups) == 1, "FOMOD should contain only the profile group (no DirectStorage step)")
    require(all(g.attrib.get("type") == "SelectExactlyOne" for g in groups),
            "FOMOD choices must be exclusive")
    module_text = (ROOT / "fomod/ModuleConfig.xml").read_text(encoding="utf-8")
    require("Optional\\DirectStorage" not in module_text and
            "Optional/DirectStorage" not in module_text,
            "FOMOD must not install Optional DirectStorage runtime")
    require("Bundle Microsoft DirectStorage" not in module_text,
            "FOMOD must not offer a DirectStorage install step")

    sections = ["FileCache", "Process", "Hardware", "WarmCache", "DirectStorage", "Log"]
    for profile in PROFILES:
        cfg = read_cfg(profile)
        for section in sections:
            require(cfg.has_section(section), f"{profile}: missing [{section}]")
        require(cfg.get("FileCache", "bConservativeHookScope") == "1",
                f"{profile}: conservative scope must ship enabled")
        # Minimal deliberately ships the unbenchmarked random-access hint off;
        # every other profile must keep it archive-only rather than removing it.
        require(cfg.get("FileCache", "bPreferRandomAccessOnArchives") in {"0", "1"},
                f"{profile}: random-access hint must be explicitly set")
        # Only the Experimental profile may widen the hook past the executable.
        expected_scope = "1" if profile == "ExperimentalWarmCache" else "0"
        require(cfg.get("FileCache", "iHookScope") == expected_scope,
                f"{profile}: iHookScope must be {expected_scope}")
        require(cfg.get("Process", "bExpandWorkingSet") == "0",
                f"{profile}: working-set expansion must ship off")
        require(cfg.get("DirectStorage", "bDirectStorageProbe") == "0",
                f"{profile}: DirectStorage probing must ship off")
        require(cfg.get("DirectStorage", "bDirectStorageWarmRead") == "0",
                f"{profile}: DirectStorage raw reads must ship off")
        require(cfg.get("Log", "bLogStatsOnExit") == "0",
                f"{profile}: exit logging must ship off")
        require(cfg.get("Hardware", "iGameDriveClass") == "0",
                f"{profile}: drive override must ship on auto")

    safe = read_cfg("SafeDefault")
    minimal = read_cfg("Minimal")
    experimental = read_cfg("ExperimentalWarmCache")
    require(safe.get("WarmCache", "bEnableWarmCache") == "0",
            "safe default unexpectedly enables warm cache")
    require(minimal.get("WarmCache", "bEnableWarmCache") == "0",
            "minimal profile unexpectedly enables warm cache")
    require(minimal.get("Hardware", "bHardwareProfile") == "0",
            "minimal profile should disable hardware probing")
    require(experimental.get("WarmCache", "bEnableWarmCache") == "1",
            "experimental profile should enable warm cache")
    require(experimental.getint("WarmCache", "iWarmCacheBudgetMB") <= 512,
            "experimental warm-cache budget exceeds 512 MB")
    require(experimental.getint("WarmCache", "iWarmCacheThreads") == 1,
            "experimental warmer must ship single-threaded")
    require(experimental.getint("WarmCache", "iWarmCacheDelaySecs") >= 60,
            "experimental warmer starts too early")

    package_ini = ROOT / "package/SKSE/Plugins/NextGenDiskCache.ini"
    require(package_ini.is_file(), "missing staged package INI")
    require(package_ini.read_bytes() == (ROOT / "profiles/SafeDefault/NextGenDiskCache.ini").read_bytes(),
            "staged package INI must exactly match SafeDefault")

    main_cpp = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    for token in [
        f'PLUGIN_VERSION_STRING "{VERSION}"',
        # 1.4.0 shipped a self-test the optimizer folded to `true`; the failure
        # branch was absent from the binary. These pin the 1.4.1 fix: volatile
        # inputs and a check against the live settings keep it unfoldable.
        "static volatile DWORD vBase",
        "g_settings.conservativeHookScope)",
        "IsSafetyGated(flags, desiredAccess, creationDisposition)",
        "bConservativeHookScope",
        "PathKind::Archive",
        "creationDisposition != OPEN_EXISTING",
        "FILE_FLAG_DELETE_ON_CLOSE",
        "preferRandomAccessOnArchives && kind == PathKind::Archive",
        "ValidateFilePolicy()",
        "internal file-policy self-test failed",
        'const wchar_t* patterns[] = { L"\\\\*.bsa", L"\\\\*.ba2" }',
        "threads, 2u",
        "SanitizeSettings()",
        "ParseUnsigned(val, g_settings.warmCacheBudgetMB)",
        "ParseBool(val, g_settings.enableWarmCache)",
        "ParseBool(val, g_settings.directStorageProbe)",
    ]:
        require(token in main_cpp, f"main.cpp missing safety token: {token}")
    require('".ini", ".json"' in main_cpp, "known assets list unexpectedly missing")
    # Unknown files must never be eligible; stripping happens only after eligibleKind.
    require(main_cpp.index("if (!eligibleKind)") < main_cpp.index("out &= ~FILE_FLAG_NO_BUFFERING"),
            "NO_BUFFERING strip occurs before eligibility gate")

    ds_cpp = (ROOT / "src/DirectStorageBackend.cpp").read_text(encoding="utf-8")
    for token in ["LoadLibraryExW", "DStorageGetFactory", "CancelRequestsWithTag",
                  "Cancellation is asynchronous", "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR"]:
        require(token in ds_cpp, f"DirectStorage backend missing {token}")

    # The narrow IAT hook is the default scope; a process-wide trampoline must
    # stay opt-in and must never be the fallback when the IAT hook fails.
    iat_cpp = (ROOT / "src/IatHook.cpp").read_text(encoding="utf-8")
    for token in ["IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_ORDINAL_FLAG",
                  "VirtualProtect", "OriginalFirstThunk"]:
        require(token in iat_cpp, f"IAT hook missing {token}")
    for token in ["AttachHooksIat", "AttachHooksDetours",
                  "IatHookInstall(nullptr, \"CreateFileW\"",
                  "IAT hook was not installed",
                  "g_settings.hookScope == 1"]:
        require(token in main_cpp, f"main.cpp missing hook-scope token: {token}")
    require(main_cpp.index("return AttachHooksIat();") >
            main_cpp.index("if (g_settings.hookScope == 1)"),
            "IAT must be the default path, Detours the explicit opt-in")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require(f"VERSION {VERSION}" in cmake, "CMake version mismatch")
    require("DirectStorageBackend.cpp" in cmake, "DirectStorage backend not built")
    require("IatHook.cpp" in cmake, "IAT hook not built")

    packager = (ROOT / "package-release.ps1").read_text(encoding="utf-8")
    for token in ["Profiles\\SafeDefault", "Profiles\\Minimal",
                  "Profiles\\ExperimentalWarmCache", "Forbidden build artifacts",
                  "Get-FileHash", "PACKAGE-NOTICE.txt",
                  "Public package must not contain DirectStorage"]:
        require(token in packager, f"release packager missing {token}")
    require("Optional\\DirectStorage" not in packager or
            "intentionally not shipped" in packager,
            "packager must not stage Optional DirectStorage runtime")
    # Explicitly reject the old stage path that copied dstorage*.dll into the zip.
    require("Copy-Item $dsLicense" not in packager,
            "packager must not copy LICENSE.DirectStorage.txt into public zip")
    require("dstorage*.dll" not in packager or
            "must not contain DirectStorage DLLs" in packager,
            "packager must not ship dstorage runtime DLLs")

    workflow = (ROOT / ".github/workflows/build-release.yml").read_text(encoding="utf-8")
    for token in ["windows-2022", "NuGet/setup-nuget@v2",
                  f"NextGenDiskCache-{VERSION}-FOMOD.zip",
                  "build/symbols/NextGenDiskCache.pdb",
                  "python tools/validate_rc.py"]:
        require(token in workflow, f"workflow missing {token}")

    print("VALIDATION PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
