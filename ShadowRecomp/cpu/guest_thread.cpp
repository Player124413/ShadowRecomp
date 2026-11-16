#include <stdafx.h>
#include <kernel/kernel.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include "x86_context.h"
#include <kernel/types.h>
#include <os/logger.h>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

struct KTHREAD_R
{
	/* 0x0/0 */ uint8_t header[16];
	/* 0x10/16 */ int8_t MutantListHead[8];
	/* 0x18/24 */ uint32_t KernelTime;
	/* 0x1C/28 */ uint32_t StackBase;
	/* 0x20/32 */ uint32_t StackLimit;
	/* 0x24/36 */ uint32_t KernelStack;
	/* 0x28/40 */ uint32_t TlsData;
	uint8_t padding[0xE4];
};

typedef struct _ETHREAD_R
{
	struct KTHREAD_R    Tcb;
	uint8_t padding[0x30];
}
ETHREAD_R;
static_assert(sizeof(ETHREAD_R) == 0x140);

const uint32_t tls_indexAddr = 0x5599A8;
constexpr size_t KPCR_SIZE = 0x280;
constexpr size_t ETHREAD_R_SIZE = sizeof(_ETHREAD_R);

constexpr size_t TLS_SIZE = 0x2000;//0x1000;
constexpr size_t STACK_SIZE = 0x10000;
constexpr size_t FX_SAVE_AREA_SIZE = sizeof(FX_SAVE_AREA);
constexpr size_t KSTART_SWITCH_FRAME_SIZE = 24;

bool isHostThread = false;

std::string GetThreadName(uint32_t func)
{
	std::string name = "";

	switch (func)
	{
	case 0x4F440:
		name = "AsyncLoadingThread";
		break;
	case 0x2AFADF:
		name = "mainXapi GameLoop";
		break;
	case 0x3291E0:
		name = "adxm_vsyncThread";
		break;
	case 0x329260:
		name = "adxm_fsThread";
		break;
	case 0x328E80:
		name = "adxm_mwidlThread";
		break;

	}

	return name;
}


static uint32_t GenerateStackSize(uint32_t &espBaseAddress, uint32_t TlsDataSize) 
{
	dword_xt StackSize = espBaseAddress & 15; // Fix 16 byte alignment
	espBaseAddress -= StackSize;
	StackSize += sizeof(FX_SAVE_AREA);
	TlsDataSize = ALIGN_UP(TlsDataSize, ulong_xt);
	StackSize += TlsDataSize; 	
	StackSize += 24; //KSWITCHFRAME + KSTART_FRAME
	return StackSize;
}

GuestThreadContext::GuestThreadContext(GuestThreadParams params)
{
	auto KernelStackSize = params.kernelStackSize;
	auto tlsSize = params.tlsSize;

	if (tlsSize <= 0)
		tlsSize = TLS_SIZE;

	if (KernelStackSize <= 0)
		KernelStackSize = STACK_SIZE;

	const size_t totalSize = KPCR_SIZE + tlsSize + KernelStackSize + FX_SAVE_AREA_SIZE + KSTART_SWITCH_FRAME_SIZE + ETHREAD_R_SIZE;

	assert(thread == nullptr);
	thread = (uint8_t*)g_userHeap.Alloc(totalSize);


	ETHREAD_R* eThread = reinterpret_cast<ETHREAD_R*>(thread + KPCR_SIZE + tlsSize + KernelStackSize);

	uint8_t* kpcr = thread;
	uint8_t* tls_region = kpcr + KPCR_SIZE;
	uint8_t* stack_limit = tls_region + tlsSize;
	uint8_t* stack_top = stack_limit + KernelStackSize;
	uint8_t* fx_save = stack_top;
	uint8_t* frames = fx_save + FX_SAVE_AREA_SIZE;
	uint8_t* eThreadMem = frames + KSTART_SWITCH_FRAME_SIZE;

	
	unsigned EspAddr = g_memory.MapVirtual(stack_top);
	auto sizeReserved = GenerateStackSize(EspAddr, params.tlsSize); //this is needed to ensure ESP start at the right place
	EspAddr -= sizeReserved;

	assert(stack_limit < stack_top);
	assert(tls_region >= kpcr + KPCR_SIZE);
	assert(stack_limit >= tls_region + tlsSize);
	assert(stack_top - stack_limit == KernelStackSize);


	memset(thread, 0, totalSize);
	memset(eThread, 0, sizeof(ETHREAD_R));

	// Set up memory regions
	uint8_t* NewPcr = thread;   //FS:[0x00] points here
	uint8_t* XbTib = (NewPcr + 0x0);
	uint8_t* Prcb = NewPcr + 0x28; //FS:[0x28] = prcbData

	eThread->Tcb.StackBase = g_memory.MapVirtual(stack_top); // stack;
	eThread->Tcb.StackLimit = g_memory.MapVirtual(stack_limit);
	eThread->Tcb.KernelStack = eThread->Tcb.StackBase; 
	eThread->Tcb.TlsData = g_memory.MapVirtual(tls_region);

	*(uint32_t*)(NewPcr + 0x30) = NPX_STATE_NOT_LOADED;

	xbox::addr_xt StackBaseAddr = reinterpret_cast<xbox::addr_xt>(eThread->Tcb.StackBase);

	uint32_t AdjustedStackBase = StackBaseAddr - sizeof(FX_SAVE_AREA);
	*(uint32_t*)(NewPcr + 0x04) = AdjustedStackBase;
	*(uint32_t*)(NewPcr + 0x08) = eThread->Tcb.StackLimit;

	*(uint32_t*)(NewPcr + 0x01C) = g_memory.MapVirtual(NewPcr); //SelfPcr
	*(uint32_t*)(NewPcr + 0x018) = g_memory.MapVirtual(XbTib); //Self
	*(uint32_t*)(NewPcr + 0x020) = g_memory.MapVirtual(Prcb);  //fs:[0x20] = Prcb

	*(uint32_t*)(Prcb + 0x30) = FALSE;
	*(uint32_t*)(NewPcr + 0x24) = PASSIVE_LEVEL;
	*(uint32_t*)(NewPcr + 0x28) = g_memory.MapVirtual(Prcb);


	if (eThread->Tcb.TlsData) 
	{
		auto newPtr = g_memory.Translate(eThread->Tcb.TlsData);
		uint32_t RawTlsDataSize = m_TLS->dwDataEndAddr - m_TLS->dwDataStartAddr;
		uint32_t TlsSizeTotal = RawTlsDataSize + m_TLS->dwSizeofZeroFill;

		uint32_t* TlsData = reinterpret_cast<xbox::addr_xt*>(newPtr);
		
		*TlsData = (uint32_t)((uintptr_t)newPtr + sizeof(uint32_t));
		// Set the actual tls data from xbe.
		TlsData += 1;
		std::memcpy(TlsData, reinterpret_cast<xbox::PVOID>(m_TLS->dwDataStartAddr), RawTlsDataSize);

		if (m_TLS->dwSizeofZeroFill) {
			std::memset(reinterpret_cast<xbox::PBYTE>(TlsData) + RawTlsDataSize, 0, m_TLS->dwSizeofZeroFill);
		}
	}


	if (isHostThread == false)
	{
		/**printf("StackBase: 0x%08X, StackLimit: 0x%08X, StackSize: 0x%08X  NtTibStackBase: 0x%08X,  TlsData: 0x%08X\n",
			eThread->Tcb.StackBase, eThread->Tcb.StackLimit, eThread->Tcb.StackBase - eThread->Tcb.StackLimit,  AdjustedStackBase, eThread->Tcb.TlsData);*/

		int addr = g_memory.MapVirtual(thread) + 0x4;
		auto base = g_memory.base;
		int ptrAddr = X86_LOAD_U32(addr) + (X86_LOAD_U32(tls_indexAddr) * 4);
		X86_STORE_U32(ptrAddr, eThread->Tcb.TlsData);
	}

	*(uint32_t*)(Prcb + 0x04B) = GuestThread::GetCurrentThreadId(); //maybe

	memset(&x86Context, 0, sizeof(x86Context));

	if (isHostThread)
	{
		//x86Context.esp.u32 = Thread->KernelStack;
		x86Context.ebp.u32 = 0;
	}
	else
	{
		x86Context.esp.u32 = EspAddr; // eThread->Tcb.StackBase & 15;
		x86Context.ebp.u32 = x86Context.esp.u32;
	}


	// Default flags (Interrupts enabled)
	x86Context.eflags.rare = 0x202; // IF=1, reserved bit 1=1

	// Floating point control
	x86Context.fpu.cw = 0x037F;
	x86Context.mxcsr = 0x1F80;
	x86Context.kpcr_ptr = g_memory.MapVirtual(thread);

	x86Context.stack_ptr = *(uint32_t*)(NewPcr + 0x04); //unused for now

	isHostThread = false;
}

GuestThreadContext::~GuestThreadContext()
{
	g_userHeap.Free(thread);
}

static void GuestThreadFunc(GuestThreadHandle* hThread)
{
	//printf("GuestThreadFunc(GuestThreadHandle* hThread): Function %08X\n", hThread->params.function);
	hThread->suspended.wait(true);
	GuestThread::Start(hThread->params);
}


GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
	: params(params), suspended((params.flags & 0x1) != 0), thread(GuestThreadFunc, this)
{

}

static uint32_t GetThreadId(const std::thread::id& id)
{
	return *reinterpret_cast<const uint32_t*>(&id);
}

GuestThreadHandle::~GuestThreadHandle()
{
	auto id = GetThreadId(this->thread.get_id());
	std::string s = GetThreadName(params.function);
	LOGFN("Destroy thread {:08X} with ID {} ({})", params.function, id, s);

	if (suspended.load())
	{
		LOGFN_WARNING("GuestThread: Destroying suspended thread {:08X} ({}), forcing resume", params.function, s);
		suspended.store(false, std::memory_order_release);
		suspended.notify_all();
	}

	{
		std::lock_guard lock(suspendMutex);
		resumeRequested = true;
	}
	suspendCV.notify_all();

	if (thread.joinable())
		thread.join();
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
	LOGFN("Wait: Thread {:08X}", params.function);
	assert(timeout == INFINITE);

	if (thread.joinable())
		thread.join();

	return STATUS_WAIT_0;
}



GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
	//printf("GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId), function: %08X\n", params.function);
	auto hThread = CreateKernelObject<GuestThreadHandle>(params);

	if (threadId != nullptr)
	{
		*threadId = GetThreadId(hThread->thread.get_id());

		std::string s = GetThreadName(params.function);

// #ifdef _WIN32
//
// 		if (s != "")
// 			GuestThread::SetThreadName(*threadId, s.c_str());
// #endif

		if (s == "")
			s = "Unknown Thread";


		LOGFN("Started thread {:08X} with ID {} ({})", params.function, *threadId, s);
	}



	return hThread;
}


uint32_t GuestThread::GetCurrentThreadId()
{
	return GetThreadId(std::this_thread::get_id());
}

static thread_local uint32_t g_currentThreadEntry = 0;

uint32_t GuestThread::GetCurrentThreadEntry()
{
	return g_currentThreadEntry;
}

uint32_t GuestThread::Start(const GuestThreadParams& params)
{

	//const auto procMask = (uint8_t)(params.flags >> 24);
	g_currentThreadEntry = params.function;
	GuestThreadContext ctx(params);

	//ctx.x86Context.esp.u32 = (ctx.x86Context.esp.u32 - 16) & ~0xF;
	// 
	auto base = g_memory.base;

	if (params.parameter)
		X86_STORE_U32(ctx.x86Context.esp.u32, params.parameter);

	ctx.x86Context.esp.u32 -= 4; //in x86, the CPU always adjust the stack pointer by 4 bytes before calling a function
	g_memory.FindFunction(params.function)(ctx.x86Context, g_memory.base); 	//call function, if it crashes there the function was most likely not found in the map array
	return ctx.x86Context.eax.u32;
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
	const DWORD MS_VC_EXCEPTION = 0x406D1388;

	typedef struct tagTHREADNAME_INFO
	{
		DWORD dwType; // Must be 0x1000.
		LPCSTR szName; // Pointer to name (in user addr space).
		DWORD dwThreadID; // Thread ID (-1=caller thread).
		DWORD dwFlags; // Reserved for future use, must be zero.
	} THREADNAME_INFO;
#pragma pack(pop)

	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = name;
	info.dwThreadID = threadId;
	info.dwFlags = 0;

	__try
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}
#endif


// Function to clean the stack after a function call
X86_FUNC(StubWithCleanStack)
{
	ctx.esp.u32 += 0x4; // Adjust ESP for the return address 
}

int XAPILIBGetCurrentThreadId()
{
	return GuestThread::GetCurrentThreadId();
}

int32_t GetThreadPriorityImpl(GuestThreadHandle* hThread)
{
#ifdef _WIN32
	return GetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle());
#else 
	return 0;
#endif
}

GUEST_FUNCTION_HOOK_STDCALL(sub_2ADC8D, GetThreadPriorityImpl);

GUEST_FUNCTION_HOOK(sub_2ADF79, XAPILIBGetCurrentThreadId);

GUEST_FUNCTION_STUB(sub_2B126A); // Some kernel hardware check, not needed on PC.
//GUEST_FUNCTION_STUB(sub_381F6C);

//GUEST_FUNCTION_STUB(sub_2B1815); 


