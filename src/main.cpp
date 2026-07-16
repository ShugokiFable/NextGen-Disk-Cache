// NextGen Disk Cache — SKSE64 plugin
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
// Does not enable ReBAR / XMP / x3D — those are BIOS / silicon.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
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

// SKSE (bundled headers from Disk Cache Enabler deps — Query/Load + AE version data)
#include <skse/common/IPrefix.h>
#include <skse/skse64/PluginAPI.h>
#include <skse/skse64_common/skse_version.h>

// Detours
#include <detours/detours.h>

#define SKSEAPI extern "C" __declspec(dllexport)

#define PLUGIN_NAME    "NextGen Disk Cache"
// Original author first (Archost). Uploader on Nexus: enpinion.
#define PLUGIN_AUTHOR  "Archost (mod: enpinion upload; derivative)"
#define PLUGIN_VERSION ((1u << 16) | (0u << 8) | 1u) // 1.0.1

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

	bool enableWarmCache = true;
	unsigned warmCacheDelaySecs = 8;
	unsigned warmCacheMaxFiles = 256;
	unsigned warmCacheBytesPerFileMB = 8;
	unsigned warmCacheThreadPriority = THREAD_PRIORITY_LOWEST;

	bool logToFile = true;
	bool logEveryOpen = false; // debug only — very spammy
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
		else if (_stricmp(key, "bEnableWarmCache") == 0)
			g_settings.enableWarmCache = ParseBool(val, true);
		else if (_stricmp(key, "iWarmCacheDelaySecs") == 0)
			g_settings.warmCacheDelaySecs = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheMaxFiles") == 0)
			g_settings.warmCacheMaxFiles = (unsigned)atoi(val);
		else if (_stricmp(key, "iWarmCacheBytesPerFileMB") == 0)
			g_settings.warmCacheBytesPerFileMB = (unsigned)atoi(val);
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
static void ToLowerAscii(std::string& s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
}

static const char* FindExt(const char* path)
{
	if (!path)
		return nullptr;
	const char* dot = nullptr;
	for (const char* p = path; *p; ++p) {
		if (*p == '.' )
			dot = p;
		if (*p == '\\' || *p == '/')
			dot = nullptr; // extension only from final component
	}
	return dot;
}

enum class PathKind { Asset, LogOrTemp, Other };

static PathKind ClassifyPathA(const char* path)
{
	if (!path || !*path)
		return PathKind::Other;
	const char* ext = FindExt(path);
	if (!ext)
		return PathKind::Other;
	char e[16] = {};
	size_t n = 0;
	for (const char* p = ext; *p && n + 1 < sizeof(e); ++p)
		e[n++] = (char)tolower((unsigned char)*p);
	e[n] = 0;

	// Logs / temp / pure append — leave sequential scan alone if present
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

static PathKind ClassifyPathW(const wchar_t* path)
{
	if (!path || !*path)
		return PathKind::Other;
	char narrow[MAX_PATH * 2] = {};
	WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, (int)sizeof(narrow) - 1, nullptr, nullptr);
	return ClassifyPathA(narrow);
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
			Log("CreateFileA flags %08X→%08X %s", dwFlagsAndAttributes, flags, lpFilename);
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
			Log("CreateFileW flags %08X→%08X", dwFlagsAndAttributes, flags);
	}
	return CreateFileW_orig(
		lpFilename, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, flags, hTemplateFile);
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

// MEMORY_PRIORITY_INFORMATION / ProcessMemoryPriority exist on Win8+
#ifndef MemoryPriorityVeryLow
// Values from winnt.h / processsnapshot — use numeric to avoid SDK version fights
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
			// Soft limits — do not hard-lock pages (that would fight the OS).
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

static void WarmReadFile(const std::wstring& path, size_t maxBytes)
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
		return;

	std::vector<char> buf(1 << 20); // 1 MB chunk
	size_t total = 0;
	DWORD rd = 0;
	while (total < maxBytes &&
		ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0) {
		total += rd;
		if (g_shutdown.load(std::memory_order_relaxed))
			break;
	}
	CloseHandle(h);
}

static void WarmCacheWorker()
{
	const unsigned delay = g_settings.warmCacheDelaySecs;
	for (unsigned i = 0; i < delay * 10; ++i) {
		if (g_shutdown.load())
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	SetThreadPriority(GetCurrentThread(), (int)g_settings.warmCacheThreadPriority);

	const std::wstring data = GetGameDataPath();
	if (data.empty()) {
		Log("WarmCache: could not resolve Data path");
		return;
	}
	Log("WarmCache: scanning %ls", data.c_str());

	const size_t perFile = (size_t)g_settings.warmCacheBytesPerFileMB * 1024ull * 1024ull;
	const unsigned maxFiles = g_settings.warmCacheMaxFiles;

	// Collect interesting archives first (BSA/BA2 + masters), then others if room.
	std::vector<std::wstring> files;
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
			files.emplace_back(data + L"\\" + fd.cFileName);
			if (files.size() >= maxFiles)
				break;
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		if (files.size() >= maxFiles)
			break;
	}

	// Prefer larger archives first (more benefit per open).
	std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
		WIN32_FILE_ATTRIBUTE_DATA aa{}, bb{};
		ULARGE_INTEGER sa{}, sb{};
		sa.QuadPart = sb.QuadPart = 0;
		if (GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &aa)) {
			sa.HighPart = aa.nFileSizeHigh;
			sa.LowPart = aa.nFileSizeLow;
		}
		if (GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &bb)) {
			sb.HighPart = bb.nFileSizeHigh;
			sb.LowPart = bb.nFileSizeLow;
		}
		return sa.QuadPart > sb.QuadPart;
	});

	unsigned warmed = 0;
	for (const auto& f : files) {
		if (g_shutdown.load())
			break;
		WarmReadFile(f, perFile);
		++warmed;
	}
	Log("WarmCache: prefetched head of %u archives (up to %u MB each)",
		warmed, g_settings.warmCacheBytesPerFileMB);
}

static void DetectSiblingPlugins()
{
	// Informational only — do not refuse to load.
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
					"Remove it — NextGen Disk Cache supersedes it (double CreateFile hooks are wasteful).");
			} else if (_stricmp(peers[i], "BSAMemoryMap.dll") == 0) {
				Log("BSAMemoryMap.dll detected — complementary (mmap/decomp cache). "
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
	// Warm cache starts here so SKSE log folder exists.
	DetectSiblingPlugins();
	ApplyProcessHints();

	if (g_settings.enableWarmCache && !g_warmThread.joinable()) {
		g_warmThread = std::thread(WarmCacheWorker);
		Log("WarmCache thread started (delay %u s)", g_settings.warmCacheDelaySecs);
	}

	Log("NextGen Disk Cache 1.0.1 loaded (Archost/enpinion Disk Cache Enabler derivative) runtime=0x%08X skse=0x%08X",
		skse ? skse->runtimeVersion : 0, skse ? skse->skseVersion : 0);
	return true;
}

// ---------------------------------------------------------------------------
// DllMain — attach CreateFile hooks as early as possible
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
		Log("NextGen Disk Cache 1.0.1 — DllMain attach (based on Archost Disk Cache Enabler; uploaded by enpinion)");
		if (!AttachHooks()) {
			MessageBoxA(nullptr,
				"Failed to attach CreateFile hooks.\nGame will run without NextGen Disk Cache.",
				PLUGIN_NAME, MB_OK | MB_ICONWARNING);
		}
		break;

	case DLL_PROCESS_DETACH:
		g_shutdown.store(true);
		if (g_warmThread.joinable()) {
			// Don't block unload forever
			g_warmThread.detach();
		}
		if (g_settings.logStatsOnExit) {
			Log("Stats: opens=%llu patched=%llu no_buffering_stripped=%llu",
				(unsigned long long)g_opensTotal.load(),
				(unsigned long long)g_opensPatched.load(),
				(unsigned long long)g_noBufferingStripped.load());
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
