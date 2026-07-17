// NextGen Disk Cache - SKSE64 plugin
// Modified derivative of "Disk Cache Enabler"
//
//   Created by:  Archost
//   Uploaded by: enpinion
//
// ISC License
//
// Copyright 2023 Archost
//
// Permission to use, copy, modify, and /or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Uses Microsoft Detours (MIT) + SKSE headers.
//
// Core behaviour (Archost): stop FILE_FLAG_NO_BUFFERING / prefer cache-friendly
// CreateFile flags. Extensions: CreateFileW, path classes, process hints,
// optional warm-cache, INI, file logging.
//
// 1.1.0: hardware profiler (X3D / L3 topology, DDR generation + speed via
// SMBIOS, NVMe/SATA/HDD game drive, GPU BAR size -> Resizable BAR state),
// auto-tuned multi-threaded warm cache with PrefetchVirtualMemory, low I/O
// priority warm reads, opt-in fast-core (V-Cache CCD / P-core) preference.
//
// ReBAR / XMP-EXPO / x3D themselves are BIOS / silicon - this plugin DETECTS
// and ADAPTS to them; it cannot switch them on.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <fstream>
#include <algorithm>

#include <Windows.h>
#include <ShlObj.h>
#include <winioctl.h>
#include <intrin.h>

// SetupAPI / cfgmgr32 for GPU BAR (Resizable BAR) detection.
// initguid.h must precede devguid.h so GUID_DEVCLASS_DISPLAY gets instantiated.
#include <initguid.h>
#include <devguid.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>

// SKSE (bundled headers from Disk Cache Enabler deps - Query/Load + AE version data)
#include <skse/common/IPrefix.h>
#include <skse/skse64/PluginAPI.h>
#include <skse/skse64_common/skse_version.h>

// Detours
#include <detours/detours.h>

#define SKSEAPI extern "C" __declspec(dllexport)

#define PLUGIN_NAME    "NextGen Disk Cache"
// Original author first (Archost). Uploader on Nexus: enpinion.
#define PLUGIN_AUTHOR  "Archost (mod: enpinion upload; derivative)"
#define PLUGIN_VERSION ((1u << 16) | (1u << 8) | 0u) // 1.1.0
#define PLUGIN_VERSION_STRING "1.1.0"

// ---------------------------------------------------------------------------
// Settings (INI)
// ---------------------------------------------------------------------------
struct Settings {
	bool enableFileCacheHooks = true;
	bool enableCreateFileW = true;
	bool enableCreateFileA = true;
	bool stripNoBuffering = true;
	bool preferRandomAccessOnAssets = true;
	bool leaveSequentialOnLogs = true;

	bool disablePowerThrottling = true;
	bool raiseMemoryPriority = true;
	bool expandWorkingSet = true;
	// Fraction of physical RAM allowed as max working set (0.10–0.75). 0 = skip.
	double workingSetMaxFraction = 0.50;
	// Min working set in MB (soft; 0 = leave Windows default min).
	unsigned workingSetMinMB = 128;

	bool hardwareProfile = true;   // detect + log; required for auto-tune
	bool autoTune = true;          // scale warm cache from the profile
	bool preferFastCores = false;  // OPT-IN: V-Cache CCD / P-core CPU-set hint

	bool enableWarmCache = true;
	unsigned warmCacheDelaySecs = 8;
	unsigned warmCacheMaxFiles = 256;
	unsigned warmCacheBytesPerFileMB = 8;
	unsigned warmCacheBudgetMB = 0;   // total budget; 0 = auto from profile
	unsigned warmCacheThreads = 0;    // 0 = auto (NVMe 4 / SATA SSD 2 / HDD 1)
	bool warmCacheMappedPrefetch = true;
	bool warmCacheLowIoPriority = true;
	unsigned warmCacheThreadPriority = THREAD_PRIORITY_LOWEST;

	bool logToFile = true;
	bool logEveryOpen = false; // debug only - very spammy
	bool logStatsOnExit = true;
};

static Settings g_settings;
static std::mutex g_logMutex;
static std::ofstream g_log;
static std::atomic<bool> g_shutdown{false};
static std::atomic<uint64_t> g_opensTotal{0};
static std::atomic<uint64_t> g_opensPatched{0};
static std::atomic<uint64_t> g_noBufferingStripped{0};
static std::thread g_warmThread;
static HMODULE g_selfModule = nullptr;
static bool g_hookAttachFailed = false;

// Newer kernel32 APIs resolved at runtime so the DLL still loads on Win7/8.1.
using PrefetchVirtualMemory_t = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
using GetSystemCpuSetInformation_t = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using SetProcessDefaultCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using SetThreadInformation_t = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
static PrefetchVirtualMemory_t g_pPrefetchVirtualMemory = nullptr;
static GetSystemCpuSetInformation_t g_pGetSystemCpuSetInformation = nullptr;
static SetProcessDefaultCpuSets_t g_pSetProcessDefaultCpuSets = nullptr;
static SetThreadInformation_t g_pSetThreadInformation = nullptr;

static void ResolveDynamicApis()
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32)
		return;
	g_pPrefetchVirtualMemory =
		reinterpret_cast<PrefetchVirtualMemory_t>(GetProcAddress(k32, "PrefetchVirtualMemory"));
	g_pGetSystemCpuSetInformation =
		reinterpret_cast<GetSystemCpuSetInformation_t>(GetProcAddress(k32, "GetSystemCpuSetInformation"));
	g_pSetProcessDefaultCpuSets =
		reinterpret_cast<SetProcessDefaultCpuSets_t>(GetProcAddress(k32, "SetProcessDefaultCpuSets"));
	g_pSetThreadInformation =
		reinterpret_cast<SetThreadInformation_t>(GetProcAddress(k32, "SetThreadInformation"));
}

static void Log(const char* fmt, ...)
{
	if (!g_settings.logToFile)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	SYSTEMTIME st{};
	GetLocalTime(&st);
	char line[1200];
	snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\n",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);

	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_log.is_open()) {
		g_log << line;
		g_log.flush();
	}
}

static std::string GetSkseLogPath()
{
	char docs[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
		return {};
	// Prefer AE path; fall back to GOG folder name if present.
	std::string base = std::string(docs) + "\\My Games\\Skyrim Special Edition\\SKSE";
	DWORD attr = GetFileAttributesA(base.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		std::string gog = std::string(docs) + "\\My Games\\Skyrim Special Edition GOG\\SKSE";
		attr = GetFileAttributesA(gog.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
			base = gog;
		else
			CreateDirectoryA(base.c_str(), nullptr);
	}
	return base + "\\NextGenDiskCache.log";
}

static void OpenLog()
{
	if (!g_settings.logToFile)
		return;
	const auto path = GetSkseLogPath();
	if (path.empty())
		return;
	std::lock_guard<std::mutex> lock(g_logMutex);
	g_log.open(path, std::ios::out | std::ios::trunc);
}

// ---------------------------------------------------------------------------
// INI
// ---------------------------------------------------------------------------
static bool ParseBool(const char* v, bool def)
{
	if (!v || !*v)
		return def;
	if (_stricmp(v, "1") == 0 || _stricmp(v, "true") == 0 || _stricmp(v, "yes") == 0 || _stricmp(v, "on") == 0)
		return true;
	if (_stricmp(v, "0") == 0 || _stricmp(v, "false") == 0 || _stricmp(v, "no") == 0 || _stricmp(v, "off") == 0)
		return false;
	return def;
}

static void LoadIniFromPath(const char* path)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "r") != 0 || !f)
		return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char* p = line;
		while (*p == ' ' || *p == '\t')
			++p;
		if (*p == ';' || *p == '#' || *p == '[' || *p == '\r' || *p == '\n' || *p == 0)
			continue;
		char* eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = 0;
		char* key = p;
		char* val = eq + 1;
		// trim key
		char* ke = key + strlen(key);
		while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
			*--ke = 0;
		// trim val
		while (*val == ' ' || *val == '\t')
			++val;
		char* ve = val + strlen(val);
		while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n'))
			*--ve = 0;

		if (_stricmp(key, "bEnableFileCacheHooks") == 0)
			g_settings.enableFileCacheHooks = ParseBool(val, true);
		else if (_stricmp(key, "bEnableCreateFileW") == 0)
			g_settings.enableCreateFileW = ParseBool(val, true);
		else if (_stricmp(key, "bEnableCreateFileA") == 0)
			g_settings.enableCreateFileA = ParseBool(val, true);
		else if (_stricmp(key, "bStripNoBuffering") == 0)
			g_settings.stripNoBuffering = ParseBool(val, true);
		else if (_stricmp(key, "bPreferRandomAccessOnAssets") == 0)
			g_settings.preferRandomAccessOnAssets = ParseBool(val, true);
		else if (_stricmp(key, "bLeaveSequentialOnLogs") == 0)
			g_settings.leaveSequentialOnLogs = ParseBool(val, true);
		else if (_stricmp(key, "bDisablePowerThrottling") == 0)
			g_settings.disablePowerThrottling = ParseBool(val, true);
		else if (_stricmp(key, "bRaiseMemoryPriority") == 0)
			g_settings.raiseMemoryPriority = ParseBool(val, true);
		else if (_stricmp(key, "bExpandWorkingSet") == 0)
			g_settings.expandWorkingSet = ParseBool(val, true);
		else if (_stricmp(key, "fWorkingSetMaxFraction") == 0)
			g_settings.workingSetMaxFraction = atof(val);
		else if (_stricmp(key, "iWorkingSetMinMB") == 0)
			g_settings.workingSetMinMB = (unsigned)atoi(val);
		else if (_stricmp(key, "bHardwareProfile") == 0)
			g_settings.hardwareProfile = ParseBool(val, true);
		else if (_stricmp(key, "bAutoTune") == 0)
			g_settings.autoTune = ParseBool(val, true);
		else if (_stricmp(key, "bPreferFastCores") == 0)
			g_settings.preferFastCores = ParseBool(val, false);
		else if (_stricmp(key, "bEnableWarmCache") == 0)
			g_settings.enableWarmCache = ParseBool(val, true);
		else if (_stricmp(key, "iWarmCacheDelaySecs") == 0)
			g_settings.warmCacheDelaySecs = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheMaxFiles") == 0)
			g_settings.warmCacheMaxFiles = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheBytesPerFileMB") == 0)
			g_settings.warmCacheBytesPerFileMB = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheBudgetMB") == 0)
			g_settings.warmCacheBudgetMB = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheThreads") == 0)
			g_settings.warmCacheThreads = (unsigned)atoi(val);
		else if (_stricmp(key, "bWarmCacheMappedPrefetch") == 0)
			g_settings.warmCacheMappedPrefetch = ParseBool(val, true);
		else if (_stricmp(key, "bWarmCacheLowIoPriority") == 0)
			g_settings.warmCacheLowIoPriority = ParseBool(val, true);
		else if (_stricmp(key, "bLogToFile") == 0)
			g_settings.logToFile = ParseBool(val, true);
		else if (_stricmp(key, "bLogEveryOpen") == 0)
			g_settings.logEveryOpen = ParseBool(val, false);
		else if (_stricmp(key, "bLogStatsOnExit") == 0)
			g_settings.logStatsOnExit = ParseBool(val, true);
	}
	fclose(f);
}

static void LoadSettings()
{
	// DLL-adjacent INI: SKSE\Plugins\NextGenDiskCache.ini
	char modPath[MAX_PATH] = {};
	if (g_selfModule && GetModuleFileNameA(g_selfModule, modPath, MAX_PATH)) {
		std::string ini = modPath;
		const auto slash = ini.find_last_of("\\/");
		if (slash != std::string::npos)
			ini = ini.substr(0, slash + 1);
		ini += "NextGenDiskCache.ini";
		LoadIniFromPath(ini.c_str());
	}
}

// ---------------------------------------------------------------------------
// Path classification
// ---------------------------------------------------------------------------
enum class PathKind { Asset, LogOrTemp, Other };

static PathKind ClassifyExt(const char* e)
{
	// Logs / temp / pure append - leave sequential scan alone if present
	if (strcmp(e, ".log") == 0 || strcmp(e, ".tmp") == 0 || strcmp(e, ".temp") == 0 ||
		strcmp(e, ".dmp") == 0 || strcmp(e, ".txt") == 0)
		return PathKind::LogOrTemp;

	// Game / mod assets that benefit from OS cache + random access
	static const char* kAssets[] = {
		".bsa", ".ba2", ".esm", ".esp", ".esl",
		".nif", ".dds", ".hkx", ".tri", ".fuz", ".lip", ".xwm", ".wav",
		".pex", ".psc", ".swf", ".gfx", ".seq",
		".ini", ".json", ".bin", ".dat", ".db",
		".skse", ".cosave", ".ess", ".bak",
		nullptr
	};
	for (int i = 0; kAssets[i]; ++i) {
		if (strcmp(e, kAssets[i]) == 0)
			return PathKind::Asset;
	}
	return PathKind::Other;
}

static PathKind ClassifyPathA(const char* path)
{
	if (!path || !*path)
		return PathKind::Other;
	// extension only from final component
	const char* dot = nullptr;
	for (const char* p = path; *p; ++p) {
		if (*p == '.')
			dot = p;
		if (*p == '\\' || *p == '/')
			dot = nullptr;
	}
	if (!dot)
		return PathKind::Other;
	char e[16] = {};
	size_t n = 0;
	for (const char* p = dot; *p && n + 1 < sizeof(e); ++p)
		e[n++] = (char)tolower((unsigned char)*p);
	e[n] = 0;
	return ClassifyExt(e);
}

static PathKind ClassifyPathW(const wchar_t* path)
{
	// Extract the extension in wide chars first - converting the whole path to
	// a fixed buffer truncated long mod paths and lost the extension entirely.
	if (!path || !*path)
		return PathKind::Other;
	const wchar_t* dot = nullptr;
	for (const wchar_t* p = path; *p; ++p) {
		if (*p == L'.')
			dot = p;
		if (*p == L'\\' || *p == L'/')
			dot = nullptr;
	}
	if (!dot)
		return PathKind::Other;
	char e[16] = {};
	size_t n = 0;
	for (const wchar_t* p = dot; *p && n + 1 < sizeof(e); ++p) {
		wchar_t c = *p;
		if (c > 127)
			return PathKind::Other; // non-ASCII extension - not a game asset type
		e[n++] = (char)tolower((int)c);
	}
	e[n] = 0;
	return ClassifyExt(e);
}

static DWORD PatchFlags(DWORD flags, PathKind kind)
{
	DWORD out = flags;
	if (g_settings.stripNoBuffering && (out & FILE_FLAG_NO_BUFFERING)) {
		out &= ~FILE_FLAG_NO_BUFFERING;
		g_noBufferingStripped.fetch_add(1, std::memory_order_relaxed);
	}

	if (kind == PathKind::LogOrTemp && g_settings.leaveSequentialOnLogs) {
		// Keep SEQUENTIAL_SCAN if the caller set it; do not force RANDOM_ACCESS.
		return out;
	}

	if (g_settings.preferRandomAccessOnAssets &&
		(kind == PathKind::Asset || kind == PathKind::Other)) {
		out &= ~FILE_FLAG_SEQUENTIAL_SCAN;
		out |= FILE_FLAG_RANDOM_ACCESS;
	}
	return out;
}

// ---------------------------------------------------------------------------
// CreateFile hooks
// ---------------------------------------------------------------------------
static HANDLE(WINAPI* CreateFileA_orig)(
	LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = CreateFileA;

static HANDLE(WINAPI* CreateFileW_orig)(
	LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = CreateFileW;

static HANDLE WINAPI CreateFileA_hook(
	LPCSTR lpFilename,
	DWORD dwDesiredAccess,
	DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile)
{
	g_opensTotal.fetch_add(1, std::memory_order_relaxed);
	DWORD flags = dwFlagsAndAttributes;
	if (g_settings.enableFileCacheHooks && g_settings.enableCreateFileA) {
		const PathKind kind = ClassifyPathA(lpFilename);
		const DWORD patched = PatchFlags(flags, kind);
		if (patched != flags)
			g_opensPatched.fetch_add(1, std::memory_order_relaxed);
		flags = patched;
		if (g_settings.logEveryOpen && lpFilename)
			Log("CreateFileA flags %08X->%08X %s", dwFlagsAndAttributes, flags, lpFilename);
	}
	return CreateFileA_orig(
		lpFilename, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, flags, hTemplateFile);
}

static HANDLE WINAPI CreateFileW_hook(
	LPCWSTR lpFilename,
	DWORD dwDesiredAccess,
	DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile)
{
	g_opensTotal.fetch_add(1, std::memory_order_relaxed);
	DWORD flags = dwFlagsAndAttributes;
	if (g_settings.enableFileCacheHooks && g_settings.enableCreateFileW) {
		const PathKind kind = ClassifyPathW(lpFilename);
		const DWORD patched = PatchFlags(flags, kind);
		if (patched != flags)
			g_opensPatched.fetch_add(1, std::memory_order_relaxed);
		flags = patched;
		if (g_settings.logEveryOpen && lpFilename)
			Log("CreateFileW flags %08X->%08X", dwFlagsAndAttributes, flags);
	}
	return CreateFileW_orig(
		lpFilename, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, flags, hTemplateFile);
}

// ---------------------------------------------------------------------------
// Hardware profile - detect modern PC tech and adapt to it
// ---------------------------------------------------------------------------
struct HardwareProfile {
	// CPU
	char cpuBrand[64] = {};
	unsigned physicalCores = 0;
	unsigned logicalCores = 0;
	struct L3Domain {
		WORD group = 0;
		KAFFINITY mask = 0;
		uint64_t bytes = 0;
	};
	std::vector<L3Domain> l3;
	uint64_t l3TotalBytes = 0;
	bool x3d = false;        // "X3D" in brand or any single L3 domain >= 64 MB
	bool dualCcdX3d = false; // two L3 domains, one much larger (7950X3D-style)
	bool hybridCpu = false;  // Intel P/E cores (EfficiencyClass spread)
	BYTE maxEfficiencyClass = 0;

	// RAM
	uint64_t ramTotalBytes = 0;
	unsigned ramModules = 0;
	unsigned ramSpeedMTs = 0;   // configured speed if SMBIOS provides it
	char ramTypeName[16] = {};  // "DDR5", "DDR4", …

	// Game drive
	enum class Drive { Unknown, HDD, SataSsd, NvmeSsd } drive = Drive::Unknown;

	// GPU / Resizable BAR
	struct Gpu {
		char name[128] = {};
		uint64_t largestBarBytes = 0;
	};
	std::vector<Gpu> gpus;
	int rebarState = -1; // -1 unknown, 0 likely off, 1 active

	// OS
	DWORD osMajor = 0;
	DWORD osBuild = 0;
};

static HardwareProfile g_hw;

static void DetectCpu(HardwareProfile& hw)
{
	int regs[4] = {};
	__cpuid(regs, (int)0x80000000);
	const unsigned maxExt = (unsigned)regs[0];
	if (maxExt >= 0x80000004u) {
		char brand[49] = {};
		for (int i = 0; i < 3; ++i) {
			__cpuid(regs, (int)(0x80000002u + i));
			memcpy(brand + i * 16, regs, 16);
		}
		const char* b = brand;
		while (*b == ' ')
			++b;
		strncpy_s(hw.cpuBrand, b, _TRUNCATE);
	}

	hw.logicalCores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

	// Physical cores + L3 topology
	for (int pass = 0; pass < 2; ++pass) {
		const LOGICAL_PROCESSOR_RELATIONSHIP rel = pass == 0 ? RelationProcessorCore : RelationCache;
		DWORD len = 0;
		GetLogicalProcessorInformationEx(rel, nullptr, &len);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len)
			continue;
		std::vector<BYTE> buf(len);
		if (!GetLogicalProcessorInformationEx(
				rel, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
			continue;
		BYTE* p = buf.data();
		const BYTE* end = buf.data() + len;
		while (p + sizeof(DWORD) * 2 <= end) {
			auto* info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
			if (info->Size == 0)
				break;
			if (info->Relationship == RelationProcessorCore) {
				++hw.physicalCores;
			} else if (info->Relationship == RelationCache && info->Cache.Level == 3 &&
				(info->Cache.Type == CacheUnified || info->Cache.Type == CacheData)) {
				const WORD grp = info->Cache.GroupMask.Group;
				const KAFFINITY mask = info->Cache.GroupMask.Mask;
				bool dup = false;
				for (const auto& d : hw.l3) {
					if (d.group == grp && d.mask == mask) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					HardwareProfile::L3Domain d;
					d.group = grp;
					d.mask = mask;
					d.bytes = info->Cache.CacheSize;
					hw.l3.push_back(d);
				}
			}
			p += info->Size;
		}
	}
	for (const auto& d : hw.l3)
		hw.l3TotalBytes += d.bytes;

	uint64_t l3Max = 0, l3Min = UINT64_MAX;
	for (const auto& d : hw.l3) {
		l3Max = std::max(l3Max, d.bytes);
		l3Min = std::min(l3Min, d.bytes);
	}
	hw.x3d = (hw.cpuBrand[0] && strstr(hw.cpuBrand, "X3D") != nullptr) ||
		l3Max >= 64ull * 1024 * 1024;
	hw.dualCcdX3d = hw.x3d && hw.l3.size() >= 2 && l3Min > 0 && l3Max >= l3Min * 2;

	// Intel hybrid (P/E cores): spread in CPU-set efficiency classes
	if (g_pGetSystemCpuSetInformation) {
		ULONG need = 0;
		g_pGetSystemCpuSetInformation(nullptr, 0, &need, GetCurrentProcess(), 0);
		if (need) {
			std::vector<BYTE> buf(need);
			if (g_pGetSystemCpuSetInformation(
					(PSYSTEM_CPU_SET_INFORMATION)buf.data(), need, &need, GetCurrentProcess(), 0)) {
				BYTE minCls = 255, maxCls = 0;
				BYTE* p = buf.data();
				const BYTE* end = buf.data() + need;
				while (p < end) {
					auto* e = (PSYSTEM_CPU_SET_INFORMATION)p;
					if (e->Size == 0)
						break;
					if (e->Type == CpuSetInformation) {
						minCls = std::min(minCls, e->CpuSet.EfficiencyClass);
						maxCls = std::max(maxCls, e->CpuSet.EfficiencyClass);
					}
					p += e->Size;
				}
				if (maxCls > minCls && minCls != 255) {
					hw.hybridCpu = true;
					hw.maxEfficiencyClass = maxCls;
				}
			}
		}
	}
}

static void DetectRam(HardwareProfile& hw)
{
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms))
		hw.ramTotalBytes = ms.ullTotalPhys;

	// SMBIOS Type 17 (Memory Device): DDR generation + configured speed (MT/s).
	// CAS timings (CL30 etc.) are SPD-only - not readable without a kernel driver.
	struct RawSMBIOSData {
		BYTE Used20CallingMethod;
		BYTE SMBIOSMajorVersion;
		BYTE SMBIOSMinorVersion;
		BYTE DmiRevision;
		DWORD Length;
		BYTE SMBIOSTableData[1];
	};
	const DWORD kRSMB = 'RSMB';
	UINT need = GetSystemFirmwareTable(kRSMB, 0, nullptr, 0);
	if (!need)
		return;
	std::vector<BYTE> buf(need);
	if (GetSystemFirmwareTable(kRSMB, 0, buf.data(), need) != need)
		return;
	auto* smb = (const RawSMBIOSData*)buf.data();
	if (buf.size() < sizeof(RawSMBIOSData) || smb->Length > need)
		return;

	const BYTE* p = smb->SMBIOSTableData;
	const BYTE* end = p + smb->Length;
	while (p + 4 <= end) {
		const BYTE type = p[0];
		const BYTE len = p[1];
		if (len < 4 || type == 127)
			break;
		if (type == 17 && len >= 0x13 && p + len <= end) {
			WORD sizeField = 0;
			memcpy(&sizeField, p + 0x0C, 2);
			if (sizeField != 0) { // populated slot only
				++hw.ramModules;
				const BYTE memType = p[0x12];
				const char* name = nullptr;
				switch (memType) {
				case 0x18: name = "DDR3"; break;
				case 0x1A: name = "DDR4"; break;
				case 0x1E: name = "LPDDR4"; break;
				case 0x22: name = "DDR5"; break;
				case 0x23: name = "LPDDR5"; break;
				default: break;
				}
				if (name && !hw.ramTypeName[0])
					strncpy_s(hw.ramTypeName, name, _TRUNCATE);
				WORD speed = 0;
				if (len >= 0x22)
					memcpy(&speed, p + 0x20, 2); // configured speed
				if (!speed && len >= 0x17)
					memcpy(&speed, p + 0x15, 2); // rated speed fallback
				if (speed && speed != 0xFFFF)
					hw.ramSpeedMTs = std::max(hw.ramSpeedMTs, (unsigned)speed);
			}
		}
		const BYTE* q = p + len;
		while (q + 1 < end && (q[0] || q[1]))
			++q;
		p = q + 2;
	}
}

static void DetectGameDrive(HardwareProfile& hw)
{
	wchar_t exe[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
		return;
	if (exe[1] != L':')
		return; // UNC / odd path - leave Unknown
	wchar_t volume[8] = { L'\\', L'\\', L'.', L'\\', exe[0], L':', 0, 0 };

	HANDLE h = CreateFileW_orig(volume, 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;

	bool isSsd = false, haveSeek = false;
	{
		STORAGE_PROPERTY_QUERY q{};
		q.PropertyId = StorageDeviceSeekPenaltyProperty;
		q.QueryType = PropertyStandardQuery;
		DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
				&q, sizeof(q), &d, sizeof(d), &ret, nullptr) && ret >= sizeof(d)) {
			haveSeek = true;
			isSsd = !d.IncursSeekPenalty;
		}
	}
	bool isNvme = false;
	{
		STORAGE_PROPERTY_QUERY q{};
		q.PropertyId = StorageAdapterProperty;
		q.QueryType = PropertyStandardQuery;
		STORAGE_ADAPTER_DESCRIPTOR d{};
		DWORD ret = 0;
		if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
				&q, sizeof(q), &d, sizeof(d), &ret, nullptr) && ret >= sizeof(d)) {
			isNvme = d.BusType == BusTypeNvme;
		}
	}
	CloseHandle(h);

	if (haveSeek) {
		if (!isSsd)
			hw.drive = HardwareProfile::Drive::HDD;
		else
			hw.drive = isNvme ? HardwareProfile::Drive::NvmeSsd : HardwareProfile::Drive::SataSsd;
	} else if (isNvme) {
		hw.drive = HardwareProfile::Drive::NvmeSsd;
	}
}

static void DetectGpuBars(HardwareProfile& hw)
{
	// Resizable BAR probe: the GPU's largest memory BAR. Non-ReBAR GPUs expose
	// a 256 MB window; ReBAR maps (nearly) all VRAM, so BAR >= 1 GB ⇒ active.
	HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
	if (devs == INVALID_HANDLE_VALUE)
		return;

	SP_DEVINFO_DATA did{};
	did.cbSize = sizeof(did);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &did); ++i) {
		HardwareProfile::Gpu gpu;
		wchar_t desc[128] = {};
		DWORD reqLen = 0;
		if (SetupDiGetDeviceRegistryPropertyW(devs, &did, SPDRP_DEVICEDESC, nullptr,
				(PBYTE)desc, sizeof(desc) - sizeof(wchar_t), &reqLen)) {
			WideCharToMultiByte(CP_UTF8, 0, desc, -1, gpu.name, sizeof(gpu.name) - 1,
				nullptr, nullptr);
		}

		LOG_CONF lc = 0;
		CONFIGRET cr = CM_Get_First_Log_Conf(&lc, did.DevInst, ALLOC_LOG_CONF);
		if (cr != CR_SUCCESS)
			cr = CM_Get_First_Log_Conf(&lc, did.DevInst, BOOT_LOG_CONF);
		if (cr == CR_SUCCESS) {
			const RESOURCEID resTypes[] = { ResType_Mem, ResType_MemLarge };
			for (RESOURCEID rt : resTypes) {
				RES_DES prev = (RES_DES)lc;
				RES_DES rd = 0;
				while (CM_Get_Next_Res_Des(&rd, prev, rt, nullptr, 0) == CR_SUCCESS) {
					ULONG sz = 0;
					if (CM_Get_Res_Des_Data_Size(&sz, rd, 0) == CR_SUCCESS && sz) {
						std::vector<BYTE> b(sz);
						if (CM_Get_Res_Des_Data(rd, b.data(), sz, 0) == CR_SUCCESS) {
							uint64_t span = 0;
							if (rt == ResType_Mem && sz >= sizeof(MEM_DES)) {
								MEM_DES md{};
								memcpy(&md, b.data(), sizeof(md));
								if (md.MD_Alloc_End >= md.MD_Alloc_Base)
									span = md.MD_Alloc_End - md.MD_Alloc_Base + 1;
							} else if (rt == ResType_MemLarge && sz >= sizeof(MEM_LARGE_DES)) {
								MEM_LARGE_DES md{};
								memcpy(&md, b.data(), sizeof(md));
								if (md.MLD_Alloc_End >= md.MLD_Alloc_Base)
									span = md.MLD_Alloc_End - md.MLD_Alloc_Base + 1;
							}
							gpu.largestBarBytes = std::max(gpu.largestBarBytes, span);
						}
					}
					if (prev != (RES_DES)lc)
						CM_Free_Res_Des_Handle(prev);
					prev = rd;
				}
				if (prev != (RES_DES)lc)
					CM_Free_Res_Des_Handle(prev);
			}
			CM_Free_Log_Conf_Handle(lc);
		}
		hw.gpus.push_back(gpu);
	}
	SetupDiDestroyDeviceInfoList(devs);

	uint64_t best = 0;
	for (const auto& g : hw.gpus)
		best = std::max(best, g.largestBarBytes);
	if (best >= 1024ull * 1024 * 1024)
		hw.rebarState = 1;
	else if (best >= 128ull * 1024 * 1024)
		hw.rebarState = 0;
}

static void DetectOs(HardwareProfile& hw)
{
	// RtlGetVersion - GetVersionEx lies under compatibility shims
	using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	auto p = ntdll
		? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"))
		: nullptr;
	if (!p)
		return;
	OSVERSIONINFOW vi{};
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (p(&vi) == 0) {
		hw.osMajor = vi.dwMajorVersion;
		hw.osBuild = vi.dwBuildNumber;
	}
}

static const char* DriveName(HardwareProfile::Drive d)
{
	switch (d) {
	case HardwareProfile::Drive::HDD: return "HDD (seek penalty)";
	case HardwareProfile::Drive::SataSsd: return "SATA SSD";
	case HardwareProfile::Drive::NvmeSsd: return "NVMe SSD";
	default: return "unknown";
	}
}

static void LogHardwareProfile(const HardwareProfile& hw)
{
	Log("=== Hardware profile ===");
	Log("CPU: %s (%uc/%ut, L3 %llu MB in %u domain%s)%s",
		hw.cpuBrand[0] ? hw.cpuBrand : "unknown",
		hw.physicalCores, hw.logicalCores,
		(unsigned long long)(hw.l3TotalBytes >> 20),
		(unsigned)hw.l3.size(), hw.l3.size() == 1 ? "" : "s",
		hw.x3d ? " [3D V-Cache]" : (hw.hybridCpu ? " [hybrid P/E]" : ""));
	if (hw.x3d)
		Log("CPU: X3D large cache detected - great for Skyrim's draw-call load; "
			"works automatically, nothing to configure.");
	if (hw.dualCcdX3d)
		Log("CPU: dual-CCD X3D - Windows/chipset driver parks the right CCD for games; "
			"bPreferFastCores=1 adds a soft CPU-set hint if you want to test it.");

	if (hw.ramTypeName[0] && hw.ramSpeedMTs)
		Log("RAM: %llu GB %s-%u (%u module%s) - CAS timings are SPD-only, not readable from user mode",
			(unsigned long long)(hw.ramTotalBytes >> 30), hw.ramTypeName, hw.ramSpeedMTs,
			hw.ramModules, hw.ramModules == 1 ? "" : "s");
	else
		Log("RAM: %llu GB (SMBIOS type/speed unavailable)",
			(unsigned long long)(hw.ramTotalBytes >> 30));
	if (hw.ramTypeName[0] && strcmp(hw.ramTypeName, "DDR5") == 0 && hw.ramSpeedMTs >= 4800 &&
		hw.ramSpeedMTs < 5600)
		Log("RAM: DDR5 running at %u MT/s - if your kit is rated higher (e.g. 6000), "
			"enable XMP/EXPO in BIOS.", hw.ramSpeedMTs);

	Log("Game drive: %s", DriveName(hw.drive));

	for (const auto& g : hw.gpus) {
		if (g.largestBarBytes)
			Log("GPU: %s - largest BAR %llu MB",
				g.name[0] ? g.name : "(display adapter)",
				(unsigned long long)(g.largestBarBytes >> 20));
		else
			Log("GPU: %s - BAR size unavailable", g.name[0] ? g.name : "(display adapter)");
	}
	if (hw.rebarState == 1)
		Log("GPU: Resizable BAR ACTIVE - full-VRAM CPU window; texture uploads take the fast path.");
	else if (hw.rebarState == 0)
		Log("GPU: BAR looks like the legacy 256 MB window - Resizable BAR appears OFF. "
			"Enable 'Resizable BAR' / 'Smart Access Memory' (+ Above 4G Decoding) in BIOS if supported.");
	else
		Log("GPU: Resizable BAR state unknown (BAR probe unavailable on this system).");

	Log("OS: Windows %lu build %lu%s", hw.osMajor, hw.osBuild,
		hw.osBuild >= 22000 ? " (Windows 11)" : "");
	Log("========================");
}

// ---------------------------------------------------------------------------
// Opt-in fast-core preference (dual-CCD X3D -> V-Cache CCD, hybrid -> P-cores)
// ---------------------------------------------------------------------------
static void ApplyFastCorePreference(const HardwareProfile& hw)
{
	if (!g_settings.preferFastCores)
		return;
	if (!g_pGetSystemCpuSetInformation || !g_pSetProcessDefaultCpuSets) {
		Log("FastCores: CPU-set APIs unavailable (needs Windows 10+) - skipped");
		return;
	}
	if (!hw.dualCcdX3d && !hw.hybridCpu) {
		Log("FastCores: CPU is neither dual-CCD X3D nor hybrid - nothing to prefer");
		return;
	}

	ULONG need = 0;
	g_pGetSystemCpuSetInformation(nullptr, 0, &need, GetCurrentProcess(), 0);
	if (!need)
		return;
	std::vector<BYTE> buf(need);
	if (!g_pGetSystemCpuSetInformation(
			(PSYSTEM_CPU_SET_INFORMATION)buf.data(), need, &need, GetCurrentProcess(), 0))
		return;

	// Dual-CCD X3D: prefer the CCD that owns the biggest L3 domain.
	const HardwareProfile::L3Domain* bigL3 = nullptr;
	if (hw.dualCcdX3d) {
		for (const auto& d : hw.l3) {
			if (!bigL3 || d.bytes > bigL3->bytes)
				bigL3 = &d;
		}
	}

	std::vector<ULONG> preferred;
	unsigned totalSets = 0;
	BYTE* p = buf.data();
	const BYTE* end = buf.data() + need;
	while (p < end) {
		auto* e = (PSYSTEM_CPU_SET_INFORMATION)p;
		if (e->Size == 0)
			break;
		if (e->Type == CpuSetInformation) {
			++totalSets;
			bool want = false;
			if (bigL3) {
				want = e->CpuSet.Group == bigL3->group &&
					((bigL3->mask >> e->CpuSet.LogicalProcessorIndex) & 1) != 0;
			} else if (hw.hybridCpu) {
				want = e->CpuSet.EfficiencyClass == hw.maxEfficiencyClass;
			}
			if (want)
				preferred.push_back(e->CpuSet.Id);
		}
		p += e->Size;
	}

	// Soft hint only; fewer than 4 preferred sets would strangle the game.
	if (preferred.size() < 4 || preferred.size() >= totalSets) {
		Log("FastCores: preferred set unusable (%u of %u) - skipped",
			(unsigned)preferred.size(), totalSets);
		return;
	}
	if (g_pSetProcessDefaultCpuSets(GetCurrentProcess(), preferred.data(), (ULONG)preferred.size()))
		Log("FastCores: default CPU sets -> %u of %u logical CPUs (%s). Soft hint - "
			"the scheduler may still use the rest.",
			(unsigned)preferred.size(), totalSets,
			bigL3 ? "V-Cache CCD" : "P-cores");
	else
		Log("FastCores: SetProcessDefaultCpuSets failed err=%lu", GetLastError());
}

// ---------------------------------------------------------------------------
// Process-level modern Windows hints
// ---------------------------------------------------------------------------
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif

static void ApplyProcessHints()
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32)
		return;

	using SetProcessInformation_t = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
	auto pSetProcessInformation =
		reinterpret_cast<SetProcessInformation_t>(GetProcAddress(k32, "SetProcessInformation"));

	if (g_settings.disablePowerThrottling && pSetProcessInformation) {
		// Disable EcoQoS / execution-speed power throttling for this process.
		struct {
			ULONG Version;
			ULONG ControlMask;
			ULONG StateMask;
		} pts{};
		pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		pts.StateMask = 0; // clear bit = not throttled
		// ProcessPowerThrottling = 4 on modern SDKs
		const PROCESS_INFORMATION_CLASS pic = (PROCESS_INFORMATION_CLASS)4;
		if (pSetProcessInformation(GetCurrentProcess(), pic, &pts, sizeof(pts)))
			Log("Power throttling (EcoQoS) disabled for Skyrim process");
		else
			Log("SetProcessInformation(PowerThrottling) failed err=%lu", GetLastError());
	}

	if (g_settings.raiseMemoryPriority && pSetProcessInformation) {
		// PROCESS_INFORMATION_CLASS: ProcessMemoryPriority = 0, ProcessPowerThrottling = 4
		// MEMORY_PRIORITY_NORMAL = 5 (winnt.h). Keeps pages from being deprioritized under pressure.
		struct {
			ULONG MemoryPriority;
		} mpi{};
		mpi.MemoryPriority = 5; // MEMORY_PRIORITY_NORMAL
		const PROCESS_INFORMATION_CLASS pic = (PROCESS_INFORMATION_CLASS)0; // ProcessMemoryPriority
		if (pSetProcessInformation(GetCurrentProcess(), pic, &mpi, sizeof(mpi)))
			Log("Process memory priority set to NORMAL (stable page residency)");
		else
			Log("SetProcessInformation(MemoryPriority) failed err=%lu (ignored)", GetLastError());
	}

	if (g_settings.expandWorkingSet) {
		MEMORYSTATUSEX ms{};
		ms.dwLength = sizeof(ms);
		if (GlobalMemoryStatusEx(&ms)) {
			double frac = g_settings.workingSetMaxFraction;
			if (frac < 0.10)
				frac = 0.10;
			if (frac > 0.75)
				frac = 0.75;
			SIZE_T maxWs = (SIZE_T)(ms.ullTotalPhys * frac);
			SIZE_T minWs = (SIZE_T)g_settings.workingSetMinMB * 1024ull * 1024ull;
			if (minWs > maxWs / 4)
				minWs = maxWs / 4;
			// Soft limits - do not hard-lock pages (that would fight the OS).
			if (SetProcessWorkingSetSizeEx(GetCurrentProcess(), minWs, maxWs, 0)) {
				Log("Working set range expanded: min=%u MB max=%u MB (%.0f%% of RAM)",
					(unsigned)(minWs / (1024 * 1024)),
					(unsigned)(maxWs / (1024 * 1024)),
					frac * 100.0);
			} else {
				Log("SetProcessWorkingSetSizeEx failed err=%lu (ignored)", GetLastError());
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Archive warm-cache (pre-fill Windows standby list / page cache)
// ---------------------------------------------------------------------------
static std::wstring GetGameDataPath()
{
	wchar_t exe[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
		return {};
	std::wstring path = exe;
	const auto slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return {};
	path = path.substr(0, slash + 1) + L"Data";
	return path;
}

struct WarmPlan {
	std::vector<std::pair<std::wstring, uint64_t>> files; // path, size (desc)
	uint64_t budgetBytes = 0;
	uint64_t perFileBytes = 0;
	unsigned threads = 1;
	bool mappedPrefetch = false;
};

static std::atomic<size_t> g_warmNext{0};
static std::atomic<int64_t> g_warmBudgetLeft{0};
static std::atomic<uint64_t> g_warmBytes{0};
static std::atomic<uint32_t> g_warmFilesTouched{0};
static const WarmPlan* g_warmPlan = nullptr;

static uint64_t WarmMappedPrefetch(HANDLE file, uint64_t bytes)
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
}

static uint64_t WarmOneFile(const std::wstring& path, uint64_t maxBytes, bool mapped)
{
	HANDLE h = CreateFileW_orig(
		path.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN | FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return 0;

	if (g_settings.warmCacheLowIoPriority) {
		// Game I/O always outranks the warm reader.
		FILE_IO_PRIORITY_HINT_INFO ph{};
		ph.PriorityHint = IoPriorityHintLow;
		SetFileInformationByHandle(h, FileIoPriorityHintInfo, &ph, sizeof(ph));
	}

	LARGE_INTEGER fs{};
	if (GetFileSizeEx(h, &fs) && (uint64_t)fs.QuadPart < maxBytes)
		maxBytes = (uint64_t)fs.QuadPart;
	if (!maxBytes) {
		CloseHandle(h);
		return 0;
	}

	uint64_t total = 0;
	if (mapped && g_pPrefetchVirtualMemory) {
		total = WarmMappedPrefetch(h, maxBytes);
	}
	if (!total) {
		std::vector<char> buf(1 << 20); // 1 MB chunk
		DWORD rd = 0;
		while (total < maxBytes &&
			ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0) {
			total += rd;
			if (g_shutdown.load(std::memory_order_relaxed))
				break;
		}
	}
	CloseHandle(h);
	return total;
}

static void WarmWorker()
{
	SetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);
	if (g_pSetThreadInformation) {
		// Warm reads are background work - let EcoQoS/E-cores have this thread.
		THREAD_POWER_THROTTLING_STATE ts{};
		ts.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
		ts.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		ts.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		g_pSetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &ts, sizeof(ts));
	}

	const WarmPlan* plan = g_warmPlan;
	if (!plan)
		return;
	for (;;) {
		if (g_shutdown.load(std::memory_order_relaxed))
			return;
		const size_t idx = g_warmNext.fetch_add(1, std::memory_order_relaxed);
		if (idx >= plan->files.size())
			return;
		const int64_t left = g_warmBudgetLeft.load(std::memory_order_relaxed);
		if (left <= 0)
			return;
		const auto& entry = plan->files[idx];
		uint64_t want = std::min(entry.second, plan->perFileBytes);
		want = std::min(want, (uint64_t)left);
		if (!want)
			continue;
		const uint64_t got = WarmOneFile(entry.first, want, plan->mappedPrefetch);
		if (got) {
			g_warmBudgetLeft.fetch_sub((int64_t)got, std::memory_order_relaxed);
			g_warmBytes.fetch_add(got, std::memory_order_relaxed);
			g_warmFilesTouched.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

static void BuildWarmPlan(WarmPlan& plan)
{
	const std::wstring data = GetGameDataPath();
	if (data.empty()) {
		Log("WarmCache: could not resolve Data path");
		return;
	}
	Log("WarmCache: scanning %ls", data.c_str());

	const unsigned maxFiles = g_settings.warmCacheMaxFiles;
	const wchar_t* patterns[] = { L"\\*.bsa", L"\\*.ba2", L"\\*.esm", L"\\*.esl", L"\\*.esp" };
	for (const wchar_t* pat : patterns) {
		WIN32_FIND_DATAW fd{};
		std::wstring query = data + pat;
		HANDLE find = FindFirstFileW(query.c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE)
			continue;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			ULARGE_INTEGER sz{};
			sz.HighPart = fd.nFileSizeHigh;
			sz.LowPart = fd.nFileSizeLow;
			plan.files.emplace_back(data + L"\\" + fd.cFileName, (uint64_t)sz.QuadPart);
			if (plan.files.size() >= maxFiles)
				break;
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		if (plan.files.size() >= maxFiles)
			break;
	}

	// Prefer larger archives first (more benefit per open).
	std::sort(plan.files.begin(), plan.files.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });

	// --- budget / threads / per-file cap ---
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof(ms);
	uint64_t totalMB = 8192, availMB = 2048;
	if (GlobalMemoryStatusEx(&ms)) {
		totalMB = ms.ullTotalPhys >> 20;
		availMB = ms.ullAvailPhys >> 20;
	}

	const auto drive = g_hw.drive;
	const bool haveProfile = g_settings.hardwareProfile;
	const bool tune = g_settings.autoTune && haveProfile;

	uint64_t budgetMB;
	if (g_settings.warmCacheBudgetMB) {
		budgetMB = g_settings.warmCacheBudgetMB;
	} else if (tune) {
		// Scale with RAM but never eat more than a quarter of what's free now.
		// Thresholds carry slack: a "32 GB" kit reports ~31.8 GiB usable.
		budgetMB = totalMB >= 60 * 1024 ? 8192
			: totalMB >= 30 * 1024 ? 4096
			: totalMB >= 14 * 1024 ? 1536
			: 512;
		if (drive == HardwareProfile::Drive::HDD)
			budgetMB = std::min<uint64_t>(budgetMB, 256); // don't grind a spinner
		budgetMB = std::min(budgetMB, availMB / 4);
	} else {
		// Legacy 1.0.x behavior: per-file cap × file count is the only limit.
		budgetMB = (uint64_t)g_settings.warmCacheBytesPerFileMB * std::max<size_t>(plan.files.size(), 1);
	}

	uint64_t perFileMB = g_settings.warmCacheBytesPerFileMB;
	if (tune) {
		const uint64_t byDrive = drive == HardwareProfile::Drive::NvmeSsd ? 64
			: drive == HardwareProfile::Drive::SataSsd ? 32
			: drive == HardwareProfile::Drive::HDD ? 8
			: 16;
		perFileMB = std::max(perFileMB, byDrive);
	}

	unsigned threads = g_settings.warmCacheThreads;
	if (!threads) {
		if (tune) {
			// NVMe loves queue depth; HDD hates concurrent readers.
			threads = drive == HardwareProfile::Drive::NvmeSsd ? 4
				: drive == HardwareProfile::Drive::SataSsd ? 2
				: 1;
		} else {
			threads = 1;
		}
	}
	threads = std::max(1u, std::min({ threads, 8u, (unsigned)std::max<size_t>(plan.files.size(), 1) }));

	plan.budgetBytes = budgetMB << 20;
	plan.perFileBytes = perFileMB << 20;
	plan.threads = threads;
	plan.mappedPrefetch = g_settings.warmCacheMappedPrefetch && g_pPrefetchVirtualMemory != nullptr;

	Log("WarmCache plan: %u files, budget %llu MB, per-file %llu MB, %u thread%s, %s%s",
		(unsigned)plan.files.size(),
		(unsigned long long)budgetMB, (unsigned long long)perFileMB,
		threads, threads == 1 ? "" : "s",
		plan.mappedPrefetch ? "mapped PrefetchVirtualMemory" : "ReadFile loop",
		tune ? " [auto-tuned]" : "");
}

static void WarmCacheCoordinator()
{
	const unsigned delay = g_settings.warmCacheDelaySecs;
	for (unsigned i = 0; i < delay * 10; ++i) {
		if (g_shutdown.load())
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	SetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);

	static WarmPlan plan;
	BuildWarmPlan(plan);
	if (plan.files.empty() || !plan.budgetBytes)
		return;

	g_warmPlan = &plan;
	g_warmNext.store(0);
	g_warmBudgetLeft.store((int64_t)plan.budgetBytes);

	const ULONGLONG t0 = GetTickCount64();
	std::vector<std::thread> workers;
	for (unsigned i = 1; i < plan.threads; ++i)
		workers.emplace_back(WarmWorker);
	WarmWorker(); // coordinator doubles as worker 0
	for (auto& w : workers) {
		if (w.joinable())
			w.join();
	}
	const ULONGLONG elapsed = GetTickCount64() - t0;

	Log("WarmCache: warmed %llu MB across %u files in %llu ms (%u thread%s)",
		(unsigned long long)(g_warmBytes.load() >> 20),
		g_warmFilesTouched.load(),
		(unsigned long long)elapsed,
		plan.threads, plan.threads == 1 ? "" : "s");
}

static void DetectSiblingPlugins()
{
	// Informational only - do not refuse to load.
	const char* peers[] = {
		"diskCacheEnabler.dll",
		"DiskCacheEnabler.dll",
		"BSAMemoryMap.dll",
		"FastDecompressSkyrim.dll",
		"FasterCellLookup.dll",
		"FasterLoadscreens.dll",
		nullptr
	};
	char dir[MAX_PATH] = {};
	if (!g_selfModule || !GetModuleFileNameA(g_selfModule, dir, MAX_PATH))
		return;
	std::string folder = dir;
	const auto slash = folder.find_last_of("\\/");
	if (slash != std::string::npos)
		folder = folder.substr(0, slash + 1);

	for (int i = 0; peers[i]; ++i) {
		std::string p = folder + peers[i];
		DWORD attr = GetFileAttributesA(p.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
			if (_stricmp(peers[i], "diskCacheEnabler.dll") == 0 ||
				_stricmp(peers[i], "DiskCacheEnabler.dll") == 0) {
				Log("NOTE: legacy DiskCacheEnabler.dll is also present. "
					"Remove it - NextGen Disk Cache supersedes it (double CreateFile hooks are wasteful).");
			} else if (_stricmp(peers[i], "BSAMemoryMap.dll") == 0) {
				Log("BSAMemoryMap.dll detected - complementary (mmap/decomp cache). "
					"NextGen only adjusts Win32 open flags / process cache policy.");
			} else {
				Log("Peer plugin present: %s", peers[i]);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Detours attach
// ---------------------------------------------------------------------------
static bool g_hooksAttached = false;

static bool AttachHooks()
{
	if (!g_settings.enableFileCacheHooks)
		return true;

	DetourRestoreAfterWith();
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	LONG err = NO_ERROR;
	if (g_settings.enableCreateFileA) {
		err = DetourAttach(&(PVOID&)CreateFileA_orig, CreateFileA_hook);
		if (err != NO_ERROR) {
			DetourTransactionAbort();
			Log("DetourAttach CreateFileA failed: %ld", err);
			return false;
		}
	}
	if (g_settings.enableCreateFileW) {
		err = DetourAttach(&(PVOID&)CreateFileW_orig, CreateFileW_hook);
		if (err != NO_ERROR) {
			DetourTransactionAbort();
			Log("DetourAttach CreateFileW failed: %ld", err);
			return false;
		}
	}
	err = DetourTransactionCommit();
	if (err != NO_ERROR) {
		Log("DetourTransactionCommit failed: %ld", err);
		return false;
	}
	g_hooksAttached = true;
	Log("CreateFile hooks attached (A=%d W=%d)",
		(int)g_settings.enableCreateFileA, (int)g_settings.enableCreateFileW);
	return true;
}

static void DetachHooks()
{
	if (!g_hooksAttached)
		return;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	if (g_settings.enableCreateFileA)
		DetourDetach(&(PVOID&)CreateFileA_orig, CreateFileA_hook);
	if (g_settings.enableCreateFileW)
		DetourDetach(&(PVOID&)CreateFileW_orig, CreateFileW_hook);
	DetourTransactionCommit();
	g_hooksAttached = false;
}

// ---------------------------------------------------------------------------
// SKSE exports
// ---------------------------------------------------------------------------
SKSEAPI const SKSEPluginVersionData SKSEPlugin_Version = {
	SKSEPluginVersionData::kVersion,
	PLUGIN_VERSION,
	PLUGIN_NAME,
	PLUGIN_AUTHOR,
	"",
	SKSEPluginVersionData::kVersionIndependentEx_NoStructUse,
	SKSEPluginVersionData::kVersionIndependent_Signatures,
	{ RUNTIME_VERSION_1_6_640,
	  RUNTIME_VERSION_1_5_97,
	  RUNTIME_VERSION_1_6_659_GOG,
	  0 },
	0
};

SKSEAPI bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = PLUGIN_NAME;
	info->version = PLUGIN_VERSION;
	(void)skse;
	return true;
}

SKSEAPI bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	(void)skse;
	// Hooks already attached from DllMain (before most file I/O).
	if (g_hookAttachFailed)
		Log("WARNING: CreateFile hooks did not attach - running detection/hints only");

	DetectSiblingPlugins();

	if (g_settings.hardwareProfile) {
		DetectCpu(g_hw);
		DetectRam(g_hw);
		DetectGameDrive(g_hw);
		DetectGpuBars(g_hw);
		DetectOs(g_hw);
		LogHardwareProfile(g_hw);
		ApplyFastCorePreference(g_hw);
	} else {
		Log("Hardware profile disabled (bHardwareProfile=0) - auto-tune inactive");
	}

	ApplyProcessHints();

	if (g_settings.enableWarmCache && !g_warmThread.joinable()) {
		g_warmThread = std::thread(WarmCacheCoordinator);
		Log("WarmCache thread started (delay %u s)", g_settings.warmCacheDelaySecs);
	}

	Log("NextGen Disk Cache " PLUGIN_VERSION_STRING
		" loaded (Archost/enpinion Disk Cache Enabler derivative) runtime=0x%08X skse=0x%08X",
		skse ? skse->runtimeVersion : 0, skse ? skse->skseVersion : 0);
	return true;
}

// ---------------------------------------------------------------------------
// DllMain - attach CreateFile hooks as early as possible
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	if (DetourIsHelperProcess())
		return TRUE;

	switch (dwReason) {
	case DLL_PROCESS_ATTACH:
		g_selfModule = hInstance;
		DisableThreadLibraryCalls(hInstance);
		LoadSettings();
		OpenLog();
		ResolveDynamicApis();
		Log("NextGen Disk Cache " PLUGIN_VERSION_STRING
			" - DllMain attach (based on Archost Disk Cache Enabler; uploaded by enpinion)");
		// No MessageBox here: UI calls under loader lock can deadlock.
		// Failure is logged now and again in SKSEPlugin_Load.
		if (!AttachHooks()) {
			g_hookAttachFailed = true;
			Log("ERROR: failed to attach CreateFile hooks - game runs without flag patching");
		}
		break;

	case DLL_PROCESS_DETACH:
		g_shutdown.store(true);
		if (g_warmThread.joinable()) {
			// Don't block unload forever
			g_warmThread.detach();
		}
		if (g_settings.logStatsOnExit) {
			Log("Stats: opens=%llu patched=%llu no_buffering_stripped=%llu warm_read=%llu MB",
				(unsigned long long)g_opensTotal.load(),
				(unsigned long long)g_opensPatched.load(),
				(unsigned long long)g_noBufferingStripped.load(),
				(unsigned long long)(g_warmBytes.load() >> 20));
		}
		DetachHooks();
		{
			std::lock_guard<std::mutex> lock(g_logMutex);
			if (g_log.is_open())
				g_log.close();
		}
		break;
	}
	return TRUE;
}
