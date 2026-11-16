#include <stdafx.h>
#include "heap.h"
#include "memory.h"
#include "function.h"
#include "x86_context.h"

Memory::Memory()
{
#ifdef _WIN32

	static constexpr uintptr_t kCandidateBases[] = 
	{
		0x100000000ull,  
		0x200000000ull,
		0x300000000ull,
		0x400000000ull,
		0x500000000ull,
	};

	for (uintptr_t candidate : kCandidateBases)
	{
		base = (uint8_t*)VirtualAlloc(
			(void*)candidate,
			X86_MEMORY_SIZE,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE
		);

		if (base)
			break;
	}


	if (base == nullptr) 
	{
		DWORD error = GetLastError();
		fprintf(stderr, "Failed to allocate at 0x10000000 (Error: 0x%lX)\n", error);
		return;
	}

	DWORD oldProtect;
	VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#elif defined(__ANDROID__)
	// Host pointers are turned back into guest addresses by truncating them to 32 bits in
	// a few places, which is only correct when the base is a multiple of 4 GiB. mmap()
	// treats the address as a hint, and an app process (the Java runtime is loaded first)
	// has far more mappings that can be in the way than a desktop one, so take the first
	// candidate that is granted exactly instead of accepting whatever address comes back.
	// MAP_NORESERVE: the 4 GiB are address space, not memory the game will ever fill.
	static constexpr uintptr_t kCandidateBases[] =
	{
		0x100000000ull,
		0x200000000ull,
		0x300000000ull,
		0x400000000ull,
		0x500000000ull,
		0x600000000ull,
		0x700000000ull,
		0x800000000ull,
	};

	for (uintptr_t candidate : kCandidateBases)
	{
		void* mapping = mmap((void*)candidate, X86_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);

		if (mapping == (void*)candidate)
		{
			base = (uint8_t*)mapping;
			break;
		}

		if (mapping != MAP_FAILED)
			munmap(mapping, X86_MEMORY_SIZE);
	}

	// HostStartup() reports the failure once a message box can be shown.
	if (base == nullptr)
		return;

	mprotect(base, 4096, PROT_NONE);
#else
	base = (uint8_t*)mmap((void*)0x100000000ull, X86_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	if (base == (uint8_t*)MAP_FAILED)
		base = (uint8_t*)mmap(NULL, X86_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	mprotect(base, 4096, PROT_NONE);
#endif


	for (size_t i = 0; X86FuncMappings[i].guest != 0; i++)
	{
		if (X86FuncMappings[i].host != nullptr)
			InsertFunction(X86FuncMappings[i].guest, X86FuncMappings[i].host);
	}
}

void* MmGetHostAddress(uint32_t ptr)
{
	return g_memory.Translate(ptr);
}


