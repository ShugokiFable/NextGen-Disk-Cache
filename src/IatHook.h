#pragma once

#include <Windows.h>

// Narrow import-table hooking.
//
// Rewriting an entry in a module's Import Address Table only redirects calls
// that module makes through its own IAT. Every other loaded module keeps its
// own import table, so an unrelated SKSE plugin's file I/O never enters our
// code at all. This is deliberately weaker than a trampoline (Detours), which
// patches the target function itself and therefore intercepts every caller in
// the process.
//
// Trade-off, stated honestly: calls the target resolves dynamically through
// GetProcAddress, or issues from a different module, are NOT intercepted. The
// narrow scope is the point - fewer opens are optimized, and unrelated plugins
// are provably unaffected.

// Replaces the entry for `dllName!funcName` in `module`'s import table.
// Returns the previous function pointer (call through it), or nullptr when the
// import is absent, imported by ordinal, or the page could not be made writable.
// Pass nullptr for `module` to patch the main executable.
void* IatHookInstall(HMODULE module, const char* funcName, void* replacement,
	char* foundInModule, size_t foundInModuleChars);

// Restores a previously replaced entry. Safe to call with nullptr original.
bool IatHookRemove(HMODULE module, const char* funcName, void* original);
