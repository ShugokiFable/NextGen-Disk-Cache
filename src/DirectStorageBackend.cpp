#include "DirectStorageBackend.h"

#include <dstorage.h>
#include <algorithm>
#include <cwchar>
#include <mutex>
#include <vector>

namespace {
using DStorageGetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

std::mutex g_mutex;
HMODULE g_runtime = nullptr;
IDStorageFactory* g_factory = nullptr;
IDStorageQueue* g_queue = nullptr;
IDStorageStatusArray* g_status = nullptr;
DirectStorageInfo g_info{};
unsigned g_batchBytes = 64u << 20;

template <class T>
void ReleaseCom(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseRuntimeUnlocked()
{
    ReleaseCom(g_status);
    if (g_queue)
        g_queue->Close();
    ReleaseCom(g_queue);
    ReleaseCom(g_factory);
    if (g_runtime) {
        FreeLibrary(g_runtime);
        g_runtime = nullptr;
    }
}

void ResetUnlocked()
{
    ReleaseRuntimeUnlocked();
    g_info = {};
}
} // namespace

DirectStorageInfo DirectStorageInitialize(
    unsigned queueCapacity,
    unsigned batchMB,
    bool createMemoryQueue)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ResetUnlocked();

    // Prefer a redistributable installed beside this SKSE plugin. Explicitly
    // using the plugin directory also lets dstorage.dll find dstoragecore.dll
    // through LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR. Fall back to registered/default
    // loader locations when another component already provides the runtime.
    wchar_t localRuntime[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_info), &self) &&
        GetModuleFileNameW(self, localRuntime, MAX_PATH)) {
        wchar_t* slash = wcsrchr(localRuntime, L'\\');
        if (slash) {
            slash[1] = 0;
            wcscat_s(localRuntime, MAX_PATH, L"dstorage.dll");
            g_runtime = LoadLibraryExW(localRuntime, nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
    }
    if (!g_runtime)
        g_runtime = LoadLibraryExW(L"dstorage.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_runtime) {
        g_info.result = HRESULT_FROM_WIN32(GetLastError());
        return g_info;
    }
    g_info.runtimeDllPresent = true;
    GetModuleFileNameW(g_runtime, g_info.runtimePath, MAX_PATH);

    auto getFactory = reinterpret_cast<DStorageGetFactoryFn>(
        GetProcAddress(g_runtime, "DStorageGetFactory"));
    if (!getFactory) {
        g_info.result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        ReleaseRuntimeUnlocked();
        return g_info;
    }

    HRESULT hr = getFactory(__uuidof(IDStorageFactory), reinterpret_cast<void**>(&g_factory));
    if (FAILED(hr) || !g_factory) {
        g_info.result = hr;
        ReleaseRuntimeUnlocked();
        return g_info;
    }
    g_info.factoryCreated = true;
    g_info.coreDllPresent = GetModuleHandleW(L"dstoragecore.dll") != nullptr;
    if (!createMemoryQueue) {
        g_info.result = S_OK;
        return g_info;
    }

    DSTORAGE_QUEUE_DESC desc{};
    desc.Capacity = static_cast<UINT16>(std::max<unsigned>(DSTORAGE_MIN_QUEUE_CAPACITY,
        std::min<unsigned>(queueCapacity, DSTORAGE_MAX_QUEUE_CAPACITY)));
    desc.Priority = DSTORAGE_PRIORITY_LOW;
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    desc.Device = nullptr; // system-memory destination queue
    desc.Name = "NextGenDiskCache raw-read queue";

    hr = g_factory->CreateQueue(&desc, __uuidof(IDStorageQueue),
        reinterpret_cast<void**>(&g_queue));
    if (FAILED(hr) || !g_queue) {
        g_info.result = hr;
        ReleaseRuntimeUnlocked();
        return g_info;
    }

    hr = g_factory->CreateStatusArray(1, "NextGenDiskCache raw-read status",
        __uuidof(IDStorageStatusArray), reinterpret_cast<void**>(&g_status));
    if (FAILED(hr) || !g_status) {
        g_info.result = hr;
        ReleaseRuntimeUnlocked();
        return g_info;
    }

    g_batchBytes = std::max(8u, std::min(batchMB, 256u)) << 20;
    g_info.memoryQueueCreated = true;
    g_info.result = S_OK;
    return g_info;
}

void DirectStorageShutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ResetUnlocked();
}

bool DirectStorageAvailable()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_factory && g_queue && g_status && g_info.memoryQueueCreated;
}

DirectStorageInfo DirectStorageGetInfo()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_info;
}

uint64_t DirectStorageReadDiscard(
    const wchar_t* path,
    uint64_t maxBytes,
    unsigned timeoutMs,
    const std::atomic<bool>& shutdownRequested)
{
    if (!path || !*path || !maxBytes)
        return 0;

    // A single queue is deliberately serialized. The queue itself batches
    // multiple requests, avoiding N competing DirectStorage schedulers.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_factory || !g_queue || !g_status)
        return 0;

    IDStorageFile* file = nullptr;
    HRESULT hr = g_factory->OpenFile(path, __uuidof(IDStorageFile),
        reinterpret_cast<void**>(&file));
    if (FAILED(hr) || !file)
        return 0;

    BY_HANDLE_FILE_INFORMATION fi{};
    hr = file->GetFileInformation(&fi);
    if (FAILED(hr)) {
        file->Release();
        return 0;
    }
    ULARGE_INTEGER fileSize{};
    fileSize.HighPart = fi.nFileSizeHigh;
    fileSize.LowPart = fi.nFileSizeLow;
    maxBytes = std::min(maxBytes, static_cast<uint64_t>(fileSize.QuadPart));
    if (!maxBytes) {
        file->Release();
        return 0;
    }

    constexpr unsigned kRequestBytes = 8u << 20;
    const unsigned slots = std::max(1u, g_batchBytes / kRequestBytes);
    const size_t scratchBytes = static_cast<size_t>(slots) * kRequestBytes;
    void* scratch = VirtualAlloc(nullptr, scratchBytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!scratch) {
        file->Release();
        return 0;
    }

    uint64_t completed = 0;
    while (completed < maxBytes && !shutdownRequested.load(std::memory_order_relaxed)) {
        const uint64_t batchStart = completed;
        unsigned queued = 0;
        for (; queued < slots && completed < maxBytes; ++queued) {
            const uint32_t bytes = static_cast<uint32_t>(
                std::min<uint64_t>(kRequestBytes, maxBytes - completed));
            DSTORAGE_REQUEST request{};
            request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            request.Source.File.Source = file;
            request.Source.File.Offset = completed;
            request.Source.File.Size = bytes;
            request.Destination.Memory.Buffer =
                static_cast<unsigned char*>(scratch) + static_cast<size_t>(queued) * kRequestBytes;
            request.Destination.Memory.Size = bytes;
            request.UncompressedSize = bytes;
            request.CancellationTag = 0x4E474443ull; // 'NGDC'
            request.Name = "NGDC raw warm read";
            g_queue->EnqueueRequest(&request);
            completed += bytes;
        }

        g_queue->EnqueueStatus(g_status, 0);
        g_queue->Submit();

        const ULONGLONG deadline = GetTickCount64() + std::max(1000u, timeoutMs);
        bool cancellationRequested = false;
        while (!g_status->IsComplete(0)) {
            if (!cancellationRequested &&
                (shutdownRequested.load(std::memory_order_relaxed) || GetTickCount64() >= deadline)) {
                // Cancellation is asynchronous. Keep waiting for the status entry
                // before releasing the file or scratch buffer, otherwise the queue
                // could write into freed memory.
                g_queue->CancelRequestsWithTag(~0ull, 0x4E474443ull);
                cancellationRequested = true;
            }
            Sleep(1);
        }
        hr = g_status->GetHResult(0);
        if (cancellationRequested || FAILED(hr)) {
            completed = batchStart;
            break;
        }
    }

    VirtualFree(scratch, 0, MEM_RELEASE);
    file->Release();
    return completed;
}
