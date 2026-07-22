#include "IatHook.h"

#include <cstring>

namespace {

// The PE headers are parsed by hand rather than through ImageDirectoryEntryToData
// so the plugin does not acquire a DbgHelp import. Keeping the import list to
// KERNEL32 (plus what the rest of the plugin needs) makes the binary trivial for
// a third party to audit, which matters for a mod distributed as a prebuilt DLL.
bool ImportDirectory(HMODULE module, BYTE*& base, PIMAGE_IMPORT_DESCRIPTOR& desc)
{
	base = reinterpret_cast<BYTE*>(module);
	if (!base)
		return false;

	const auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;

	const auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;

	const IMAGE_DATA_DIRECTORY& dir =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress || !dir.Size)
		return false;

	desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + dir.VirtualAddress);
	return true;
}

// Locates the IAT slot holding `funcName`. Matching is by function name across
// every import descriptor rather than by a hardcoded "KERNEL32.dll", because
// Windows may route the import through an API set such as
// api-ms-win-core-file-l1-1-0.dll. CreateFileA/CreateFileW are unambiguous
// enough that a name match cannot reasonably collide.
PIMAGE_THUNK_DATA FindIatSlot(HMODULE module, const char* funcName,
	char* foundInModule, size_t foundInModuleChars)
{
	BYTE* base = nullptr;
	PIMAGE_IMPORT_DESCRIPTOR desc = nullptr;
	if (!ImportDirectory(module, base, desc))
		return nullptr;

	for (; desc->Name; ++desc) {
		// A bound import has no OriginalFirstThunk; its FirstThunk already holds
		// addresses, so the name array is unavailable and the entry is skipped.
		if (!desc->OriginalFirstThunk || !desc->FirstThunk)
			continue;

		auto* nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->OriginalFirstThunk);
		auto* addrThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);

		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk) {
			if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				continue; // imported by ordinal: no name to compare
			const auto* byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
				base + nameThunk->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(byName->Name), funcName) != 0)
				continue;
			if (foundInModule && foundInModuleChars) {
				strncpy_s(foundInModule, foundInModuleChars,
					reinterpret_cast<const char*>(base + desc->Name), _TRUNCATE);
			}
			return addrThunk;
		}
	}
	return nullptr;
}

bool WriteSlot(PIMAGE_THUNK_DATA slot, ULONGLONG value)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function),
			PAGE_READWRITE, &oldProtect))
		return false;
	slot->u1.Function = value;
	DWORD ignored = 0;
	VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), oldProtect, &ignored);
	return true;
}

} // namespace

void* IatHookInstall(HMODULE module, const char* funcName, void* replacement,
	char* foundInModule, size_t foundInModuleChars)
{
	if (!funcName || !replacement)
		return nullptr;
	if (!module)
		module = GetModuleHandleW(nullptr); // main executable

	PIMAGE_THUNK_DATA slot = FindIatSlot(module, funcName, foundInModule, foundInModuleChars);
	if (!slot)
		return nullptr;

	void* previous = reinterpret_cast<void*>(slot->u1.Function);
	if (previous == replacement)
		return nullptr; // already installed; do not chain onto ourselves
	if (!WriteSlot(slot, reinterpret_cast<ULONGLONG>(replacement)))
		return nullptr;
	return previous;
}

bool IatHookRemove(HMODULE module, const char* funcName, void* original)
{
	if (!funcName || !original)
		return false;
	if (!module)
		module = GetModuleHandleW(nullptr);

	PIMAGE_THUNK_DATA slot = FindIatSlot(module, funcName, nullptr, 0);
	if (!slot)
		return false;
	return WriteSlot(slot, reinterpret_cast<ULONGLONG>(original));
}
