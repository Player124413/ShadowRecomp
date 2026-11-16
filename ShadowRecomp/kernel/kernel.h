
#pragma once
#include "heap.h"
#include "memory.h"
#include <xbox.h>
#include <unordered_map>

#define OBJECT_SIGNATURE   (uint32_t)'XBOX'
#define GUEST_INVALID_HANDLE_VALUE 0xFFFFFFFF

//from CXBX's code:
#define TIB_ArbitraryDataSlot  0x14 // = 20 = offsetof(NT_TIB, ArbitraryUserPointer)
#define TIB_LinearSelfAddress  0x18 // = 24 = offsetof(NT_TIB, Self)
#define XBOX_KERNEL_BASE                    (PHYSICAL_MAP_BASE + XBE_IMAGE_BASE)
#define KERNEL_PHYSICAL_ADDRESS             XBE_IMAGE_BASE // = 0x10000
#define KERNEL_SIZE                         sizeof(DUMMY_KERNEL)
#define KERNEL_STACK_SIZE                   12288 // 0x03000, needed by PsCreateSystemThreadEx, however the current implementation doesn't use it

// Miscellaneous memory variables
// Xbox pages are (1 << 12) = 0x00001000 = 4096 bytes in size. Large pages are 4 MiB instead
// NOTE: PAGE_SIZE is also defined in xfile.h (oxdk) and linux_wrapper.h (oxdk)
// The host's C library may define these for its own pages (bionic's <sys/user.h> does).
#undef PAGE_SHIFT
#undef PAGE_SIZE
#undef PAGE_MASK
#define PAGE_SHIFT                          12 // 2^12 = 4 KiB
#define PAGE_SIZE                           (1 << PAGE_SHIFT) 
#define PAGE_MASK                           (PAGE_SIZE - 1)

struct KernelObject
{
    virtual ~KernelObject()
    {
    }

    virtual uint32_t Wait(uint32_t timeout)
    {
        assert(false && "Wait not implemented for this kernel object.");
        return STATUS_TIMEOUT;
    }
};

template<typename T, typename... Args>
inline T* CreateKernelObject(Args&&... args)
{ 
    static_assert(std::is_base_of_v<KernelObject, T>);
    return g_userHeap.AllocPhysical<T>(std::forward<Args>(args)...);
}

template<typename T = KernelObject>
inline T* GetKernelObject(uint32_t handle)
{
	//printf("GetKernelObject %s\n", typeid(T).name());
    assert(handle != GUEST_INVALID_HANDLE_VALUE);
    return reinterpret_cast<T*>(g_memory.Translate(handle));
}

uint32_t GetKernelHandle(KernelObject* obj);

void DestroyKernelObject(KernelObject* obj);
void DestroyKernelObject(uint32_t handle);

bool IsKernelObject(uint32_t handle);
bool IsKernelObject(void* obj);

bool IsInvalidKernelObject(void* obj);

template<typename T = void>
inline T* GetInvalidKernelObject()
{
    return reinterpret_cast<T*>(g_memory.Translate(GUEST_INVALID_HANDLE_VALUE));
}

extern Mutex g_kernelLock;

template<typename T>
inline T* QueryKernelObject(XDISPATCHER_HEADER &header)
{
    std::lock_guard guard{ g_kernelLock };
    if (header.WaitListHead.Flink != OBJECT_SIGNATURE)
    {
        header.WaitListHead.Flink = OBJECT_SIGNATURE;
        auto* obj = CreateKernelObject<T>(reinterpret_cast<typename T::guest_type*>(&header));
        header.WaitListHead.Blink = g_memory.MapVirtual(obj);

        return obj;
    }

    return static_cast<T*>(g_memory.Translate(header.WaitListHead.Blink));
}


void InitDpcData();
void InitMultiCoreMutexesPatches();
void RtlEnterCriticalSection(X86Context& ctx, XRTL_CRITICAL_SECTION* CritSec);
void RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* CritSec);
void RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* CritSec);