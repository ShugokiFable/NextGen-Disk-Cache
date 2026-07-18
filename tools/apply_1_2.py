#!/usr/bin/env python3
r"""Upgrade a clean NextGen-Disk-Cache 1.1.0 checkout to the 1.2.0 source RC.

Usage:
  python tools/apply_1_2.py C:\path\to\NextGen-Disk-Cache

The script is intentionally anchor-based and aborts rather than guessing when
an upstream file differs. It creates a timestamped backup before modifying files.
"""
from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def copy_overlay(repo: pathlib.Path) -> None:
    for rel in [
        "src/DirectStorageBackend.h",
        "src/DirectStorageBackend.cpp",
        "tools/apply_1_2.py",
        "tools/validate_rc.py",
        "package-release.ps1",
        ".github/workflows/build-release.yml",
        "fomod/info.xml",
        "fomod/ModuleConfig.xml",
        "profiles/HighEnd/NextGenDiskCache.ini",
        "profiles/Balanced/NextGenDiskCache.ini",
        "docs/README-1.2-INSERT.md",
        "docs/CHANGELOG-1.2-ENTRY.txt",
        "docs/1.2.0-TECHNICAL-NOTES.md",
        "docs/1.2.0-AUDIT.md",
        "PATCH-MANIFEST.md",
        "BASE-COMMIT.txt",
    ]:
        src = HERE / rel
        dst = repo / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.resolve() != dst.resolve():
            shutil.copy2(src, dst)


def patch_cmake(repo: pathlib.Path) -> None:
    path = repo / "CMakeLists.txt"
    text = path.read_text(encoding="utf-8-sig")
    text = replace_once(text,
        "project(NextGenDiskCache VERSION 1.1.0 LANGUAGES CXX)",
        "project(NextGenDiskCache VERSION 1.2.0 LANGUAGES CXX)",
        "CMake version")
    text = replace_once(text,
        "add_library(NextGenDiskCache SHARED src/main.cpp)",
        "add_library(NextGenDiskCache SHARED\n  src/main.cpp\n  src/DirectStorageBackend.cpp\n)",
        "CMake sources")
    text = replace_once(text,
        "  ${CMAKE_SOURCE_DIR}/deps/include\n)",
        "  ${CMAKE_SOURCE_DIR}/deps/include\n  ${CMAKE_SOURCE_DIR}/deps/directstorage/native/include\n)",
        "DirectStorage include")
    text = replace_once(text,
        "  target_compile_options(NextGenDiskCache PRIVATE /W3 /EHsc /Zi)\n  target_link_options(NextGenDiskCache PRIVATE /DEBUG /OPT:REF /OPT:ICF)",
        "  target_compile_options(NextGenDiskCache PRIVATE /W4 /EHsc /Zi /Zc:__cplusplus)\n  target_link_options(NextGenDiskCache PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)\n  # Keep release symbols outside the Nexus payload. CI archives them separately.\n  set_target_properties(NextGenDiskCache PROPERTIES COMPILE_PDB_NAME NextGenDiskCache)",
        "release compiler options")
    text = replace_once(text,
        '  PDB_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"',
        '  PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/symbols"\n  COMPILE_PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/symbols"',
        "PDB output directory")
    path.write_text(text, encoding="utf-8")


def patch_main(repo: pathlib.Path) -> None:
    path = repo / "src/main.cpp"
    text = path.read_text(encoding="utf-8-sig")

    text = replace_once(text,
        "#include <algorithm>\n\n#include <Windows.h>",
        "#include <algorithm>\n#include <memory>\n\n#include <Windows.h>",
        "standard includes")
    text = replace_once(text,
        "#include <detours/detours.h>",
        "#include <detours/detours.h>\n#include \"DirectStorageBackend.h\"",
        "DirectStorage include")
    text = replace_once(text,
        "#define PLUGIN_VERSION ((1u << 16) | (1u << 8) | 0u) // 1.1.0\n#define PLUGIN_VERSION_STRING \"1.1.0\"",
        "#define PLUGIN_VERSION ((1u << 16) | (2u << 8) | 0u) // 1.2.0\n#define PLUGIN_VERSION_STRING \"1.2.0\"",
        "plugin version")
    text = replace_once(text,
        "\tbool disablePowerThrottling = true;\n\tbool raiseMemoryPriority = true;\n\tbool expandWorkingSet = true;",
        "\tbool disablePowerThrottling = true;\n\tbool raiseMemoryPriority = true;\n\tbool expandWorkingSet = false;",
        "safe process defaults")
    text = replace_once(text,
        "\tbool logToFile = true;\n\tbool logEveryOpen = false; // debug only - very spammy\n\tbool logStatsOnExit = true;",
        "\tbool logToFile = true;\n\tbool logEveryOpen = false; // debug only - very spammy\n\tbool logStatsOnExit = false; // retained for config compatibility; no loader-lock I/O\n\tbool logStatsAfterWarm = true;",
        "safe stats settings")
    text = replace_once(text,
        "\tunsigned warmCacheDelaySecs = 8;\n\tunsigned warmCacheMaxFiles = 256;",
        "\tunsigned warmCacheDelaySecs = 12;\n\tunsigned warmCacheMaxFiles = 512;",
        "high-end warm defaults")
    text = replace_once(text,
        "static std::thread g_warmThread;",
        "static std::atomic<bool> g_warmStarted{false};",
        "detached warm-thread state")
    text = replace_once(text,
        "\t\t(kind == PathKind::Asset || kind == PathKind::Other)) {",
        "\t\tkind == PathKind::Asset) {",
        "asset-only random access")

    text = replace_once(text,
        "\tbool hardwareProfile = true;   // detect + log; required for auto-tune\n\tbool autoTune = true;          // scale warm cache from the profile\n\tbool preferFastCores = false;  // OPT-IN: V-Cache CCD / P-core CPU-set hint\n",
        "\tbool hardwareProfile = true;\n\tbool autoTune = true;\n\tbool highEndMode = true;       // default target: modern X3D/DDR5/NVMe systems\n\tbool preferFastCores = false;  // legacy whole-process hint, still opt-in\n\tbool placeWarmThreadsOnBackgroundCores = true;\n",
        "hardware settings")
    text = replace_once(text,
        "\tbool warmCacheMappedPrefetch = true;\n\tbool warmCacheLowIoPriority = true;\n\tunsigned warmCacheThreadPriority = THREAD_PRIORITY_LOWEST;\n",
        "\tbool warmCacheMappedPrefetch = true;\n\tbool warmCacheLowIoPriority = true;\n\tbool warmCacheStridedPrefetch = true;\n\tunsigned warmCacheStrideMB = 256;\n\tunsigned warmCacheReserveMB = 0; // 0 = automatic safety reserve\n\tbool stopWarmCacheOnMemoryPressure = true;\n\tint warmCacheThreadPriority = THREAD_PRIORITY_LOWEST;\n\n\tbool directStorageProbe = true;\n\tbool directStorageWarmRead = false; // experimental: raw read, not OS cache population\n\tunsigned directStorageQueueCapacity = 128;\n\tunsigned directStorageBatchMB = 64;\n\tunsigned directStorageTimeoutMs = 30000;\n",
        "warm and DirectStorage settings")

    text = replace_once(text,
        "using SetProcessDefaultCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);\nusing SetThreadInformation_t = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);",
        "using SetProcessDefaultCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);\nusing SetThreadSelectedCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);\nusing SetThreadInformation_t = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);",
        "CPU set type")
    text = replace_once(text,
        "static SetProcessDefaultCpuSets_t g_pSetProcessDefaultCpuSets = nullptr;\nstatic SetThreadInformation_t g_pSetThreadInformation = nullptr;",
        "static SetProcessDefaultCpuSets_t g_pSetProcessDefaultCpuSets = nullptr;\nstatic SetThreadSelectedCpuSets_t g_pSetThreadSelectedCpuSets = nullptr;\nstatic SetThreadInformation_t g_pSetThreadInformation = nullptr;",
        "CPU set pointer")
    text = replace_once(text,
        "\tg_pSetProcessDefaultCpuSets =\n\t\treinterpret_cast<SetProcessDefaultCpuSets_t>(GetProcAddress(k32, \"SetProcessDefaultCpuSets\"));\n\tg_pSetThreadInformation =",
        "\tg_pSetProcessDefaultCpuSets =\n\t\treinterpret_cast<SetProcessDefaultCpuSets_t>(GetProcAddress(k32, \"SetProcessDefaultCpuSets\"));\n\tg_pSetThreadSelectedCpuSets =\n\t\treinterpret_cast<SetThreadSelectedCpuSets_t>(GetProcAddress(k32, \"SetThreadSelectedCpuSets\"));\n\tg_pSetThreadInformation =",
        "resolve thread CPU sets")

    parser_anchor = """\t\telse if (_stricmp(key, \"bWarmCacheLowIoPriority\") == 0)
\t\t\tg_settings.warmCacheLowIoPriority = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bLogToFile\") == 0)"""
    parser_repl = """\t\telse if (_stricmp(key, \"bWarmCacheLowIoPriority\") == 0)
\t\t\tg_settings.warmCacheLowIoPriority = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bHighEndMode\") == 0)
\t\t\tg_settings.highEndMode = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bPlaceWarmThreadsOnBackgroundCores\") == 0)
\t\t\tg_settings.placeWarmThreadsOnBackgroundCores = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bWarmCacheStridedPrefetch\") == 0)
\t\t\tg_settings.warmCacheStridedPrefetch = ParseBool(val, true);
\t\telse if (_stricmp(key, \"iWarmCacheStrideMB\") == 0)
\t\t\tg_settings.warmCacheStrideMB = (unsigned)atoi(val);
\t\telse if (_stricmp(key, \"iWarmCacheReserveMB\") == 0)
\t\t\tg_settings.warmCacheReserveMB = (unsigned)atoi(val);
\t\telse if (_stricmp(key, \"bStopWarmCacheOnMemoryPressure\") == 0)
\t\t\tg_settings.stopWarmCacheOnMemoryPressure = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bDirectStorageProbe\") == 0)
\t\t\tg_settings.directStorageProbe = ParseBool(val, true);
\t\telse if (_stricmp(key, \"bDirectStorageWarmRead\") == 0)
\t\t\tg_settings.directStorageWarmRead = ParseBool(val, false);
\t\telse if (_stricmp(key, \"iDirectStorageQueueCapacity\") == 0)
\t\t\tg_settings.directStorageQueueCapacity = (unsigned)atoi(val);
\t\telse if (_stricmp(key, \"iDirectStorageBatchMB\") == 0)
\t\t\tg_settings.directStorageBatchMB = (unsigned)atoi(val);
\t\telse if (_stricmp(key, \"iDirectStorageTimeoutMs\") == 0)
\t\t\tg_settings.directStorageTimeoutMs = (unsigned)atoi(val);
\t\telse if (_stricmp(key, \"bLogToFile\") == 0)"""
    text = replace_once(text, parser_anchor, parser_repl, "INI parser")
    text = replace_once(text,
        "\t\telse if (_stricmp(key, \"bLogStatsOnExit\") == 0)\n\t\t\tg_settings.logStatsOnExit = ParseBool(val, true);",
        "\t\telse if (_stricmp(key, \"bLogStatsOnExit\") == 0)\n\t\t\tg_settings.logStatsOnExit = ParseBool(val, false);\n\t\telse if (_stricmp(key, \"bLogStatsAfterWarm\") == 0)\n\t\t\tg_settings.logStatsAfterWarm = ParseBool(val, true);",
        "post-warm stats parser")

    background_code = r'''
static void ApplyBackgroundCorePreference(const HardwareProfile& hw)
{
    if (!g_settings.placeWarmThreadsOnBackgroundCores ||
        !g_pGetSystemCpuSetInformation || !g_pSetThreadSelectedCpuSets)
        return;

    ULONG need = 0;
    g_pGetSystemCpuSetInformation(nullptr, 0, &need, GetCurrentProcess(), 0);
    if (!need)
        return;
    std::vector<BYTE> buf(need);
    if (!g_pGetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
            need, &need, GetCurrentProcess(), 0))
        return;

    const HardwareProfile::L3Domain* smallL3 = nullptr;
    if (hw.dualCcdX3d) {
        for (const auto& d : hw.l3) {
            if (!smallL3 || d.bytes < smallL3->bytes)
                smallL3 = &d;
        }
    }

    BYTE minEfficiency = 255;
    if (hw.hybridCpu) {
        BYTE* p = buf.data();
        const BYTE* end = buf.data() + need;
        while (p < end) {
            auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(p);
            if (!e->Size)
                break;
            if (e->Type == CpuSetInformation && !e->CpuSet.Parked)
                minEfficiency = std::min(minEfficiency, e->CpuSet.EfficiencyClass);
            p += e->Size;
        }
    }

    std::vector<ULONG> ids;
    BYTE* p = buf.data();
    const BYTE* end = buf.data() + need;
    while (p < end) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(p);
        if (!e->Size)
            break;
        if (e->Type == CpuSetInformation && !e->CpuSet.Parked) {
            bool use = false;
            if (smallL3) {
                use = e->CpuSet.Group == smallL3->group &&
                    ((smallL3->mask >> e->CpuSet.LogicalProcessorIndex) & 1) != 0;
            } else if (hw.hybridCpu && minEfficiency != 255) {
                use = e->CpuSet.EfficiencyClass == minEfficiency;
            }
            if (use)
                ids.push_back(e->CpuSet.Id);
        }
        p += e->Size;
    }

    if (!ids.empty() && g_pSetThreadSelectedCpuSets(
            GetCurrentThread(), ids.data(), static_cast<ULONG>(ids.size()))) {
        Log("WarmCache: background thread placed on %u %s logical CPUs",
            static_cast<unsigned>(ids.size()), smallL3 ? "non-V-Cache CCD" : "efficiency-class");
    }
}

static uint64_t AutomaticMemoryReserveMB(uint64_t totalMB)
{
    if (g_settings.warmCacheReserveMB)
        return g_settings.warmCacheReserveMB;
    if (totalMB >= 60ull * 1024) return 8192;
    if (totalMB >= 30ull * 1024) return 4096;
    if (totalMB >= 14ull * 1024) return 2048;
    return 1024;
}

static bool HasWarmCacheMemoryHeadroom()
{
    if (!g_settings.stopWarmCacheOnMemoryPressure)
        return true;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return true;
    const uint64_t totalMB = ms.ullTotalPhys >> 20;
    const uint64_t availMB = ms.ullAvailPhys >> 20;
    return availMB > AutomaticMemoryReserveMB(totalMB);
}

'''
    text = replace_once(text,
        "// ---------------------------------------------------------------------------\n// Process-level modern Windows hints",
        background_code + "// ---------------------------------------------------------------------------\n// Process-level modern Windows hints",
        "background CPU placement")

    old_prefetch = r'''static uint64_t WarmMappedPrefetch(HANDLE file, uint64_t bytes)
{
	HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!map)
		return 0;
	uint64_t done = 0;
	void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, (SIZE_T)bytes);
	if (view) {
		BYTE* base = (BYTE*)view;
		const uint64_t chunk = 64ull << 20; // 64 MB per Prefetch call, shutdown-checkable
		while (done < bytes && !g_shutdown.load(std::memory_order_relaxed)) {
			const uint64_t n = std::min(chunk, bytes - done);
			WIN32_MEMORY_RANGE_ENTRY r{ base + done, (SIZE_T)n };
			if (!g_pPrefetchVirtualMemory(GetCurrentProcess(), 1, &r, 0))
				break;
			done += n;
		}
		UnmapViewOfFile(view);
	}
	CloseHandle(map);
	return done;
}'''
    new_prefetch = r'''static uint64_t WarmMappedPrefetch(HANDLE file, uint64_t fileBytes, uint64_t budgetBytes)
{
	HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!map)
		return 0;
	void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
	if (!view) {
		CloseHandle(map);
		return 0;
	}

	BYTE* base = static_cast<BYTE*>(view);
	const uint64_t chunk = 32ull << 20;
	const uint64_t stride = std::max<uint64_t>(chunk,
		static_cast<uint64_t>(std::max(32u, g_settings.warmCacheStrideMB)) << 20);
	uint64_t requested = 0;

	if (g_settings.warmCacheStridedPrefetch && fileBytes > budgetBytes + stride) {
		// Touch representative windows across the entire archive instead of only
		// its header. This better matches open-world traversal through large BSAs.
		const uint64_t windows = std::max<uint64_t>(1, (budgetBytes + chunk - 1) / chunk);
		const uint64_t step = std::max<uint64_t>(stride, fileBytes / windows);
		std::vector<WIN32_MEMORY_RANGE_ENTRY> ranges;
		for (uint64_t off = 0; off < fileBytes && requested < budgetBytes; off += step) {
			const uint64_t n = std::min({ chunk, fileBytes - off, budgetBytes - requested });
			ranges.push_back({ base + off, static_cast<SIZE_T>(n) });
			requested += n;
		}
		if (!ranges.empty() && !g_shutdown.load(std::memory_order_relaxed)) {
			if (!g_pPrefetchVirtualMemory(GetCurrentProcess(), ranges.size(), ranges.data(), 0))
				requested = 0;
		}
	} else {
		while (requested < budgetBytes && !g_shutdown.load(std::memory_order_relaxed)) {
			const uint64_t n = std::min(chunk, budgetBytes - requested);
			WIN32_MEMORY_RANGE_ENTRY r{ base + requested, static_cast<SIZE_T>(n) };
			if (!g_pPrefetchVirtualMemory(GetCurrentProcess(), 1, &r, 0))
				break;
			requested += n;
		}
	}

	UnmapViewOfFile(view);
	CloseHandle(map);
	return requested;
}'''
    text = replace_once(text, old_prefetch, new_prefetch, "strided prefetch")

    text = replace_once(text,
        "static uint64_t WarmOneFile(const std::wstring& path, uint64_t maxBytes, bool mapped)\n{\n\tHANDLE h = CreateFileW_orig(",
        "static uint64_t WarmOneFile(const std::wstring& path, uint64_t maxBytes, bool mapped)\n{\n\tif (g_settings.directStorageWarmRead && DirectStorageAvailable()) {\n\t\treturn DirectStorageReadDiscard(path.c_str(), maxBytes,\n\t\t\tg_settings.directStorageTimeoutMs, g_shutdown);\n\t}\n\n\tHANDLE h = CreateFileW_orig(",
        "DirectStorage warm path")
    text = replace_once(text,
        "\tLARGE_INTEGER fs{};\n\tif (GetFileSizeEx(h, &fs) && (uint64_t)fs.QuadPart < maxBytes)\n\t\tmaxBytes = (uint64_t)fs.QuadPart;",
        "\tLARGE_INTEGER fs{};\n\tconst bool haveFileSize = GetFileSizeEx(h, &fs) != FALSE;\n\tif (haveFileSize && static_cast<uint64_t>(fs.QuadPart) < maxBytes)\n\t\tmaxBytes = static_cast<uint64_t>(fs.QuadPart);",
        "warm file-size safety")
    text = replace_once(text,
        "\tif (mapped && g_pPrefetchVirtualMemory) {\n\t\ttotal = WarmMappedPrefetch(h, maxBytes);\n\t}",
        "\tif (mapped && haveFileSize && g_pPrefetchVirtualMemory) {\n\t\ttotal = WarmMappedPrefetch(h, static_cast<uint64_t>(fs.QuadPart), maxBytes);\n\t}",
        "prefetch call")
    text = replace_once(text,
        "\t\tDWORD rd = 0;\n\t\twhile (total < maxBytes &&\n\t\t\tReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0) {\n\t\t\ttotal += rd;\n\t\t\tif (g_shutdown.load(std::memory_order_relaxed))\n\t\t\t\tbreak;\n\t\t}",
        "\t\tDWORD rd = 0;\n\t\twhile (total < maxBytes) {\n\t\t\tconst DWORD readBytes = static_cast<DWORD>(std::min<uint64_t>(buf.size(), maxBytes - total));\n\t\t\tif (!ReadFile(h, buf.data(), readBytes, &rd, nullptr) || rd == 0)\n\t\t\t\tbreak;\n\t\t\ttotal += rd;\n\t\t\tif (g_shutdown.load(std::memory_order_relaxed))\n\t\t\t\tbreak;\n\t\t}",
        "bounded ReadFile fallback")

    reserve_helper = r'''static uint64_t ReserveWarmBudget(uint64_t wanted)
{
	int64_t current = g_warmBudgetLeft.load(std::memory_order_relaxed);
	while (current > 0) {
		const uint64_t take = std::min<uint64_t>(wanted, static_cast<uint64_t>(current));
		if (g_warmBudgetLeft.compare_exchange_weak(current, current - static_cast<int64_t>(take),
				std::memory_order_relaxed, std::memory_order_relaxed))
			return take;
	}
	return 0;
}

'''
    text = replace_once(text,
        "static void WarmWorker()",
        reserve_helper + "static void WarmWorker()",
        "atomic warm-budget reservation")

    text = replace_once(text,
        "static void WarmWorker()\n{\n\tSetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);",
        "static void WarmWorker()\n{\n\tSetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);\n\tApplyBackgroundCorePreference(g_hw);",
        "warm CPU placement call")
    text = replace_once(text,
        "\t\tif (g_shutdown.load(std::memory_order_relaxed))\n\t\t\treturn;\n\t\tconst size_t idx =",
        "\t\tif (g_shutdown.load(std::memory_order_relaxed))\n\t\t\treturn;\n\t\tif (!HasWarmCacheMemoryHeadroom()) {\n\t\t\tLog(\"WarmCache: stopped because the RAM safety reserve was reached\");\n\t\t\treturn;\n\t\t}\n\t\tconst size_t idx =",
        "memory pressure stop")

    old_budget = r'''\t} else if (tune) {
\t\t// Scale with RAM but never eat more than a quarter of what's free now.
\t\t// Thresholds carry slack: a "32 GB" kit reports ~31.8 GiB usable.
\t\tbudgetMB = totalMB >= 60 * 1024 ? 8192
\t\t\t: totalMB >= 30 * 1024 ? 4096
\t\t\t: totalMB >= 14 * 1024 ? 1536
\t\t\t: 512;
\t\tif (drive == HardwareProfile::Drive::HDD)
\t\t\tbudgetMB = std::min<uint64_t>(budgetMB, 256); // don't grind a spinner
\t\tbudgetMB = std::min(budgetMB, availMB / 4);
\t} else {'''.replace('\\t','\t')
    new_budget = r'''\t} else if (tune) {
\t\tconst uint64_t reserveMB = AutomaticMemoryReserveMB(totalMB);
\t\tif (g_settings.highEndMode) {
\t\t\tbudgetMB = totalMB >= 60 * 1024 ? 12288
\t\t\t\t: totalMB >= 30 * 1024 ? 6144
\t\t\t\t: totalMB >= 14 * 1024 ? 2048
\t\t\t\t: 768;
\t\t} else {
\t\t\tbudgetMB = totalMB >= 60 * 1024 ? 8192
\t\t\t\t: totalMB >= 30 * 1024 ? 4096
\t\t\t\t: totalMB >= 14 * 1024 ? 1536
\t\t\t\t: 512;
\t\t}
\t\tif (drive == HardwareProfile::Drive::HDD)
\t\t\tbudgetMB = std::min<uint64_t>(budgetMB, 256);
\t\tconst uint64_t usableMB = availMB > reserveMB ? availMB - reserveMB : 0;
\t\tbudgetMB = std::min(budgetMB, usableMB);
\t} else {'''.replace('\\t','\t')
    text = replace_once(text, old_budget, new_budget, "high-end budget")

    old_perfile = r'''\tif (tune) {
\t\tconst uint64_t byDrive = drive == HardwareProfile::Drive::NvmeSsd ? 64
\t\t\t: drive == HardwareProfile::Drive::SataSsd ? 32
\t\t\t: drive == HardwareProfile::Drive::HDD ? 8
\t\t\t: 16;
\t\tperFileMB = std::max(perFileMB, byDrive);
\t}'''.replace('\\t','\t')
    new_perfile = r'''\tif (tune) {
\t\tconst uint64_t byDrive = drive == HardwareProfile::Drive::NvmeSsd
\t\t\t? (g_settings.highEndMode ? 256 : 128)
\t\t\t: drive == HardwareProfile::Drive::SataSsd ? 64
\t\t\t: drive == HardwareProfile::Drive::HDD ? 8
\t\t\t: 32;
\t\tperFileMB = std::max(perFileMB, byDrive);
\t}'''.replace('\\t','\t')
    text = replace_once(text, old_perfile, new_perfile, "per-file tuning")

    old_threads = r'''\tif (!threads) {
\t\tif (tune) {
\t\t\t// NVMe loves queue depth; HDD hates concurrent readers.
\t\t\tthreads = drive == HardwareProfile::Drive::NvmeSsd ? 4
\t\t\t\t: drive == HardwareProfile::Drive::SataSsd ? 2
\t\t\t\t: 1;
\t\t} else {
\t\t\tthreads = 1;
\t\t}
\t}
\tthreads = std::max(1u, std::min({ threads, 8u, (unsigned)std::max<size_t>(plan.files.size(), 1) }));'''.replace('\\t','\t')
    new_threads = r'''\tif (!threads) {
\t\tif (tune) {
\t\t\tif (drive == HardwareProfile::Drive::NvmeSsd) {
\t\t\t\tconst unsigned cpuScale = std::max(4u, std::min(8u, g_hw.logicalCores / 4));
\t\t\t\tthreads = g_settings.highEndMode ? cpuScale : std::min(4u, cpuScale);
\t\t\t} else {
\t\t\t\tthreads = drive == HardwareProfile::Drive::SataSsd ? 2 : 1;
\t\t\t}
\t\t} else {
\t\t\tthreads = 1;
\t\t}
\t}
\tif (g_settings.directStorageWarmRead && DirectStorageAvailable())
\t\tthreads = 1; // DirectStorage batches internally through one queue.
\tthreads = std::max(1u, std::min({ threads, 8u, (unsigned)std::max<size_t>(plan.files.size(), 1) }));'''.replace('\\t','\t')
    text = replace_once(text,
        '\t\tconst int64_t left = g_warmBudgetLeft.load(std::memory_order_relaxed);\n\t\tif (left <= 0)\n\t\t\treturn;\n\t\tconst auto& entry = plan->files[idx];\n\t\tuint64_t want = std::min(entry.second, plan->perFileBytes);\n\t\twant = std::min(want, (uint64_t)left);\n\t\tif (!want)\n\t\t\tcontinue;\n\t\tconst uint64_t got = WarmOneFile(entry.first, want, plan->mappedPrefetch);\n\t\tif (got) {\n\t\t\tg_warmBudgetLeft.fetch_sub((int64_t)got, std::memory_order_relaxed);\n\t\t\tg_warmBytes.fetch_add(got, std::memory_order_relaxed);\n\t\t\tg_warmFilesTouched.fetch_add(1, std::memory_order_relaxed);\n\t\t}',
        '\t\tconst auto& entry = plan->files[idx];\n\t\tuint64_t want = std::min(entry.second, plan->perFileBytes);\n\t\twant = ReserveWarmBudget(want);\n\t\tif (!want)\n\t\t\treturn;\n\t\tconst uint64_t got = WarmOneFile(entry.first, want, plan->mappedPrefetch);\n\t\tif (got < want)\n\t\t\tg_warmBudgetLeft.fetch_add(static_cast<int64_t>(want - got), std::memory_order_relaxed);\n\t\tif (got) {\n\t\t\tg_warmBytes.fetch_add(got, std::memory_order_relaxed);\n\t\t\tg_warmFilesTouched.fetch_add(1, std::memory_order_relaxed);\n\t\t}',
        "atomic budget accounting")

    text = replace_once(text, old_threads, new_threads, "thread tuning")
    text = replace_once(text,
        "\tLog(\"WarmCache: warmed %llu MB across %u files in %llu ms (%u thread%s)\",\n\t\t(unsigned long long)(g_warmBytes.load() >> 20),\n\t\tg_warmFilesTouched.load(),\n\t\t(unsigned long long)elapsed,\n\t\tplan.threads, plan.threads == 1 ? \"\" : \"s\");\n}",
        "\tLog(\"WarmCache: warmed %llu MB across %u files in %llu ms (%u thread%s)\",\n\t\t(unsigned long long)(g_warmBytes.load() >> 20),\n\t\tg_warmFilesTouched.load(),\n\t\t(unsigned long long)elapsed,\n\t\tplan.threads, plan.threads == 1 ? \"\" : \"s\");\n\tif (g_settings.logStatsAfterWarm) {\n\t\tLog(\"Stats snapshot: opens=%llu patched=%llu no_buffering_stripped=%llu warm_read=%llu MB\",\n\t\t\t(unsigned long long)g_opensTotal.load(),\n\t\t\t(unsigned long long)g_opensPatched.load(),\n\t\t\t(unsigned long long)g_noBufferingStripped.load(),\n\t\t\t(unsigned long long)(g_warmBytes.load() >> 20));\n\t}\n}",
        "post-warm stats snapshot")

    load_anchor = """\tif (g_settings.hardwareProfile) {
\t\tDetectCpu(g_hw);
\t\tDetectRam(g_hw);
\t\tDetectGameDrive(g_hw);
\t\tDetectGpuBars(g_hw);
\t\tDetectOs(g_hw);
\t\tLogHardwareProfile(g_hw);
\t\tApplyFastCorePreference(g_hw);
\t} else {"""
    load_repl = """\tif (g_settings.hardwareProfile) {
\t\tDetectCpu(g_hw);
\t\tDetectRam(g_hw);
\t\tDetectGameDrive(g_hw);
\t\tDetectGpuBars(g_hw);
\t\tDetectOs(g_hw);
\t\tLogHardwareProfile(g_hw);
\t\tApplyFastCorePreference(g_hw);
\t} else {"""
    text = replace_once(text, load_anchor, load_repl, "hardware load anchor")
    text = replace_once(text,
        "\t\tLog(\"GPU: Resizable BAR ACTIVE - full-VRAM CPU window; texture uploads take the fast path.\");",
        "\t\tLog(\"GPU: Resizable BAR appears ACTIVE based on BAR size; diagnostics only for this CPU/file-cache plugin.\");",
        "honest ReBAR log")

    text = replace_once(text,
        "\tApplyProcessHints();\n\n\tif (g_settings.enableWarmCache",
        "\tApplyProcessHints();\n\n\tif (g_settings.directStorageProbe) {\n\t\tconst auto ds = DirectStorageInitialize(\n\t\t\tg_settings.directStorageQueueCapacity, g_settings.directStorageBatchMB,\n\t\t\tg_settings.directStorageWarmRead);\n\t\tif (ds.memoryQueueCreated)\n\t\t\tLog(\"DirectStorage: runtime ready at %ls; low-priority system-memory queue created [EXPERIMENTAL RAW READ ENABLED]\", ds.runtimePath);\n\t\telse if (ds.factoryCreated && !g_settings.directStorageWarmRead)\n\t\t\tLog(\"DirectStorage: runtime validated at %ls; queue not created because raw reads are disabled\", ds.runtimePath);\n\t\telse if (ds.runtimeDllPresent)\n\t\t\tLog(\"DirectStorage: runtime found at %ls but requested initialization failed hr=0x%08X\", ds.runtimePath, (unsigned)ds.result);\n\t\telse\n\t\t\tLog(\"DirectStorage: runtime not found; PrefetchVirtualMemory remains active\");\n\t}\n\n\tif (g_settings.enableWarmCache",
        "DirectStorage initialization")

    old_load_start = r'''SKSEAPI bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	(void)skse;
	// Hooks already attached from DllMain (before most file I/O).
	if (g_hookAttachFailed)
		Log("WARNING: CreateFile hooks did not attach - running detection/hints only");

	DetectSiblingPlugins();'''
    new_load_start = r'''SKSEAPI bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	LoadSettings();
	OpenLog();
	ResolveDynamicApis();
	Log("NextGen Disk Cache " PLUGIN_VERSION_STRING " - safe SKSE load path");
	if (!AttachHooks()) {
		g_hookAttachFailed = true;
		Log("WARNING: CreateFile hooks did not attach - running detection/hints only");
	}

	DetectSiblingPlugins();'''
    text = replace_once(text, old_load_start, new_load_start, "loader-lock move")
    text = replace_once(text,
        "\tif (g_settings.enableWarmCache && !g_warmThread.joinable()) {\n\t\tg_warmThread = std::thread(WarmCacheCoordinator);\n\t\tLog(\"WarmCache thread started (delay %u s)\", g_settings.warmCacheDelaySecs);\n\t}",
        "\tbool expectedWarm = false;\n\tif (g_settings.enableWarmCache &&\n\t\tg_warmStarted.compare_exchange_strong(expectedWarm, true)) {\n\t\tstd::thread(WarmCacheCoordinator).detach();\n\t\tLog(\"WarmCache thread started (delay %u s)\", g_settings.warmCacheDelaySecs);\n\t}",
        "detached warm start")

    bottom_start = text.index("// ---------------------------------------------------------------------------\n// DllMain - attach CreateFile hooks as early as possible")
    new_bottom = r'''// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	if (DetourIsHelperProcess())
		return TRUE;
	if (dwReason == DLL_PROCESS_ATTACH) {
		g_selfModule = hInstance;
		DisableThreadLibraryCalls(hInstance);
		// No file I/O, Detours transactions, COM, threads, or logging under loader lock.
	} else if (dwReason == DLL_PROCESS_DETACH) {
		// SKSE does not hot-unload plugins. At process exit Windows reclaims handles,
		// mappings, COM objects, and detached workers without blocking loader lock.
		g_shutdown.store(true, std::memory_order_relaxed);
	}
	return TRUE;
}
'''
    text = text[:bottom_start] + new_bottom

    path.write_text(text, encoding="utf-8")


def patch_build(repo: pathlib.Path) -> None:
    path = repo / "build.ps1"
    text = path.read_text(encoding="utf-8-sig")
    new = r'''# Build NextGenDiskCache 1.2.0 and stage a clean FOMOD payload.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
if (-not $nuget) { throw "nuget.exe is required (winget install Microsoft.NuGet)" }
$ds = Join-Path $here "deps\directstorage"
if (-not (Test-Path (Join-Path $ds "native\include\dstorage.h"))) {
    & $nuget.Source install Microsoft.Direct3D.DirectStorage -Version 1.3.0 `
        -OutputDirectory (Join-Path $here "deps\_nuget") -NonInteractive
    $pkg = Join-Path $here "deps\_nuget\Microsoft.Direct3D.DirectStorage.1.3.0"
    if (Test-Path $ds) { Remove-Item $ds -Recurse -Force }
    Copy-Item $pkg $ds -Recurse
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022 Build Tools" }
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
    -property installationPath
if (-not $vs) { throw "Visual Studio 2022 with C++ tools not found" }
$vsDev = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

$cmd = @"
call "$vsDev" -arch=amd64 -host_arch=amd64 >nul
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
"@
$tmp = Join-Path $env:TEMP "ngdc_build_12.cmd"
Set-Content -Path $tmp -Value $cmd -Encoding ASCII
& cmd.exe /c $tmp
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

$dll = Join-Path $here "package\SKSE\Plugins\NextGenDiskCache.dll"
if (-not (Test-Path $dll)) { throw "DLL missing after build: $dll" }
Write-Host "BUILD OK: $dll"
'''
    path.write_text(new, encoding="utf-8")


def patch_gitignore(repo: pathlib.Path) -> None:
    path = repo / ".gitignore"
    text = path.read_text(encoding="utf-8-sig") if path.exists() else ""
    additions = ["deps/directstorage/", "deps/_nuget/", "dist/"]
    for item in additions:
        if item not in text.splitlines():
            text += ("" if text.endswith("\n") or not text else "\n") + item + "\n"
    path.write_text(text, encoding="utf-8")


def patch_ini(repo: pathlib.Path) -> None:
    src = HERE / "profiles/HighEnd/NextGenDiskCache.ini"
    dst = repo / "package/SKSE/Plugins/NextGenDiskCache.ini"
    shutil.copy2(src, dst)


def patch_docs(repo: pathlib.Path) -> None:
    readme = repo / "README.md"
    text = readme.read_text(encoding="utf-8-sig")
    insert = (HERE / "docs/README-1.2-INSERT.md").read_text(encoding="utf-8")
    marker = "## 1.1.0 — modern hardware support"
    if marker not in text:
        raise RuntimeError("README: 1.1 section marker not found")
    text = text.replace(marker, insert + "\n\n" + marker, 1)
    text = text.replace("Visual Studio 2022/2026 + CMake", "Visual Studio 2022 + CMake + NuGet")
    text = text.replace(
        "Warm reads prime the OS page cache *and* the drive's own DRAM/SLC layer. Reads are tagged **IoPriorityHintLow** so the game always outranks them.",
        "Warm reads prime the Windows file cache. They may also interact with an SSD's firmware-managed DRAM/SLC cache, but that cache is not directly controllable or generically detectable from this plugin.")
    readme.write_text(text, encoding="utf-8")

    changelog = repo / "CHANGELOG.txt"
    old = changelog.read_text(encoding="utf-8-sig")
    entry = (HERE / "docs/CHANGELOG-1.2-ENTRY.txt").read_text(encoding="utf-8")
    changelog.write_text(entry + "\n" + old, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", type=pathlib.Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    required = [repo / "src/main.cpp", repo / "CMakeLists.txt", repo / "build.ps1"]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        raise SystemExit("Not a NextGen-Disk-Cache checkout; missing: " + ", ".join(missing))

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = repo.parent / f"{repo.name}-backup-{stamp}"
    shutil.copytree(repo, backup, ignore=shutil.ignore_patterns("build", ".git", "*.pdb", "*.dll"))
    print(f"Backup: {backup}")

    copy_overlay(repo)
    patch_cmake(repo)
    patch_main(repo)
    patch_build(repo)
    patch_gitignore(repo)
    patch_ini(repo)
    patch_docs(repo)
    print("NextGen Disk Cache 1.2.0 source RC applied successfully.")
    print("Next: run .\\package-release.ps1 to build and create the validated FOMOD.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise
