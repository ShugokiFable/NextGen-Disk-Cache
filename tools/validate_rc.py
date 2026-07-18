#!/usr/bin/env python3
from __future__ import annotations

import argparse
import configparser
import importlib.util
import pathlib
import py_compile
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def validate_bundle() -> None:
    required = [
        "tools/apply_1_2.py",
        "src/DirectStorageBackend.h",
        "src/DirectStorageBackend.cpp",
        "package-release.ps1",
        ".github/workflows/build-release.yml",
        "fomod/info.xml",
        "fomod/ModuleConfig.xml",
        "profiles/HighEnd/NextGenDiskCache.ini",
        "profiles/Balanced/NextGenDiskCache.ini",
        "docs/1.2.0-TECHNICAL-NOTES.md",
        "docs/1.2.0-AUDIT.md",
    ]
    for rel in required:
        require((ROOT / rel).is_file(), f"missing {rel}")

    forbidden_suffixes = {".pdb", ".lib", ".exp", ".ilk", ".obj", ".pyc"}
    # Only authored source is scanned for build junk. Vendored import libraries
    # (deps/), CMake output (build/), release staging (dist/, package/), and the
    # git store legitimately contain .lib/.pdb/.obj and must not fail this check.
    skip_top = {"deps", "build", "dist", "package", ".git"}

    def authored(p: pathlib.Path) -> bool:
        rel = p.relative_to(ROOT).parts
        return bool(rel) and rel[0] not in skip_top

    junk = [p for p in ROOT.rglob("*")
            if p.is_file() and p.suffix.lower() in forbidden_suffixes and authored(p)]
    require(not junk, "source bundle contains generated/build junk: " + ", ".join(str(p) for p in junk))
    require(not any(authored(p) for p in ROOT.rglob("__pycache__")),
            "source bundle contains __pycache__")

    # Compile to a throwaway cfile: writing __pycache__ next to the script
    # would trip this validator's own junk scan on the next run.
    with tempfile.TemporaryDirectory() as td:
        py_compile.compile(str(ROOT / "tools/apply_1_2.py"),
                           cfile=str(pathlib.Path(td) / "apply_1_2.pyc"), doraise=True)
    ET.parse(ROOT / "fomod/info.xml")
    module = ET.parse(ROOT / "fomod/ModuleConfig.xml").getroot()
    require(module.findtext("moduleName") == "NextGen Disk Cache 1.3.0", "wrong FOMOD version")
    groups = module.findall(".//group")
    require(len(groups) == 2, "FOMOD should contain profile and DirectStorage groups")
    require(all(g.attrib.get("type") == "SelectExactlyOne" for g in groups), "FOMOD choices must be exclusive")

    for profile in ["HighEnd", "Balanced"]:
        cfg = configparser.ConfigParser()
        cfg.optionxform = str
        cfg.read(ROOT / f"profiles/{profile}/NextGenDiskCache.ini", encoding="utf-8")
        for section in ["FileCache", "Process", "Hardware", "WarmCache", "DirectStorage", "Log"]:
            require(cfg.has_section(section), f"{profile}: missing [{section}]")
        require(cfg.get("Process", "bExpandWorkingSet") == "0", f"{profile}: working-set expansion must ship off")
        require(cfg.get("DirectStorage", "bDirectStorageProbe") == "1", f"{profile}: DirectStorage probe missing")
        require(cfg.get("DirectStorage", "bDirectStorageWarmRead") == "0", f"{profile}: experimental DS must ship off")
        require(cfg.get("Log", "bLogStatsOnExit") == "0", f"{profile}: exit logging must ship off")
        require(cfg.get("Log", "bLogStatsAfterWarm") == "1", f"{profile}: post-warm stats missing")
        require(cfg.get("Hardware", "iGameDriveClass") == "0", f"{profile}: drive override must ship on auto")

    high = configparser.ConfigParser(); high.optionxform = str
    high.read(ROOT / "profiles/HighEnd/NextGenDiskCache.ini", encoding="utf-8")
    require(high.get("Hardware", "bHighEndMode") == "1", "high-end mode is not default")
    require(high.getint("WarmCache", "iWarmCacheMaxFiles") >= 512, "high-end file ceiling too low")

    cpp = (ROOT / "src/DirectStorageBackend.cpp").read_text(encoding="utf-8")
    for token in ["LoadLibraryExW", "DStorageGetFactory", "IDStorageQueue", "DSTORAGE_REQUEST_DESTINATION_MEMORY", "CancelRequestsWithTag"]:
        require(token in cpp, f"DirectStorage backend missing {token}")
    require("LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR" in cpp, "bundled runtime directory is not searched safely")
    require("DirectStorageReadDiscard" in cpp, "actual DirectStorage read path missing")
    require("createMemoryQueue" in cpp, "probe-only mode still creates a queue")
    require("Cancellation is asynchronous" in cpp, "DirectStorage cancellation lifetime guard missing")
    require("reinterpret_cast<LPCWSTR>(&g_info)" in cpp, "module address lookup uses a function-pointer cast")

    packager = (ROOT / "package-release.ps1").read_text(encoding="utf-8")
    for token in ["Compress-Archive", "Get-FileHash", "Forbidden build artifacts", "LICENSE.DirectStorage.txt"]:
        require(token in packager, f"release packager missing {token}")

    workflow = (ROOT / ".github/workflows/build-release.yml").read_text(encoding="utf-8")
    for token in ["windows-2022", "NuGet/setup-nuget@v2", "Microsoft.Direct3D.DirectStorage", "NextGenDiskCache-1.3.0-FOMOD.zip", "build/symbols/NextGenDiskCache.pdb"]:
        require(token in workflow, f"workflow missing {token}")
    require("package-release.ps1 -SkipBuild" in workflow,
            "workflow does not use the validated release packager")
    require(".pdb" in packager and "Forbidden build artifacts" in packager,
            "public FOMOD gate does not reject PDBs")


def synthetic_transform_test() -> None:
    # Do not let the loader drop a __pycache__ next to tools/apply_1_2.py.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("ngdc_apply", ROOT / "tools/apply_1_2.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(mod)

    with tempfile.TemporaryDirectory() as td:
        repo = pathlib.Path(td) / "repo"
        (repo / "src").mkdir(parents=True)
        # The adaptive replacement shim exercises every transformation in order.
        marker = "// ---------------------------------------------------------------------------\n// DllMain - attach CreateFile hooks as early as possible"
        final_log_anchor = '\tLog("NextGen Disk Cache " PLUGIN_VERSION_STRING\n\t\t" loaded'
        (repo / "src/main.cpp").write_text(final_log_anchor + "\n" + marker + "\n", encoding="utf-8")

        original = mod.replace_once
        def adaptive(text: str, old: str, new: str, label: str) -> str:
            if old not in text:
                if marker in text:
                    text = text.replace(marker, old + "\n" + marker, 1)
                else:
                    text += "\n" + old
            return original(text, old, new, label)
        mod.replace_once = adaptive
        mod.patch_main(repo)
        out = (repo / "src/main.cpp").read_text(encoding="utf-8")
        for token in ["PLUGIN_VERSION_STRING \"1.2.0\"", "DirectStorageInitialize", "ReserveWarmBudget", "g_warmStarted", "safe SKSE load path", "Stats snapshot", "kind == PathKind::Asset"]:
            require(token in out, f"transform did not emit {token}")


def validate_repo(repo: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory() as td:
        copy = pathlib.Path(td) / "repo"
        shutil.copytree(repo, copy, ignore=shutil.ignore_patterns(".git", "build", "*.pdb", "*.dll", "__pycache__", "*.pyc"))
        subprocess.run([sys.executable, str(ROOT / "tools/apply_1_2.py"), str(copy)], check=True)
        main = (copy / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (copy / "CMakeLists.txt").read_text(encoding="utf-8")
        require("PLUGIN_VERSION_STRING \"1.2.0\"" in main, "repo transform did not bump main.cpp")
        require("DirectStorageBackend.cpp" in cmake, "repo transform did not add backend to CMake")
        require("LoadSettings();" in main and "safe SKSE load path" in main, "loader-lock move missing")
        require("PDB_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_BINARY_DIR}/symbols\"" in cmake, "symbols still stage with mod")
        require("COMPILE_PDB_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_BINARY_DIR}/symbols\"" in cmake, "compiler symbols still stage with mod")
        require("/Zi" in cmake and "/permissive-" not in cmake, "release symbol/compiler policy is wrong")
        require((copy / "tools/validate_rc.py").is_file(), "validator was not copied into upgraded repo")
        require((copy / ".github/workflows/build-release.yml").is_file(), "workflow was not copied")
        subprocess.run([sys.executable, str(copy / "tools/validate_rc.py")], cwd=copy, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, help="optional clean 1.1.0 checkout to dry-run")
    args = parser.parse_args()
    validate_bundle()
    synthetic_transform_test()
    if args.repo:
        validate_repo(args.repo.resolve())
    print("VALIDATION PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
