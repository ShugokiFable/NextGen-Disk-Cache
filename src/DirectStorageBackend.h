#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

struct DirectStorageInfo {
    bool runtimeDllPresent = false;
    bool coreDllPresent = false;
    bool factoryCreated = false;
    bool memoryQueueCreated = false;
    HRESULT result = E_NOTIMPL;
    wchar_t runtimePath[MAX_PATH] = {};
};

// DirectStorage is optional. The plugin resolves it dynamically so Skyrim can
// still load when the redistributable is absent.
DirectStorageInfo DirectStorageInitialize(
    unsigned queueCapacity,
    unsigned batchMB,
    bool createMemoryQueue);
void DirectStorageShutdown();
bool DirectStorageAvailable();
DirectStorageInfo DirectStorageGetInfo();

// Experimental raw read path. It performs real DirectStorage file-to-memory
// reads, then discards the scratch buffer. This can exercise the NVMe path and
// controller cache, but it intentionally does NOT claim to populate Windows'
// normal file cache. The normal PrefetchVirtualMemory backend remains the
// default cache warmer.
uint64_t DirectStorageReadDiscard(
    const wchar_t* path,
    uint64_t maxBytes,
    unsigned timeoutMs,
    const std::atomic<bool>& shutdownRequested);
