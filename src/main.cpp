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
// bounded optional warm cache with PrefetchVirtualMemory, low I/O
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
#include <cerrno>
#include <climits>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <memory>
#include <cmath>

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
#include "DirectStorageBackend.h"

#define SKSEAPI extern "C" __declspec(dllexport)

#define PLUGIN_NAME    "NextGen Disk Cache"
// Original author first (Archost). Uploader on Nexus: enpinion.
#define PLUGIN_AUTHOR  "Archost (mod: enpinion upload; derivative)"
#define PLUGIN_VERSION ((1u << 16) | (4u << 8) | 1u) // 1.4.1
#define PLUGIN_VERSION_STRING "1.4.1"

// ---------------------------------------------------------------------------
// Settings (INI)
// ---------------------------------------------------------------------------
struct Settings {
	bool enableFileCacheHooks = true;
	bool enableCreateFileW = true;
	bool enableCreateFileA = true;
	bool stripNoBuffering = true;
	// Default-safe scope: only known Bethesda archive reads are rewritten.
	// Broad mode may include known loose assets, but unknown extensions are
	// never modified.
	bool conservativeHookScope = true;
	bool preferRandomAccessOnArchives = true;
	bool leaveSequentialOnLogs = true;

	// Process-scoped hints: SetProcessInformation(GetCurrentProcess(), ...) only
	// affects Skyrim itself, so unlike the CreateFile hook these cannot influence
	// other SKSE plugins. EcoQoS throttling actively hurts a foreground game, so
	// clearing it is on by default. Working-set expansion stays off: the process
	// working set is not the Windows system file cache.
	bool disablePowerThrottling = true;
	bool raiseMemoryPriority = true;
	bool expandWorkingSet = false;
	// Fraction of physical RAM allowed as max working set (0.10–0.75). 0 = skip.
	double workingSetMaxFraction = 0.50;
	// Min working set in MB (soft; 0 = leave Windows default min).
	unsigned workingSetMinMB = 128;

	bool hardwareProfile = false;
	bool autoTune = false;
	bool highEndMode = false;       // default target: modern X3D/DDR5/NVMe systems
	int  gameDriveClass = 0;       // 0 = auto-detect, 1 = HDD, 2 = SATA SSD, 3 = NVMe SSD
	bool preferFastCores = false;  // legacy whole-process hint, still opt-in
	bool placeWarmThreadsOnBackgroundCores = false;

	bool enableWarmCache = false;
	unsigned warmCacheDelaySecs = 60;
	unsigned warmCacheMaxFiles = 128;
	unsigned warmCacheBytesPerFileMB = 8;
	unsigned warmCacheBudgetMB = 512; // hard total budget; 0 = conservative auto
	unsigned warmCacheThreads = 1;    // explicit by default; auto never exceeds 2
	bool warmCacheMappedPrefetch = true;
	bool warmCacheLowIoPriority = true;
	bool warmCacheStridedPrefetch = true;
	unsigned warmCacheStrideMB = 256;
	unsigned warmCacheReserveMB = 0; // 0 = automatic safety reserve
	bool stopWarmCacheOnMemoryPressure = true;
	int warmCacheThreadPriority = THREAD_PRIORITY_LOWEST;

	bool directStorageProbe = false;
	bool directStorageWarmRead = false; // experimental: raw read, not OS cache population
	unsigned directStorageQueueCapacity = 128;
	unsigned directStorageBatchMB = 64;
	unsigned directStorageTimeoutMs = 30000;

	bool logToFile = true;
	bool logEveryOpen = false; // debug only - very spammy
	bool logStatsOnExit = false; // retained for config compatibility; no loader-lock I/O
	bool logStatsAfterWarm = true;
};

static Settings g_settings;
static std::mutex g_logMutex;
static std::ofstream g_log;
static std::atomic<bool> g_shutdown{false};
static std::atomic<uint64_t> g_opensTotal{0};
static std::atomic<uint64_t> g_opensPatched{0};
static std::atomic<uint64_t> g_noBufferingStripped{0};
// In-scope opens refused by the safety gate (write/delete intent, OVERLAPPED,
// WRITE_THROUGH, DELETE_ON_CLOSE, or a create/truncate disposition).
static std::atomic<uint64_t> g_opensSafetyGated{0};
static std::atomic<bool> g_warmStarted{false};
static HMODULE g_selfModule = nullptr;
static bool g_hookAttachFailed = false;

// Newer kernel32 APIs resolved at runtime so the DLL still loads on Win7/8.1.
using PrefetchVirtualMemory_t = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
using GetSystemCpuSetInformation_t = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using SetProcessDefaultCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using SetThreadSelectedCpuSets_t = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using SetThreadInformation_t = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
static PrefetchVirtualMemory_t g_pPrefetchVirtualMemory = nullptr;
static GetSystemCpuSetInformation_t g_pGetSystemCpuSetInformation = nullptr;
static SetProcessDefaultCpuSets_t g_pSetProcessDefaultCpuSets = nullptr;
static SetThreadSelectedCpuSets_t g_pSetThreadSelectedCpuSets = nullptr;
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
	g_pSetThreadSelectedCpuSets =
		reinterpret_cast<SetThreadSelectedCpuSets_t>(GetProcAddress(k32, "SetThreadSelectedCpuSets"));
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

static unsigned ParseUnsigned(const char* v, unsigned def)
{
	if (!v || !*v)
		return def;
	errno = 0;
	char* end = nullptr;
	const long long parsed = strtoll(v, &end, 10);
	while (end && (*end == ' ' || *end == '\t'))
		++end;
	if (errno == ERANGE || end == v || (end && *end) || parsed < 0 ||
		static_cast<unsigned long long>(parsed) > UINT_MAX)
		return def;
	return static_cast<unsigned>(parsed);
}

static int ParseInt(const char* v, int def)
{
	if (!v || !*v)
		return def;
	errno = 0;
	char* end = nullptr;
	const long parsed = strtol(v, &end, 10);
	while (end && (*end == ' ' || *end == '\t'))
		++end;
	if (errno == ERANGE || end == v || (end && *end) || parsed < INT_MIN || parsed > INT_MAX)
		return def;
	return static_cast<int>(parsed);
}

static double ParseDouble(const char* v, double def)
{
	if (!v || !*v)
		return def;
	errno = 0;
	char* end = nullptr;
	const double parsed = strtod(v, &end);
	while (end && (*end == ' ' || *end == '\t'))
		++end;
	if (errno == ERANGE || end == v || (end && *end) || !std::isfinite(parsed))
		return def;
	return parsed;
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
			g_settings.enableFileCacheHooks = ParseBool(val, g_settings.enableFileCacheHooks);
		else if (_stricmp(key, "bEnableCreateFileW") == 0)
			g_settings.enableCreateFileW = ParseBool(val, g_settings.enableCreateFileW);
		else if (_stricmp(key, "bEnableCreateFileA") == 0)
			g_settings.enableCreateFileA = ParseBool(val, g_settings.enableCreateFileA);
		else if (_stricmp(key, "bStripNoBuffering") == 0)
			g_settings.stripNoBuffering = ParseBool(val, g_settings.stripNoBuffering);
		else if (_stricmp(key, "bConservativeHookScope") == 0)
			g_settings.conservativeHookScope = ParseBool(val, g_settings.conservativeHookScope);
		else if (_stricmp(key, "bPreferRandomAccessOnArchives") == 0 ||
			_stricmp(key, "bPreferRandomAccessOnAssets") == 0)
			g_settings.preferRandomAccessOnArchives = ParseBool(val, g_settings.preferRandomAccessOnArchives);
		else if (_stricmp(key, "bLeaveSequentialOnLogs") == 0)
			g_settings.leaveSequentialOnLogs = ParseBool(val, g_settings.leaveSequentialOnLogs);
		else if (_stricmp(key, "bDisablePowerThrottling") == 0)
			g_settings.disablePowerThrottling = ParseBool(val, g_settings.disablePowerThrottling);
		else if (_stricmp(key, "bRaiseMemoryPriority") == 0)
			g_settings.raiseMemoryPriority = ParseBool(val, g_settings.raiseMemoryPriority);
		else if (_stricmp(key, "bExpandWorkingSet") == 0)
			g_settings.expandWorkingSet = ParseBool(val, g_settings.expandWorkingSet);
		else if (_stricmp(key, "fWorkingSetMaxFraction") == 0)
			g_settings.workingSetMaxFraction = ParseDouble(val, g_settings.workingSetMaxFraction);
		else if (_stricmp(key, "iWorkingSetMinMB") == 0)
			g_settings.workingSetMinMB = ParseUnsigned(val, g_settings.workingSetMinMB);
		else if (_stricmp(key, "bHardwareProfile") == 0)
			g_settings.hardwareProfile = ParseBool(val, g_settings.hardwareProfile);
		else if (_stricmp(key, "bAutoTune") == 0)
			g_settings.autoTune = ParseBool(val, g_settings.autoTune);
		else if (_stricmp(key, "iGameDriveClass") == 0)
			g_settings.gameDriveClass = ParseInt(val, g_settings.gameDriveClass);
		else if (_stricmp(key, "bPreferFastCores") == 0)
			g_settings.preferFastCores = ParseBool(val, g_settings.preferFastCores);
		else if (_stricmp(key, "bEnableWarmCache") == 0)
			g_settings.enableWarmCache = ParseBool(val, g_settings.enableWarmCache);
		else if (_stricmp(key, "iWarmCacheDelaySecs") == 0)
			g_settings.warmCacheDelaySecs = ParseUnsigned(val, g_settings.warmCacheDelaySecs);
		else if (_stricmp(key, "iWarmCacheMaxFiles") == 0)
			g_settings.warmCacheMaxFiles = ParseUnsigned(val, g_settings.warmCacheMaxFiles);
		else if (_stricmp(key, "iWarmCacheBytesPerFileMB") == 0)
			g_settings.warmCacheBytesPerFileMB = ParseUnsigned(val, g_settings.warmCacheBytesPerFileMB);
		else if (_stricmp(key, "iWarmCacheBudgetMB") == 0)
			g_settings.warmCacheBudgetMB = ParseUnsigned(val, g_settings.warmCacheBudgetMB);
		else if (_stricmp(key, "iWarmCacheThreads") == 0)
			g_settings.warmCacheThreads = ParseUnsigned(val, g_settings.warmCacheThreads);
		else if (_stricmp(key, "bWarmCacheMappedPrefetch") == 0)
			g_settings.warmCacheMappedPrefetch = ParseBool(val, g_settings.warmCacheMappedPrefetch);
		else if (_stricmp(key, "bWarmCacheLowIoPriority") == 0)
			g_settings.warmCacheLowIoPriority = ParseBool(val, g_settings.warmCacheLowIoPriority);
		else if (_stricmp(key, "bHighEndMode") == 0)
			g_settings.highEndMode = ParseBool(val, g_settings.highEndMode);
		else if (_stricmp(key, "bPlaceWarmThreadsOnBackgroundCores") == 0)
			g_settings.placeWarmThreadsOnBackgroundCores = ParseBool(val, g_settings.placeWarmThreadsOnBackgroundCores);
		else if (_stricmp(key, "bWarmCacheStridedPrefetch") == 0)
			g_settings.warmCacheStridedPrefetch = ParseBool(val, g_settings.warmCacheStridedPrefetch);
		else if (_stricmp(key, "iWarmCacheStrideMB") == 0)
			g_settings.warmCacheStrideMB = ParseUnsigned(val, g_settings.warmCacheStrideMB);
		else if (_stricmp(key, "iWarmCacheReserveMB") == 0)
			g_settings.warmCacheReserveMB = ParseUnsigned(val, g_settings.warmCacheReserveMB);
		else if (_stricmp(key, "bStopWarmCacheOnMemoryPressure") == 0)
			g_settings.stopWarmCacheOnMemoryPressure = ParseBool(val, g_settings.stopWarmCacheOnMemoryPressure);
		else if (_stricmp(key, "bDirectStorageProbe") == 0)
			g_settings.directStorageProbe = ParseBool(val, g_settings.directStorageProbe);
		else if (_stricmp(key, "bDirectStorageWarmRead") == 0)
			g_settings.directStorageWarmRead = ParseBool(val, g_settings.directStorageWarmRead);
		else if (_stricmp(key, "iDirectStorageQueueCapacity") == 0)
			g_settings.directStorageQueueCapacity = ParseUnsigned(val, g_settings.directStorageQueueCapacity);
		else if (_stricmp(key, "iDirectStorageBatchMB") == 0)
			g_settings.directStorageBatchMB = ParseUnsigned(val, g_settings.directStorageBatchMB);
		else if (_stricmp(key, "iDirectStorageTimeoutMs") == 0)
			g_settings.directStorageTimeoutMs = ParseUnsigned(val, g_settings.directStorageTimeoutMs);
		else if (_stricmp(key, "bLogToFile") == 0)
			g_settings.logToFile = ParseBool(val, g_settings.logToFile);
		else if (_stricmp(key, "bLogEveryOpen") == 0)
			g_settings.logEveryOpen = ParseBool(val, g_settings.logEveryOpen);
		else if (_stricmp(key, "bLogStatsOnExit") == 0)
			g_settings.logStatsOnExit = ParseBool(val, g_settings.logStatsOnExit);
		else if (_stricmp(key, "bLogStatsAfterWarm") == 0)
			g_settings.logStatsAfterWarm = ParseBool(val, g_settings.logStatsAfterWarm);
	}
	fclose(f);
}

static unsigned ClampUnsigned(unsigned value, unsigned lo, unsigned hi)
{
	return std::max(lo, std::min(value, hi));
}

static void SanitizeSettings()
{
	if (!std::isfinite(g_settings.workingSetMaxFraction))
		g_settings.workingSetMaxFraction = 0.50;
	g_settings.workingSetMaxFraction =
		std::max(0.10, std::min(g_settings.workingSetMaxFraction, 0.75));
	g_settings.workingSetMinMB = ClampUnsigned(g_settings.workingSetMinMB, 0, 16384);
	g_settings.gameDriveClass = std::max(0, std::min(g_settings.gameDriveClass, 3));

	// Prevent malformed or negative INI values (atoi -> unsigned wrap) from
	// turning an optional prefetch into an unbounded delay, allocation, or I/O burst.
	g_settings.warmCacheDelaySecs = ClampUnsigned(g_settings.warmCacheDelaySecs, 0, 3600);
	g_settings.warmCacheMaxFiles = ClampUnsigned(g_settings.warmCacheMaxFiles, 0, 4096);
	g_settings.warmCacheBytesPerFileMB = ClampUnsigned(g_settings.warmCacheBytesPerFileMB, 1, 256);
	g_settings.warmCacheBudgetMB = ClampUnsigned(g_settings.warmCacheBudgetMB, 0, 16384);
	g_settings.warmCacheThreads = ClampUnsigned(g_settings.warmCacheThreads, 0, 8);
	g_settings.warmCacheStrideMB = ClampUnsigned(g_settings.warmCacheStrideMB, 32, 4096);
	g_settings.warmCacheReserveMB = ClampUnsigned(g_settings.warmCacheReserveMB, 0, 131072);
	g_settings.directStorageQueueCapacity =
		ClampUnsigned(g_settings.directStorageQueueCapacity, 32, 8192);
	g_settings.directStorageBatchMB = ClampUnsigned(g_settings.directStorageBatchMB, 8, 256);
	g_settings.directStorageTimeoutMs = ClampUnsigned(g_settings.directStorageTimeoutMs, 1000, 120000);

	if (!g_settings.directStorageProbe)
		g_settings.directStorageWarmRead = false;
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
	SanitizeSettings();
}

// ---------------------------------------------------------------------------
// Path classification
// ---------------------------------------------------------------------------
enum class PathKind { Archive, Asset, LogOrTemp, Save, Other };

static PathKind ClassifyExt(const char* e)
{
	// Logs / temp / pure append: preserve caller-selected sequential behavior.
	if (strcmp(e, ".log") == 0 || strcmp(e, ".tmp") == 0 || strcmp(e, ".temp") == 0 ||
		strcmp(e, ".dmp") == 0 || strcmp(e, ".txt") == 0)
		return PathKind::LogOrTemp;

	// Save games, cosaves, backups, and common state databases are durability-
	// sensitive. Never rewrite these requests, even for read-only opens.
	if (strcmp(e, ".skse") == 0 || strcmp(e, ".cosave") == 0 ||
		strcmp(e, ".ess") == 0 || strcmp(e, ".bak") == 0 ||
		strcmp(e, ".journal") == 0 || strcmp(e, ".sqlite") == 0 ||
		strcmp(e, ".sqlite3") == 0 || strcmp(e, ".db-wal") == 0 ||
		strcmp(e, ".db-shm") == 0)
		return PathKind::Save;

	// The conservative default modifies only Bethesda archives. Their internal
	// block lookup is genuinely non-sequential and is the narrowest defensible
	// continuation of the original Disk Cache Enabler behavior.
	if (strcmp(e, ".bsa") == 0 || strcmp(e, ".ba2") == 0)
		return PathKind::Archive;

	// Known game assets. In broad/advanced mode NO_BUFFERING may be removed from
	// synchronous read-only opens, but RANDOM_ACCESS is still reserved for archives.
	static const char* kAssets[] = {
		".esm", ".esp", ".esl",
		".nif", ".dds", ".hkx", ".tri", ".fuz", ".lip", ".xwm", ".wav",
		".pex", ".psc", ".swf", ".gfx", ".seq",
		".ini", ".json", ".toml", ".yaml", ".yml", ".bin", ".dat", ".db",
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
		const wchar_t c = *p;
		if (c > 127)
			return PathKind::Other;
		e[n++] = (char)tolower((int)c);
	}
	e[n] = 0;
	return ClassifyExt(e);
}

// True when a handle must never be rewritten regardless of file kind: write or
// delete intent, async completion, write-through durability, delete-on-close,
// or any disposition that can create/truncate. Shared by the flag computation
// and the statistics counter so the log can never disagree with the policy.
static bool IsSafetyGated(DWORD flags, DWORD desiredAccess, DWORD creationDisposition)
{
	const DWORD kWriteIntent = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED |
		FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
	const DWORD kSensitiveFlags = FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH |
		FILE_FLAG_DELETE_ON_CLOSE;
	return creationDisposition != OPEN_EXISTING ||
		(desiredAccess & kWriteIntent) != 0 ||
		(flags & kSensitiveFlags) != 0;
}

static DWORD ComputePatchedFlags(
	DWORD flags,
	DWORD desiredAccess,
	DWORD creationDisposition,
	PathKind kind)
{
	// Unknown extensions, saves/state, logs/temp, and known loose assets in the
	// default conservative mode are outside this plugin's authority.
	const bool eligibleKind = kind == PathKind::Archive ||
		(!g_settings.conservativeHookScope && kind == PathKind::Asset);
	if (!eligibleKind)
		return flags;

	// Only existing files opened for synchronous reads are eligible. This keeps
	// create/truncate/delete semantics and every write-capable or async handle intact.
	if (IsSafetyGated(flags, desiredAccess, creationDisposition))
		return flags;

	DWORD out = flags;
	if (g_settings.stripNoBuffering)
		out &= ~FILE_FLAG_NO_BUFFERING;

	// RANDOM_ACCESS is a caching hint, not a generic "faster" switch. Apply it
	// only to container archives with internal non-sequential block access.
	if (g_settings.preferRandomAccessOnArchives && kind == PathKind::Archive) {
		out &= ~FILE_FLAG_SEQUENTIAL_SCAN;
		out |= FILE_FLAG_RANDOM_ACCESS;
	}
	return out;
}

static DWORD PatchFlags(
	DWORD flags,
	DWORD desiredAccess,
	DWORD creationDisposition,
	PathKind kind)
{
	const DWORD out = ComputePatchedFlags(flags, desiredAccess, creationDisposition, kind);
	// Count only handles this plugin would otherwise have been allowed to touch
	// but deliberately refused for safety. Opens skipped merely because of file
	// kind (unknown extension, save, log, loose asset in conservative mode) are
	// not "protected" - they were never in scope - and must not inflate this
	// number, or the log stops being usable evidence in a bug report.
	if (out == flags) {
		const bool inScope = kind == PathKind::Archive ||
			(!g_settings.conservativeHookScope && kind == PathKind::Asset);
		if (inScope && IsSafetyGated(flags, desiredAccess, creationDisposition))
			g_opensSafetyGated.fetch_add(1, std::memory_order_relaxed);
		return out;
	}
	if ((flags & FILE_FLAG_NO_BUFFERING) && !(out & FILE_FLAG_NO_BUFFERING))
		g_noBufferingStripped.fetch_add(1, std::memory_order_relaxed);
	return out;
}

// Safety invariants that must hold under EVERY configuration. Unlike the 1.4.0
// version this is pure (it never mutates the live g_settings the hooks read)
// and it exercises the user's ACTUAL loaded settings, so the optimizer cannot
// prove the result and fold the failure branch away. In 1.4.0 it could: the
// whole function reduced to `true` and the error path was absent from the
// shipped binary, making the advertised guard unable to fail.
static bool ValidateFilePolicy()
{
	// Volatile sources defeat constant propagation into ComputePatchedFlags.
	static volatile DWORD vBase =
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN;
	static volatile DWORD vRead = GENERIC_READ;
	static volatile DWORD vWrite = GENERIC_WRITE;
	const DWORD base = vBase;
	const DWORD read = vRead;
	const DWORD write = vWrite;

	bool ok = true;
	// 1. Durability-sensitive and out-of-scope kinds are never rewritten.
	ok = ok && ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::Save) == base;
	ok = ok && ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::LogOrTemp) == base;
	ok = ok && ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::Other) == base;
	// 2. Write/delete intent is never rewritten, even for archives.
	ok = ok && ComputePatchedFlags(base, write, OPEN_EXISTING, PathKind::Archive) == base;
	// 3. Async completion and durability flags survive untouched.
	ok = ok && ComputePatchedFlags(base | FILE_FLAG_OVERLAPPED, read,
		OPEN_EXISTING, PathKind::Archive) == (base | FILE_FLAG_OVERLAPPED);
	ok = ok && ComputePatchedFlags(base | FILE_FLAG_WRITE_THROUGH, read,
		OPEN_EXISTING, PathKind::Archive) == (base | FILE_FLAG_WRITE_THROUGH);
	ok = ok && ComputePatchedFlags(base | FILE_FLAG_DELETE_ON_CLOSE, read,
		OPEN_EXISTING, PathKind::Archive) == (base | FILE_FLAG_DELETE_ON_CLOSE);
	// 4. Anything that can create or truncate is never rewritten.
	ok = ok && ComputePatchedFlags(base, read, CREATE_ALWAYS, PathKind::Archive) == base;
	ok = ok && ComputePatchedFlags(base, read, TRUNCATE_EXISTING, PathKind::Archive) == base;
	// 5. RANDOM_ACCESS stays archive-only in every mode, including broad mode.
	ok = ok && !(ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::Asset) &
		FILE_FLAG_RANDOM_ACCESS);
	// 6. Conservative scope really does exclude loose assets when enabled.
	if (g_settings.conservativeHookScope)
		ok = ok && ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::Asset) == base;
	// 7. When hooks and stripping are both on, an eligible archive read is
	//    actually optimized - otherwise the plugin is silently doing nothing.
	if (g_settings.enableFileCacheHooks && g_settings.stripNoBuffering) {
		const DWORD archive = ComputePatchedFlags(base, read, OPEN_EXISTING, PathKind::Archive);
		ok = ok && !(archive & FILE_FLAG_NO_BUFFERING);
	}
	return ok;
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
		const DWORD patched = PatchFlags(flags, dwDesiredAccess, dwCreationDisposition, kind);
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
		const DWORD patched = PatchFlags(flags, dwDesiredAccess, dwCreationDisposition, kind);
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
		Log("GPU: Resizable BAR appears ACTIVE based on BAR size; diagnostics only for this CPU/file-cache plugin.");
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

static uint64_t WarmMappedPrefetch(HANDLE file, uint64_t fileBytes, uint64_t budgetBytes)
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
	const uint64_t chunk = 4ull << 20;
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
}

static uint64_t WarmOneFile(const std::wstring& path, uint64_t maxBytes, bool mapped)
{
	if (g_settings.directStorageWarmRead && DirectStorageAvailable()) {
		return DirectStorageReadDiscard(path.c_str(), maxBytes,
			g_settings.directStorageTimeoutMs, g_shutdown);
	}

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
	const bool haveFileSize = GetFileSizeEx(h, &fs) != FALSE;
	if (haveFileSize && static_cast<uint64_t>(fs.QuadPart) < maxBytes)
		maxBytes = static_cast<uint64_t>(fs.QuadPart);
	if (!maxBytes) {
		CloseHandle(h);
		return 0;
	}

	uint64_t total = 0;
	if (mapped && haveFileSize && g_pPrefetchVirtualMemory) {
		total = WarmMappedPrefetch(h, static_cast<uint64_t>(fs.QuadPart), maxBytes);
	}
	if (!total) {
		std::vector<char> buf(1 << 20); // 1 MB chunk
		DWORD rd = 0;
		while (total < maxBytes) {
			const DWORD readBytes = static_cast<DWORD>(std::min<uint64_t>(buf.size(), maxBytes - total));
			if (!ReadFile(h, buf.data(), readBytes, &rd, nullptr) || rd == 0)
				break;
			total += rd;
			if (g_shutdown.load(std::memory_order_relaxed))
				break;
		}
	}
	CloseHandle(h);
	return total;
}

static uint64_t ReserveWarmBudget(uint64_t wanted)
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

static void WarmWorker()
{
	SetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);
	ApplyBackgroundCorePreference(g_hw);
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
		if (!HasWarmCacheMemoryHeadroom()) {
			Log("WarmCache: stopped because the RAM safety reserve was reached");
			return;
		}
		const size_t idx = g_warmNext.fetch_add(1, std::memory_order_relaxed);
		if (idx >= plan->files.size())
			return;
		const auto& entry = plan->files[idx];
		uint64_t want = std::min(entry.second, plan->perFileBytes);
		want = ReserveWarmBudget(want);
		if (!want)
			return;
		const uint64_t got = WarmOneFile(entry.first, want, plan->mappedPrefetch);
		if (got < want)
			g_warmBudgetLeft.fetch_add(static_cast<int64_t>(want - got), std::memory_order_relaxed);
		if (got) {
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
	if (!maxFiles) {
		Log("WarmCache: iWarmCacheMaxFiles=0; nothing to do");
		return;
	}
	const wchar_t* patterns[] = { L"\\*.bsa", L"\\*.ba2" };
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

	// Stable alphabetical coverage avoids privileging a few giant archives that
	// may be irrelevant to the current save. The warmer is intentionally blind
	// to save-specific asset use, so the default remains disabled.
	std::sort(plan.files.begin(), plan.files.end(),
		[](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });

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

	// Auto-tune may only reduce pressure; it never silently expands a user's
	// configured budget into a multi-gigabyte startup read.
	uint64_t budgetMB = g_settings.warmCacheBudgetMB;
	if (!budgetMB)
		budgetMB = g_settings.highEndMode ? 1024 : 512;
	const uint64_t reserveMB = AutomaticMemoryReserveMB(totalMB);
	const uint64_t usableMB = availMB > reserveMB ? availMB - reserveMB : 0;
	budgetMB = std::min(budgetMB, usableMB);
	if (tune && drive == HardwareProfile::Drive::HDD)
		budgetMB = std::min<uint64_t>(budgetMB, 256);

	const uint64_t perFileMB = g_settings.warmCacheBytesPerFileMB;

	unsigned threads = g_settings.warmCacheThreads;
	if (!threads) {
		threads = tune && drive == HardwareProfile::Drive::NvmeSsd &&
			g_settings.highEndMode ? 2 : 1;
	}
	if (g_settings.directStorageWarmRead && DirectStorageAvailable())
		threads = 1; // DirectStorage batches internally through one queue.
	threads = std::max(1u, std::min({ threads, 2u, (unsigned)std::max<size_t>(plan.files.size(), 1) }));

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
	if (g_settings.logStatsAfterWarm) {
		Log("Stats snapshot: opens=%llu patched=%llu no_buffering_stripped=%llu "
			"in_scope_safety_gated=%llu warm_read=%llu MB",
			(unsigned long long)g_opensTotal.load(),
			(unsigned long long)g_opensPatched.load(),
			(unsigned long long)g_noBufferingStripped.load(),
			(unsigned long long)g_opensSafetyGated.load(),
			(unsigned long long)(g_warmBytes.load() >> 20));
	}
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
	Log("CreateFile hooks attached (A=%d W=%d scope=%s random_access=archives_only)",
		(int)g_settings.enableCreateFileA, (int)g_settings.enableCreateFileW,
		g_settings.conservativeHookScope ? "archives-only" : "known-assets");
	return true;
}

// Retained for completeness. SKSE never hot-unloads plugins and DllMain's
// DLL_PROCESS_DETACH no longer detaches (Windows reclaims the trampoline at
// process exit), so this is intentionally uncalled.
[[maybe_unused]] static void DetachHooks()
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
	LoadSettings();
	OpenLog();
	ResolveDynamicApis();
	Log("NextGen Disk Cache " PLUGIN_VERSION_STRING " - conservative SKSE load path");
	if (!ValidateFilePolicy()) {
		g_settings.enableFileCacheHooks = false;
		Log("ERROR: internal file-policy self-test failed; hooks disabled");
	}
	if (!AttachHooks()) {
		g_hookAttachFailed = true;
		Log("WARNING: CreateFile hooks did not attach - running detection/hints only");
	}

	DetectSiblingPlugins();

	if (g_settings.hardwareProfile) {
		DetectCpu(g_hw);
		DetectRam(g_hw);
		DetectGameDrive(g_hw);
		// Optional INI override for setups auto-detection misreads (RAID
		// arrays, Storage Spaces, some USB enclosures).
		if (g_settings.gameDriveClass >= 1 && g_settings.gameDriveClass <= 3) {
			const HardwareProfile::Drive prev = g_hw.drive;
			g_hw.drive = g_settings.gameDriveClass == 3 ? HardwareProfile::Drive::NvmeSsd
				: g_settings.gameDriveClass == 2 ? HardwareProfile::Drive::SataSsd
				: HardwareProfile::Drive::HDD;
			if (g_hw.drive != prev)
				Log("Game drive override: %s -> %s (iGameDriveClass=%d)",
					DriveName(prev), DriveName(g_hw.drive), g_settings.gameDriveClass);
		}
		DetectGpuBars(g_hw);
		DetectOs(g_hw);
		LogHardwareProfile(g_hw);
		ApplyFastCorePreference(g_hw);
	} else {
		Log("Hardware profile disabled (bHardwareProfile=0) - auto-tune inactive");
	}

	ApplyProcessHints();

	if (g_settings.directStorageProbe) {
		const auto ds = DirectStorageInitialize(
			g_settings.directStorageQueueCapacity, g_settings.directStorageBatchMB,
			g_settings.directStorageWarmRead);
		if (ds.memoryQueueCreated)
			Log("DirectStorage: runtime ready at %ls; low-priority system-memory queue created [EXPERIMENTAL RAW READ ENABLED]", ds.runtimePath);
		else if (ds.factoryCreated && !g_settings.directStorageWarmRead)
			Log("DirectStorage: runtime validated at %ls; queue not created because raw reads are disabled", ds.runtimePath);
		else if (ds.runtimeDllPresent)
			Log("DirectStorage: runtime found at %ls but requested initialization failed hr=0x%08X%s",
				ds.runtimePath, (unsigned)ds.result,
				ds.coreDllPresent ? "" : " (dstoragecore.dll is not loaded - it must sit beside dstorage.dll)");
		else
			Log("DirectStorage: runtime not found; PrefetchVirtualMemory remains active");
	}

	if (!g_settings.enableWarmCache)
		Log("WarmCache: disabled (safe default)");

	bool expectedWarm = false;
	if (g_settings.enableWarmCache &&
		g_warmStarted.compare_exchange_strong(expectedWarm, true)) {
		std::thread(WarmCacheCoordinator).detach();
		Log("WarmCache thread started (delay %u s)", g_settings.warmCacheDelaySecs);
	} else if (g_settings.logStatsAfterWarm) {
		// The warm cache normally emits the stats snapshot, but it ships disabled,
		// which left the hook counters unreachable in the default configuration -
		// exactly the evidence a bug report needs. Emit them once from a one-shot
		// low-priority worker instead. Exit-time logging is deliberately not used:
		// that would put file I/O under the loader lock.
		std::thread([] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
			for (unsigned i = 0; i < 600 && !g_shutdown.load(std::memory_order_relaxed); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (g_shutdown.load(std::memory_order_relaxed))
				return;
			Log("Stats snapshot: opens=%llu patched=%llu no_buffering_stripped=%llu "
				"in_scope_safety_gated=%llu warm_read=%llu MB",
				(unsigned long long)g_opensTotal.load(),
				(unsigned long long)g_opensPatched.load(),
				(unsigned long long)g_noBufferingStripped.load(),
				(unsigned long long)g_opensSafetyGated.load(),
				(unsigned long long)(g_warmBytes.load() >> 20));
		}).detach();
	}

	Log("NextGen Disk Cache " PLUGIN_VERSION_STRING
		" loaded (Archost/enpinion Disk Cache Enabler derivative) runtime=0x%08X skse=0x%08X",
		skse ? skse->runtimeVersion : 0, skse ? skse->skseVersion : 0);
	return true;
}

// ---------------------------------------------------------------------------
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
