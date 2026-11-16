#include <stdafx.h>
#include <cpu/guest_thread.h>
#include "io/file_system.h"
#include <gpu/video.h>
#include "function.h"
#include <gpu/nv2a.h>

#include <user/config.h>
#include <os/logger.h>
#include "xbox_types.h"
#include "xbox.h"
#include "types.h"
#include "heap.h"
#include "memory.h"
#include <memory>
#include <kernel/kernel.h>
#include <util.h>
#include <gpu/renderstate.h>

// This page contains the implementation of the Xbox kernel functions 
// Some functions were stub since they were considered not needed for Shadow to run.
// Some were inspired by CXBX Reloaded to work properly.
// Other stuff were just faked (as in, tell the game that everything is fine).

// APU registers
uint8_t g_mmio[MMIO_APERTURE_SIZE];

uint32_t XboxSignatureKey[(X_D3DRS_LAST + 1) * 8];
static std::atomic<uint32_t> g_keSetEventGeneration;
constexpr uint32_t GPU_ALIGNMENT = 4096;

//IRQL levels
#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2
#define PROFILE_LEVEL 27
#define CLOCK_LEVEL 28
#define IPI_LEVEL 29
#define POWER_LEVEL 30
#define HIGH_LEVEL 31

#define MAX_BUS_INTERRUPT_LEVEL 27
#define IRQ_BASE 0x30 
#define VECTOR2IRQ(vector)  ((vector)-IRQ_BASE)
#define IRQ2VECTOR(irq)     ((irq)+IRQ_BASE)
#define VECTOR2IRQL(vector) (MAX_BUS_INTERRUPT_LEVEL - VECTOR2IRQ(vector))


class HalSystemInterrupt 
{
public:
	void Assert(bool state) 
	{
		// If the interrupt was marked as Asserted, and was previously not, set the pending flag too!
		if (m_Asserted == 0 && state == 1) 
		{
			m_Pending = true;
		}

		m_Asserted = state;
	};

	void Enable() {
		m_Enabled = true;
	}

	void Disable() {
		m_Enabled = false;
	}

	bool IsEnabled() {
		return m_Enabled;
	}

	bool IsPending() 
	{
		return m_Asserted && m_Pending;
	}

	void SetInterruptMode(xbox::KINTERRUPT_MODE InterruptMode) {
		m_InterruptMode = InterruptMode;
	}

	void Trigger(xbox::PKINTERRUPT Interrupt, X86Context& ctx) {
		// If interrupt was level sensitive, we clear the pending flag, preventing the interrupt from being triggered 
		// until it is deasserted then asserted again. Latched interrupts are triggered until the line is Deasserted!
		if (m_InterruptMode == xbox::KINTERRUPT_MODE::LevelSensitive) {
			m_Pending = false;
		}

		Sleep(16);
		uint32_t funcAddr = (uint32_t)Interrupt->ServiceRoutine;


		if (funcAddr == 0x4286AD)
		{
			return;
		
		}
	

		auto base = g_memory.base;

		uint32_t interrupt = (uint32_t)(uintptr_t)Interrupt;

		X86_STORE_U32(ctx.esp.u32, interrupt);
		X86_STORE_U32(ctx.esp.u32 + 4, (uint32_t)Interrupt->ServiceContext);

		ctx.esp.u32 -= 4; //in x86, the CPU always adjust the stack pointer by 4 bytes before calling a function
		g_memory.FindFunction(funcAddr)(ctx, g_memory.base);	//call function, if it crashes there the function was most likely not found in the map array
	}
private:
	bool m_Asserted = false;
	bool m_Enabled = false;
	xbox::KINTERRUPT_MODE m_InterruptMode;
	bool m_Pending = false;
};


template<typename TGuest>
struct HostObject
{
	typedef TGuest guest_type;
};

struct Event final : KernelObject, HostObject<XKEVENT>
{
	bool manualReset;
	std::atomic<bool> signaled;

	Event(XKEVENT* header)
		: manualReset(!header->Type), signaled(!!header->SignalState)
	{
	}

	Event(bool manualReset, bool initialState)
		: manualReset(manualReset), signaled(initialState)
	{
	}

	uint32_t Wait(uint32_t timeout) override
	{
		if (timeout == 0)
		{
			if (manualReset)
			{
				if (!signaled)
					return STATUS_TIMEOUT;
			}
			else
			{
				bool expected = true;
				if (!signaled.compare_exchange_strong(expected, false))
					return STATUS_TIMEOUT;
			}
		}
		else if (timeout == INFINITE)
		{
			if (manualReset)
			{
				signaled.wait(false);
			}
			else
			{
				while (true)
				{
					bool expected = true;
					if (signaled.compare_exchange_weak(expected, false))
						break;

					signaled.wait(expected);
				}
			}
		}
		else
		{
			assert(false && "Unhandled timeout value.");
		}

		return X_STATUS_SUCCESS;
	}

	bool Set()
	{
		signaled = true;

		if (manualReset)
			signaled.notify_all();
		else
			signaled.notify_one();

		return TRUE;
	}

	bool Reset()
	{
		signaled = false;
		return TRUE;
	}
};


void AvSendTVEncoderOption
(
	IN  PVOID   RegisterBase,
	IN  uint32_t   Option,
	IN  uint32_t   Param,
	OUT uint32_t* Result
)
{
	if (Option == 6)
	{

	}
}

uint32_t AvSetDisplayMode
(
	IN  PVOID   RegisterBase,
	IN  uint32_t   Step,
	IN  uint32_t   Mode,
	IN  uint32_t   Format,
	IN  uint32_t   Pitch,
	IN  uint32_t   FrameBuffer
)
{
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

void NtReadFile()
{
	LOG_UTILITY("!!! STUB !!!");
}

void AvGetSavedDataAddress()
{
	LOG_UTILITY("!!! STUB !!!");
}


int32_t NtFreeVirtualMemory(IN OUT uint32_t* BaseAddress, IN OUT ULONG32 FreeSize, IN uint32_t FreeType)
{
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

int32_t NtAllocateVirtualMemory(IN OUT uint32_t* BaseAddress,
	IN ulong_xt         ZeroBits,
	IN OUT uint32_t* AllocationSize,
	IN dword_xt         AllocationType,
	IN dword_xt         Protect)
{
	return X_STATUS_SUCCESS;
}

void* AvSavedDataAddress;
void AvSetSavedDataAddress(IN  uint32_t* Address)
{
	AvSavedDataAddress = Address;
}

uint32_t DbgPrintV(PCHAR Format, va_list args) {
	vprintf(Format, args);
	return X_STATUS_SUCCESS;
}

uint32_t DbgPrint(PCHAR Format, ...) {
	va_list args;
	va_start(args, Format);
	uint32_t result = DbgPrintV(Format, args);
	va_end(args);
	return result;
}

// The AArch64 procedure call standard makes va_list a structure. Unlike the pointer it is
// on other hosts, the argument translator cannot make one out of a guest stack slot, so
// the guest's DbgPrint prints its format string as it is there.
#if defined(__aarch64__) && !defined(_WIN32) && !defined(__APPLE__)
#define SHADOW_RECOMP_VA_LIST_IS_STRUCT

uint32_t DbgPrintFormat(PCHAR Format) {
	if (Format != nullptr)
		fputs(Format, stdout);
	return X_STATUS_SUCCESS;
}
#endif

void ExFreePool(void* ptr)
{
	g_userHeap.Free(g_memory.Translate((uint32_t)(uintptr_t)ptr));
}

void* ExAllocatePoolWithTag
(
	IN uint32_t NumberOfBytes,
	IN uint32_t Tag
)
{
	void* ptr = reinterpret_cast<PVOID>(g_memory.MapVirtual(g_userHeap.Alloc(NumberOfBytes > 0 ? NumberOfBytes : 1)));
	LOGF_UTILITY("ptr: {}", ptr);
	return ptr;
}

uint32_t ExQueryPoolBlockSize
(
	IN PVOID PoolBlock
)
{
	LOGF_UTILITY("!!! STUB !!! PoolBlock {}, returning dummy 4096", fmt::ptr(PoolBlock));
	auto size = g_userHeap.Size(PoolBlock);
	return size;
}

LONG ExQueryNonVolatileSetting(
	IN  dword_xt  ValueIndex,
	OUT dword_xt* Type,
	OUT uint32_t* Value,
	IN  size_xt   ValueLength,
	OUT uint32_t* ResultLength OPTIONAL
)
{

	// Default to REG_DWORD (common for Xbox settings)
	if (Type)
		*Type = REG_DWORD;

	// Handle known settings
	switch (ValueIndex)
	{
	case 17:
		return X_STATUS_SUCCESS;
	case 0x1000: // Language (en-US)
		if (ValueLength >= 4)
		{
			*(DWORD*)Value = 0x0409; // English
			if (ResultLength)
				*ResultLength = 4;
			return X_STATUS_SUCCESS;
		}
		break;

	case 0x2002:
		if (ValueLength >= 4)
		{
			*(DWORD*)Value = 0; // 640x480
			if (ResultLength) *ResultLength = 4;
			return X_STATUS_SUCCESS;
		}
		break;

	case xbox::XC_AUDIO:
		if (ValueLength >= 4)
		{
			*(DWORD*)Value = 0; // force stereo
			if (ResultLength) *ResultLength = 4;
			return X_STATUS_SUCCESS;
		}
		break;

	case xbox::XC_FACTORY_GAME_REGION:
		if (ValueLength >= 4)
		{
			*(DWORD*)Value = 0x01;
			if (ResultLength) *ResultLength = 4;
			return X_STATUS_SUCCESS;
		}
		break;
	default:
		break;
	}

	// Fallback: Log unknown indices and return "not found"
	LOGF_WARNING("Unhandled index 0x{:X}", ValueIndex);
	return X_STATUS_OBJECT_NAME_NOT_FOUND;
}

LONG FscSetCacheSize(ULONG NumberOfCachePages)
{
	return X_STATUS_SUCCESS;
}

uint32_t HalGetInterruptVector(uint32_t BusInterruptLevel, std::uint8_t* Irql)
{
	uint32_t dwVector = 0;

	if ((BusInterruptLevel >= 0) && (BusInterruptLevel <= MAX_BUS_INTERRUPT_LEVEL))
	{
		dwVector = IRQ2VECTOR(BusInterruptLevel);
		if (Irql)
			*Irql = (uchar_xt)VECTOR2IRQL(dwVector);
	}

	return dwVector;
}

//allows drivers or system components to register a callback function to be notified when the console is about to shut down.
void HalRegisterShutdownNotification(int32_t* ShutdownRegistration, uint8_t Register)
{
	LOGF_UTILITY("!!! STUB !!! ({}, {})", fmt::ptr(ShutdownRegistration), Register);
}

void HalReturnToFirmware(ULONG Action)
{
	LOGF_UTILITY("!!! STUB !!! ({}) Would reboot", Action);
}

void HalReadWritePCISpace
(
	uint32_t   BusNumber,
	uint32_t   SlotNumber,
	uint32_t   RegisterNumber,
	uint32_t* Buffer,
	uint32_t   Length,
	std::uint8_t WritePCISpace
)
{
	LOGF_UTILITY("!!! STUB !!! {}PCISpace(Bus {}, Slot {}, Buffer {}, Len {})",
		WritePCISpace ? "Write" : "Read",
		BusNumber, SlotNumber, fmt::ptr(Buffer), Length);

	if (WritePCISpace)
		return;

	// For reads, zero the buffer
	memset(Buffer, 0, Length);
}

LONG IoCreateSymbolicLink(uint32_t* SymbolicLinkName_, uint32_t* DeviceName_)
{

	LOG_UTILITY("!!! STUB !!!");

	return X_STATUS_SUCCESS;
}

LONG IoDeleteSymbolicLink(uint32_t* SymbolicLinkName_)
{
	xbox::PSTRING SymbolicLinkName = (xbox::PSTRING)SymbolicLinkName_;
	LOGF_UTILITY("!!! STUB !!! ({})",
		std::string_view(SymbolicLinkName->Buffer, SymbolicLinkName->Length));

	return X_STATUS_SUCCESS;
}

int32_t KeBugCheckEx(IN std::uint32_t BugCheckCode, IN uint32_t* BugCheckParameter1, IN uint32_t* BugCheckParameter2, IN PVOID BugCheckParameter3, IN PVOID BugCheckParameter4)
{

	static bool KeBugCheckIgnored = false;

	if (KeBugCheckIgnored)
	{
		return S_OK;
	}

	char buffer[1024];
	sprintf(buffer, "The running software triggered KeBugCheck with the following information\n"
		"BugCheckCode: 0x%.8X\n"
		"BugCheckParameter1: 0x%p\n"
		"BugCheckParameter2: 0x%p\n"
		"BugCheckParameter3: 0x%p\n"
		"BugCheckParameter4: 0x%p\n"
		"\nThis is the Xbox equivalent to a BSOD and would cause the console to automatically reboot\n"
		"\nContinue Execution (Not Recommended)?\n",
		BugCheckCode, BugCheckParameter1, BugCheckParameter2, BugCheckParameter3, BugCheckParameter4);


	__builtin_debugtrap();
	KeBugCheckIgnored = true;
	return S_OK;
}


void KeBugCheck(IN uint32_t BugCheckMode)
{
	LOGF_ERROR("BugCheckMode {}", BugCheckMode);
	KeBugCheckEx(BugCheckMode, 0, 0, 0, 0);
}

std::uint8_t KeRaiseIrqlToDpcLevel()
{
	LOG_UTILITY("!!! STUB !!!");
	return DISPATCH_LEVEL;
}

void KeQuerySystemTime(uint64_t* CurrentTime_)
{
	constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

	auto now = std::chrono::system_clock::now();
	auto timeSinceEpoch = now.time_since_epoch();

	int64_t currentTime100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timeSinceEpoch).count();
	currentTime100ns += FILETIME_EPOCH_DIFFERENCE;

	*CurrentTime_ = currentTime100ns;
}

LONG KeQueryBasePriorityThread(GuestThreadHandle* Thread)
{
	return 0;
}

typedef struct _KINTERRUPT
{
	/* 0x00= 0 */ uint32_t ServiceRoutine;
	/* 0x04= 4 */ uint32_t ServiceContext;
	/* 0x08= 8 */ ulong_xt BusInterruptLevel;
	/* 0x0C=12 */ ulong_xt Irql; // Was : unsigned char     KIRQL; unsigned char     PaddingA[0x03];
	/* 0x10=16 */ boolean_xt Connected;
	/* 0x11=17 */ boolean_xt ShareVector;
	/* 0x12=18 */ uint32_t Mode;
	/* 0x14=20 */ ulong_xt ServiceCount;
	/* 0x18=24 */ ulong_xt DispatchCode[DISPATCH_SIZE]; // Same as old : unsigned char ISR[0x58];
}
KINTERRUPT, * PKINTERRUPT;


void KeInitializeInterrupt(xbox::PKINTERRUPT Interrupt, uint32_t ServiceRoutine,
	uint32_t ServiceContext, ULONG Vector, std::uint8_t Irql,
	xbox::_KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector)
{
	Interrupt->ServiceRoutine = ServiceRoutine;
	Interrupt->ServiceContext = ServiceContext;
	Interrupt->BusInterruptLevel = VECTOR2IRQ(Vector);
	Interrupt->Irql = Irql;
	Interrupt->Connected = FALSE;
	Interrupt->Mode = InterruptMode;
}

void KeDisconnectInterrupt(uint32_t* Interrupt_)
{
	xbox::PKINTERRUPT Interrupt = (xbox::PKINTERRUPT)Interrupt;
	LOGF_UTILITY("!!! STUB !!! ({})", fmt::ptr(Interrupt));
}

#define MAX_BUS_INTERRUPT_LEVEL 27
xbox::KINTERRUPT* EmuInterruptList[MAX_BUS_INTERRUPT_LEVEL + 1] = { 0 };
HalSystemInterrupt HalSystemInterrupts[MAX_BUS_INTERRUPT_LEVEL + 1];

void HalEnableSystemInterrupt
(
	IN ulong_xt BusInterruptLevel,
	IN uint32_t InterruptMode
)
{
	HalSystemInterrupts[BusInterruptLevel].Enable();
	HalSystemInterrupts[BusInterruptLevel].SetInterruptMode((xbox::KINTERRUPT_MODE)InterruptMode);
}

std::uint8_t KeConnectInterrupt(xbox::KINTERRUPT* InterruptObject)
{
	BOOLEAN ret = FALSE;

	if (!InterruptObject->Connected)
	{
		// One interrupt per IRQ - only set when not set yet:
		if (EmuInterruptList[InterruptObject->BusInterruptLevel] == NULL)
		{
			InterruptObject->Connected = TRUE;
			EmuInterruptList[InterruptObject->BusInterruptLevel] = InterruptObject;
			HalEnableSystemInterrupt(InterruptObject->BusInterruptLevel, InterruptObject->Mode);
			ret = TRUE;
		}
	}

	return ret;
}

uint32_t GuestTimeoutToMilliseconds(int64_t* timeout)
{
	return timeout ? (*timeout * -1) / 10000 : INFINITE;
}

LONG KeDelayExecutionThread(uint8_t WaitMode, BOOLEAN Alertable, int64_t* Interval)
{
	if (Alertable)
		return STATUS_USER_APC;

	uint32_t timeout = GuestTimeoutToMilliseconds(Interval);

#ifdef _WIN32
	Sleep(timeout);
#else
	if (timeout == 0)
		std::this_thread::yield();
	else
		std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
#endif

	return X_STATUS_SUCCESS;
}

std::uint8_t KeCancelTimer(uint32_t* Timer_)
{
	xbox::PKTIMER Timer = (xbox::PKTIMER)Timer_;
	LOGF_UTILITY("!!! STUB !!! ({})", fmt::ptr(Timer));
	return TRUE;
}


VOID KeInitializeDpc(_KDPC* Dpc, uint32_t* DeferredRoutine, uint32_t* DeferredContext)
{
	Dpc->Type = 19;
	Dpc->Inserted = FALSE;
	Dpc->DeferredRoutine.v = (uint32_t)(uintptr_t)DeferredRoutine;
	Dpc->DeferredContext.v = (uint32_t)(uintptr_t)DeferredContext;
}

typedef struct _XKTIMER
{
	XDISPATCHER_HEADER   Header;           // 0x00
	ULARGE_INTEGER      DueTime;          // 0x10
	LIST_ENTRY          TimerListEntry;   // 0x18
	struct _KDPC* Dpc;              // 0x20
	long_xt             Period;           // 0x24
}
KTIMER, * PKTIMER;

void InitializeListHead(PXLIST_ENTRY pListHead);
VOID KeInitializeTimerEx(_XKTIMER* Timer, uint32_t Type)
{
	Timer->Header.Type = Type + 8;
	Timer->Header.Inserted = FALSE;
	Timer->Header.Size = sizeof(xbox::_KTIMER) / sizeof(ULONG);
	Timer->Header.SignalState = 0;

	InitializeListHead((PXLIST_ENTRY)&(Timer->Header.WaitListHead));

	Timer->TimerListEntry.Blink = NULL;
	Timer->TimerListEntry.Flink = NULL;
	Timer->DueTime.QuadPart = 0;
	Timer->Period = 0;
}

int32_t KeRestoreFloatingPointState(uint32_t* SaveArea_)
{
	LONG ret = X_STATUS_SUCCESS;
	return ret;
}

LONG KeSaveFloatingPointState(uint32_t* SaveArea_)
{
	return X_STATUS_SUCCESS;
}

int map_xbox_priority(int xbox_priority) {
	switch (xbox_priority) {
	case -2:
		return THREAD_PRIORITY_LOWEST;
	case 1:
	case 2:
		return THREAD_PRIORITY_BELOW_NORMAL;
	case 8:
		return THREAD_PRIORITY_NORMAL;
	case 16:
		return THREAD_PRIORITY_TIME_CRITICAL;
	default:
		return THREAD_PRIORITY_NORMAL;
	}
}

std::int32_t KeSetBasePriorityThread(GuestThreadHandle* hThread, LONG priority)
{
#ifdef _WIN32
	if (priority == 16)
	{
		priority = 15;
	}
	else if (priority == -16)
	{
		priority = -15;
	}
	else if (priority == 8)
	{
		priority = THREAD_PRIORITY_NORMAL;
	}

	if (!SetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle(), priority)) 
	{
		LOGF_ERROR("Failed to set priority: {}", GetLastError());
	}

#endif

	return priority;
}

std::uint8_t KeSetDisableBoostThread(uint32_t* Thread_, BOOLEAN Disable)
{
	LOG_UTILITY("!!! STUB !!!");
	return 0;
}

LONG KeSetEvent(XKEVENT* pEvent, uint32_t Increment, bool Wait)
{
	bool result = QueryKernelObject<Event>(*pEvent)->Set();

	++g_keSetEventGeneration;
	g_keSetEventGeneration.notify_all();

	return result;
}

BOOLEAN KeSetTimer(uint32_t* Dpc, uint32_t* DueTime, uint32_t* Timer)
{
	return FALSE; // Timer wasn't already active
}

VOID KeStallExecutionProcessor(ULONG Microseconds)
{
	std::this_thread::sleep_for(std::chrono::microseconds(Microseconds));
}

BOOLEAN KeSynchronizeExecution(uint32_t* Interrupt_, uint32_t* SynchronizeRoutine_, uint32_t SynchronizeContext)
{
	xbox::PKSYNCHRONIZE_ROUTINE SynchronizeRoutine = (xbox::PKSYNCHRONIZE_ROUTINE)SynchronizeRoutine_;
	xbox::PKINTERRUPT Interrupt = (xbox::PKINTERRUPT)Interrupt_;
	LOGF_UTILITY("!!! STUB !!! (Int {}, Routine {}, Ctx 0x{:X})", fmt::ptr(Interrupt), fmt::ptr(SynchronizeRoutine), SynchronizeContext);
	return SynchronizeRoutine(&SynchronizeContext);
}

LONG KeWaitForSingleObject(XDISPATCHER_HEADER* Object, uint32_t WaitReason, uint32_t WaitMode, bool Alertable, int64_t* Timeout)
{
	const uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);
	assert(timeout == INFINITE);

	switch (Object->Type)
	{
	case 0:
	case 1:
		QueryKernelObject<Event>(*Object)->Wait(timeout);
		break;
	default:
		assert(false && "Unrecognized kernel object type.");
		return STATUS_TIMEOUT;
	}

	return X_STATUS_SUCCESS;
}

std::uint8_t KfRaiseIrql(std::uint8_t NewIrql)
{
	std::uint8_t old = PASSIVE_LEVEL;
	return old; // Return previous IRQL
}

uint32_t* MmAllocateContiguousMemory(uint32_t NumberOfBytes)
{
	return reinterpret_cast<uint32_t*>(g_memory.MapVirtual(g_userHeap.AllocPhysical(NumberOfBytes, GPU_ALIGNMENT)));
}

uint32_t* MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress,
	uint32_t HighestAcceptableAddress, uint32_t Alignment,
	uint32_t ProtectionType
)
{
	return reinterpret_cast<uint32_t*>(g_memory.MapVirtual(g_userHeap.AllocPhysical(NumberOfBytes, Alignment)));
}

PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, ULONG Protect
) {

	return reinterpret_cast<PVOID>(g_memory.MapVirtual(g_userHeap.AllocPhysical(NumberOfBytes, GPU_ALIGNMENT)));
}

void MmFreeContiguousMemory(uint32_t guestAddress)
{
	if (guestAddress != NULL)
		g_userHeap.Free(g_memory.Translate(guestAddress));
}

VOID MmFreeSystemMemory(uint32_t* BaseAddress, uint32_t NumberOfBytes)
{
	LOGF_UTILITY("!!! STUB !!! ({}, {} bytes)", fmt::ptr(BaseAddress), NumberOfBytes);
}

uint32_t MmAllocateSystemMemory(uint32_t NumberOfBytes, ULONG Protect)
{
	LOGF_UTILITY("{} bytes, Prot 0x{:X}", NumberOfBytes, Protect);
	return g_memory.MapVirtual(g_userHeap.Alloc(NumberOfBytes));
}

void KfLowerIrql(std::uint8_t NewIrql)
{
	//printf("[STUB] KfLowerIrql(%d)\n", NewIrql);
}

LONG NtClose(uint32_t Handle)
{

	if (Handle == 0)
	{
		LOG_WARNING("No handle detected for this thread, aborting deleting handle.");
		return X_STATUS_SUCCESS;
	}

	if (Handle == GUEST_INVALID_HANDLE_VALUE)
		return 0xFFFFFFFF;

	if (IsKernelObject(Handle))
	{
		DestroyKernelObject(Handle);
		return 0;
	}
	else
	{
		assert(false && "Unrecognized kernel object.");
		return 0xFFFFFFFF;
	}

	return 1;
}

LONG NtCreateEvent(uint32_t* EventHandle, void* bjectAttributes, uint32_t EventType, std::uint8_t InitialState)
{
	*EventHandle = GetKernelHandle(CreateKernelObject<Event>(!EventType, !!InitialState));
	return X_STATUS_SUCCESS;
}

#ifdef _WIN32
#include <windows.h>

using FileHandleType = HANDLE;
#define INVALID_HANDLE_VALUE_WIN32 INVALID_HANDLE_VALUE
#else
using FileHandleType = int;
#define INVALID_HANDLE_VALUE_WIN32 (-1)
#endif

LONG NtCreateFile(
	OUT uint32_t* FileHandle,
	IN  uint32_t         DesiredAccess,
	IN  uint32_t ObjectAttributes_,
	OUT uint32_t* IoStatusBlock_,
	IN  uint32_t* AllocationSize_ OPTIONAL,
	IN  uint32_t               FileAttributes,
	IN  uint32_t               ShareAccess,
	IN  uint32_t               CreateDisposition,
	IN  uint32_t               CreateOptions
)
{
	LOG_UTILITY("!!! STUB !!!");
	return 0;
}

LONG NtDeviceIoControlFile(
	HANDLE FileHandle,
	HANDLE Event,
	uint32_t* ApcRoutine,
	PVOID ApcContext,
	uint32_t* IoStatusBlock,
	ULONG IoControlCode,
	PVOID InputBuffer,
	ULONG InputBufferLength,
	PVOID OutputBuffer,
	ULONG OutputBufferLength
) {
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

LONG NtFlushBuffersFile(HANDLE FileHandle, uint32_t* IoStatusBlock)
{
#ifdef _WIN32
	FlushFileBuffers(FileHandle);
#else
	fsync((int)(intptr_t)FileHandle);
#endif
	return X_STATUS_SUCCESS;
}

LONG NtFsControlFile(
	uint32_t* FileHandle,
	uint32_t* Event,
	uint32_t* ApcRoutine,
	uint32_t* ApcContext,
	uint32_t* IoStatusBlock,
	ULONG FsControlCode,
	uint32_t* InputBuffer,
	ULONG InputBufferLength,
	uint32_t* OutputBuffer,
	ULONG OutputBufferLength
) {
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

LONG NtOpenFile(
	uint32_t* FileHandle,
	ACCESS_MASK DesiredAccess,
	uint32_t* ObjectAttributes_,
	uint32_t* IoStatusBlock_,
	ULONG ShareAccess,
	ULONG OpenOptions
) {

	return -1; // NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes_, IoStatusBlock_, 0, ShareAccess, FILE_OPEN, OpenOptions, NULL);
}

LONG NtOpenSymbolicLinkObject(OUT PHANDLE LinkHandle, IN void* ObjectAttributes
) {
	*LinkHandle = reinterpret_cast<HANDLE>(0xCAFEBABE);
	return X_STATUS_SUCCESS;
}

LONG NtQueryDirectoryFile(
	IN  HANDLE FileHandle,
	IN  HANDLE Event OPTIONAL,
	IN  uint32_t* ApcRoutine,
	IN  uint32_t* ApcContext,
	OUT uint32_t* IoStatusBlock,
	OUT uint32_t* FileInformation_,
	IN  uint32_t Length,
	IN  uint32_t FileInformationClass,
	IN  uint32_t* FileMask,
	IN  std::uint8_t RestartScan
)
{
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

//todo figure out if this is correct
LONG NtQueryFullAttributesFile(uint32_t* ObjectAttributes_, uint32_t* FileInformation_)
{
	return -1073741772;
}

LONG NtQueryInformationFile(
	uint32_t* FileHandle,
	uint32_t* IoStatusBlock_,
	uint32_t* FileInformation,
	ULONG FileInformationLength,
	uint32_t FileInformationClass_
)
{
	LOG_UTILITY("!!! STUB !!!");

	return 0;
}

LONG NtQuerySymbolicLinkObject(HANDLE LinkHandle, void* TargetName_, PULONG ReturnedLength
)
{

	xbox::PUNICODE_STRING TargetName = (xbox::PUNICODE_STRING)TargetName_;
	// Xbox uses UTF-16 strings with 16-bit wchar_xt
	static const xbox::wchar_xt dummy[] = {
		'\\', 'D', 'e', 'v', 'i', 'c', 'e', '\\',
		'H', 'a', 'r', 'd', 'd', 'i', 's', 'k', '0', '\\',
		'P', 'a', 'r', 't', 'i', 't', 'i', 'o', 'n', '1', 0
	};

	// Calculate required bytes (including null terminator)
	size_t required_bytes = (sizeof(dummy) / sizeof(xbox::wchar_xt)) * sizeof(xbox::wchar_xt);

	// Check if buffer is large enough
	if (TargetName->MaximumLength < required_bytes) {
		if (ReturnedLength) *ReturnedLength = static_cast<ULONG>(required_bytes);
		return X_STATUS_BUFFER_TOO_SMALL;
	}

	// Copy the string
	memcpy(TargetName->Buffer, dummy, required_bytes);
	TargetName->Length = static_cast<USHORT>(required_bytes - sizeof(xbox::wchar_xt)); // Length without null terminator

	if (ReturnedLength) *ReturnedLength = static_cast<ULONG>(required_bytes);
	return X_STATUS_SUCCESS;
}

LONG NtQueryVirtualMemory(uint32_t* BaseAddress, uint32_t* Buffer)
{
	LOG_UTILITY("STUB NtQueryVirtualMemory\n");

	if (!Buffer)
		return X_STATUS_UNSUCCESSFUL;
	return X_STATUS_SUCCESS;
}


uint32_t MmGetPhysicalAddress(uint32_t BaseAddress)
{
	LOGF_UTILITY("0x{:08X}", BaseAddress);
	return BaseAddress;
}

LONG MmLockUnlockBufferPages(uint32_t* BaseAddress, uint32_t NumberOfBytes, BOOLEAN Lock)
{
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS; // No-op in recompiler
}

LONG MmLockUnlockPhysicalPage(uint32_t PhysicalAddress_, BOOLEAN Lock)
{
	xbox::physical_address_xt PhysicalAddress = PhysicalAddress_;
	uint32_t addr = static_cast<uint32_t>(PhysicalAddress);
	LOGF_UTILITY("(0x{:08X}, {})", addr, Lock ? "Lock" : "Unlock");
	return X_STATUS_SUCCESS;
}

VOID MmPersistContiguousMemory(uint32_t* BaseAddress, uint32_t NumberOfBytes, BOOLEAN Persist)
{
	LOG_UTILITY("!!! STUB !!!");
}

LONG MmQueryAddressProtect(uint32_t* BaseAddress, uint32_t* Protect)
{
	LOG_UTILITY("!!! STUB !!!");
	*Protect = PAGE_READWRITE;
	return X_STATUS_SUCCESS;
}

SIZE_T MmQueryAllocationSize(uint32_t* BaseAddress)
{
	return g_userHeap.Size(BaseAddress);
}

LONG MmSetAddressProtect(uint32_t* BaseAddress, SIZE_T NumberOfBytes, ULONG NewProtect)
{
	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

LONG NtWaitForSingleObject(uint32_t Handle, BOOLEAN Alertable, int64_t* Timeout)
{
	uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);
	assert(timeout == 0 || timeout == INFINITE);

	if (IsKernelObject(Handle))
	{
		return GetKernelObject(Handle)->Wait(timeout);
	}
	else
	{
		assert(false && "Unrecognized handle value.");
	}

	return STATUS_TIMEOUT;
}

LONG NtSetEvent(Event* handle, uint32_t* PreviousState)
{
	LOG_UTILITY(" Handle Set");
	handle->Set();

	return X_STATUS_SUCCESS;
}

LONG NtWriteFile(
	uint32_t* FileHandle,
	uint32_t* Event,
	uint32_t** ApcRoutine_,
	uint32_t* ApcContext,
	uint32_t** IoStatusBlock_,
	uint32_t* Buffer,
	ULONG Length,
	uint32_t* ByteOffset
) {

	LOG_UTILITY("!!! STUB !!!");
	return X_STATUS_SUCCESS;
}

void NtYieldExecution() {
	std::this_thread::yield();
}

SIZE_T RtlCompareMemoryUlong(uint32_t* Buffer, uint32_t Length, ULONG Pattern)
{
	LOG_UTILITY("!!! STUB !!! Pretend bytes match");
	return Length; // Pretend all bytes match
}

BOOLEAN RtlEqualString(PCSTR String1, PCSTR String2)
{
	LOG_UTILITY("!!! STUB !!!");
	return strcmp(String1, String2) == 0;
}

LONG NtSuspendThread(GuestThreadHandle* ThreadHandle, uint32_t* PreviousSuspendCount)
{
	if (!ThreadHandle) 
		return STATUS_INVALID_HANDLE;

	assert(ThreadHandle != GetKernelObject(CURRENT_THREAD_HANDLE) && ThreadHandle->thread.get_id() == std::this_thread::get_id());

	std::unique_lock lock(ThreadHandle->suspendMutex);
	ThreadHandle->suspendCV.wait(lock, [&] { return ThreadHandle->resumeRequested.load(); });
	ThreadHandle->resumeRequested = false;

	return S_OK;
}

LONG NtResumeThread(GuestThreadHandle* ThreadHandle, uint32_t* PreviousSuspendCount)
{
	if (!ThreadHandle)
		return STATUS_INVALID_HANDLE;

	assert(ThreadHandle != GetKernelObject(CURRENT_THREAD_HANDLE));

	{
		std::lock_guard lock(ThreadHandle->suspendMutex);
		ThreadHandle->resumeRequested = true;
	}
	ThreadHandle->suspendCV.notify_all();

	ThreadHandle->suspended = false;
	ThreadHandle->suspended.notify_all();

	return X_STATUS_SUCCESS;
}

typedef void_xt(NTAPI* PKSTART_ROUTINE)
(
	IN PVOID StartContext
	);

typedef void_xt(NTAPI* PKSYSTEM_ROUTINE)
(
	IN PKSTART_ROUTINE StartRoutine OPTIONAL,
	IN PVOID StartContext OPTIONAL
	);



LONG PsCreateSystemThreadEx(
	OUT uint32_t* ThreadHandle,
	IN  ulong_xt           ThreadExtensionSize,
	IN  ulong_xt           KernelStackSize,
	IN  ulong_xt           TlsDataSize,
	OUT uint32_t          ThreadId OPTIONAL,
	IN  uint32_t StartRoutine,
	IN  uint32_t           StartContext,
	IN  boolean_xt         CreateSuspended,
	IN  boolean_xt         DebuggerThread,
	IN  uint32_t SystemRoutine OPTIONAL)
{
	
	if (KernelStackSize < KERNEL_STACK_SIZE)
		KernelStackSize = KERNEL_STACK_SIZE;

	// Double the stack size, this is to account for the overhead HLE patching adds to the stack
	uint32_t hKernelStackSize = KernelStackSize * 2;

	// round up to the next page boundary if un-aligned
	hKernelStackSize = RoundUp(hKernelStackSize, PAGE_SIZE);

	uint32_t hostThreadId;
	auto handle = GetKernelHandle(GuestThread::Start({ (uint32_t)StartRoutine, StartContext, 0, CreateSuspended ? 1u : 0u, TlsDataSize, hKernelStackSize }, &hostThreadId));
	*ThreadHandle = handle;

	return X_STATUS_SUCCESS;
}

void PsTerminateSystemThread(LONG ExitStatus)
{
	// No-op (thread cleanup is handled by the recompiler)
}

// Turns a handle into a kernel object pointer.  The ObjectType parameter
// specifies what type of object it is. 
LONG ObReferenceObjectByHandle(uint32_t Handle, uint32_t ObjectType, uint32_t Object)
{
	auto base = g_memory.base;
	X86_STORE_U32(Object, Handle);
	return X_STATUS_SUCCESS;
}

void ObfDereferenceObject(PVOID Object) {
	// No-op
}

void RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* CritSec)
{
	CritSec->Event.Absolute = 0;
	CritSec->LockCount = -1;
	CritSec->RecursionCount = 0;
	CritSec->OwningThread = 0;
}

void RtlEnterCriticalSection(X86Context& ctx, XRTL_CRITICAL_SECTION* CritSec)
{
	uint32_t thisThread = ctx.kpcr_ptr;
	assert(thisThread != NULL);

	std::atomic_ref owningThread(CritSec->OwningThread);

	while (true)
	{
		uint32_t previousOwner = 0;

		if (owningThread.compare_exchange_weak(previousOwner, thisThread) || previousOwner == thisThread)
		{
			CritSec->RecursionCount++;
			return;
		}

		owningThread.wait(previousOwner);
	}
}

void RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* CritSec)
{
	CritSec->RecursionCount--;

	if (CritSec->RecursionCount != 0)
		return;

	std::atomic_ref owningThread(CritSec->OwningThread);
	owningThread.store(0);
	owningThread.notify_one();
}


void RtlInitAnsiString(_XANSI_STRING* Destination, char* Source)
{
	const uint16_t length = Source ? (uint16_t)strlen(Source) : 0;
	Destination->Length = length;
	Destination->MaximumLength = length + 1;
	Destination->Buffer = (xpointer<char>)(Source);
}

LONG NtQueryVolumeInformationFile(
	uint32_t* FileHandle,
	uint32_t* IoStatusBlock_,
	uint32_t* FsInformation,
	ULONG Length,
	uint32_t FsInformationClass_
)
{

	return X_STATUS_SUCCESS;
}

LONG NtSetInformationFile(
	uint32_t* FileHandle,
	uint32_t* IoStatusBlock_,
	uint32_t* FileInformation,
	ULONG Length,
	uint32_t* FileInformationClass
)
{
	return X_STATUS_SUCCESS;
}

void HalInitiateShutdown() {
	LOG_UTILITY("!!! STUB !!!");
}

LONG XeLoadSection(const char* Name)
{
	return X_STATUS_SUCCESS;
}

LONG XeUnloadSection(const char* Name)
{
	return X_STATUS_SUCCESS;
}

void XcSHAInit(uint32_t* Context)
{
	memset(Context, 0, 20); //SHA-1 context size
}

void XcSHAUpdate(uint32_t* Context, const uint32_t* Data, ULONG Length)
{
	// No-op
}

void XcSHAFinal(uint32_t* Context, uint32_t* Digest)
{
	memset(Digest, 0xAA, 20); //Dummy SHA-1 hash
}

void XcHMAC(uint32_t* Key, ULONG KeySize, uint32_t* Data, ULONG DataSize, uint32_t* Digest)
{
	memset(Digest, 0xBB, 20);
}

uint32_t XGetLanguage()
{
	return (uint32_t)Config::Language.Value;
}


void RtlRaiseException_(uint32_t* ExceptionRecord)
{
	std::terminate();
}

void RtlUnwind_()
{
	LOG_UTILITY("!!! STUB !!!");
}

uint32_t RtlNtStatusToDosError(xbox::ntstatus_xt status)
{
	if (!status || (status & 0x20000000))
		return TRUE;

	return TRUE;
}

uint32_t NtWaitForSingleObjectEx(uint32_t Handle, uint32_t WaitMode, uint32_t Alertable, int64_t* Timeout)
{
	uint32_t timeout = GuestTimeoutToMilliseconds(Timeout);
	assert(timeout == 0 || timeout == INFINITE);

	if (IsKernelObject(Handle))
	{
		return GetKernelObject(Handle)->Wait(timeout);
	}
	else
	{
		assert(false && "Unrecognized handle value.");
	}

	return STATUS_TIMEOUT;
}

// See also :
// https://github.com/reactos/reactos/blob/40a16a9cf1cdfca399e9154b42d32c30b63480f5/reactos/drivers/filesystems/udfs/Include/env_spec_w32.h
void InitializeListHead(PXLIST_ENTRY pListHead)
{
	pListHead->Flink = pListHead->Blink = (uint32_t)(uintptr_t)pListHead.get();
}

std::uint8_t KeInsertQueueDpc(PKDPC Dpc, uint32_t* Argument1, uint32_t* Argument2)
{
	return FALSE;
}

std::uint8_t KeRemoveQueueDpc(KDPC* Dpc)
{
	return FALSE;
}


std::unique_ptr<NV2AState> m_nv2a_state;
uint8_t* g_nv2aPtr = nullptr;

// D3DK::CMiniport::MapRegisters
// Because the game stores hardware address directly as a pointer, we have to swap it to our nv2a array that handle basic hardware stuff. 
// We need to do that because we have to simulate vblank call since these are needed for the game to progress normally
// Thus, whenever the game will write a vblank signal in our array, we can check that and simulate the wait properly.
X86_FUNC_IMPL(__imp__sub_3D7B81);
X86_OVERRIDE_FUNC(sub_3D7B81)
{
	__imp__sub_3D7B81(ctx, base);
	X86_STORE_U32(ctx.ecx.u32, g_memory.MapVirtual(g_nv2aPtr)); //this will make the game write hardware value in our array directly.
}


GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlRaiseException, RtlRaiseException_);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmPersistContiguousMemory, MmPersistContiguousMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmAllocateContiguousMemory, MmAllocateContiguousMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlInitAnsiString, RtlInitAnsiString);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtClose, NtClose);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__ExQueryNonVolatileSetting, ExQueryNonVolatileSetting);
GUEST_FUNCTION_HOOK_STDCALL(__imp__HalReturnToFirmware, HalReturnToFirmware);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmFreeContiguousMemory, MmFreeContiguousMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError);
GUEST_FUNCTION_HOOK_STDCALL(__imp__FscSetCacheSize, FscSetCacheSize);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtCreateEvent, NtCreateEvent);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtSetEvent, NtSetEvent);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeDelayExecutionThread, KeDelayExecutionThread)
GUEST_FUNCTION_HOOK_FASTCALL(__imp__ObfDereferenceObject, ObfDereferenceObject);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle);
GUEST_FUNCTION_HOOK_STDCALL(__imp__PsCreateSystemThreadEx, PsCreateSystemThreadEx);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSetDisableBoostThread, KeSetDisableBoostThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtResumeThread, NtResumeThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtSuspendThread, NtSuspendThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtYieldExecution, NtYieldExecution);
GUEST_FUNCTION_HOOK_STDCALL(__imp__PsTerminateSystemThread, PsTerminateSystemThread);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtWaitForSingleObject, NtWaitForSingleObject);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtFsControlFile, NtFsControlFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtDeviceIoControlFile, NtDeviceIoControlFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XcSHAInit, XcSHAInit);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XcSHAUpdate, XcSHAUpdate);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XcSHAFinal, XcSHAFinal);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeQuerySystemTime, KeQuerySystemTime);

GUEST_FUNCTION_HOOK_STDCALL(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK_STDCALL(__imp__IoCreateSymbolicLink, IoCreateSymbolicLink);
GUEST_FUNCTION_HOOK_STDCALL(__imp__IoDeleteSymbolicLink, IoDeleteSymbolicLink);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtQuerySymbolicLinkObject, NtQuerySymbolicLinkObject);
GUEST_FUNCTION_HOOK_STDCALL(__imp__NtOpenSymbolicLinkObject, NtOpenSymbolicLinkObject);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmFreeSystemMemory, MmFreeSystemMemory);

GUEST_FUNCTION_HOOK_STDCALL(__imp__MmAllocateSystemMemory, MmAllocateSystemMemory);
GUEST_FUNCTION_HOOK_FASTCALL(__imp__KfLowerIrql, KfLowerIrql);

GUEST_FUNCTION_HOOK_STDCALL(__imp__KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel);
GUEST_FUNCTION_HOOK_STDCALL(__imp__HalRegisterShutdownNotification, HalRegisterShutdownNotification);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeInitializeTimerEx, KeInitializeTimerEx);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSetTimer, KeSetTimer);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeCancelTimer, KeCancelTimer);


GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlUnwind, RtlUnwind_);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XcHMAC, XcHMAC);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XeLoadSection, XeLoadSection);
GUEST_FUNCTION_HOOK_STDCALL(__imp__XeUnloadSection, XeUnloadSection);
GUEST_FUNCTION_HOOK_STDCALL(__imp__HalInitiateShutdown, HalInitiateShutdown);
GUEST_FUNCTION_HOOK_STDCALL(__imp__RtlEqualString, RtlEqualString);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmSetAddressProtect, MmSetAddressProtect);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmQueryAllocationSize, MmQueryAllocationSize);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmQueryAddressProtect, MmQueryAddressProtect);


GUEST_FUNCTION_HOOK_STDCALL(__imp__MmLockUnlockPhysicalPage, MmLockUnlockPhysicalPage);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmLockUnlockBufferPages, MmLockUnlockBufferPages);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress);
GUEST_FUNCTION_HOOK_FASTCALL(__imp__KfRaiseIrql, KfRaiseIrql);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeWaitForSingleObject, KeWaitForSingleObject);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSynchronizeExecution, KeSynchronizeExecution);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmClaimGpuInstanceMemory, MmClaimGpuInstanceMemory);
GUEST_FUNCTION_HOOK_STDCALL(__imp__MmAllocateContiguousMemoryEx, MmAllocateContiguousMemoryEx);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeStallExecutionProcessor, KeStallExecutionProcessor);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSetEvent, KeSetEvent);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeSaveFloatingPointState, KeSaveFloatingPointState);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeRestoreFloatingPointState, KeRestoreFloatingPointState);

GUEST_FUNCTION_HOOK_STDCALL(__imp__KeRemoveQueueDpc, KeRemoveQueueDpc);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeInitializeDpc, KeInitializeDpc);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeConnectInterrupt, KeConnectInterrupt);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeDisconnectInterrupt, KeDisconnectInterrupt);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeInitializeInterrupt, KeInitializeInterrupt);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeInsertQueueDpc, KeInsertQueueDpc);
GUEST_FUNCTION_HOOK_STDCALL(__imp__KeBugCheck, KeBugCheck);
GUEST_FUNCTION_HOOK_STDCALL(__imp__ExQueryPoolBlockSize, ExQueryPoolBlockSize);
GUEST_FUNCTION_HOOK_STDCALL(__imp__HalReadWritePCISpace, HalReadWritePCISpace);
GUEST_FUNCTION_HOOK_STDCALL(__imp__HalGetInterruptVector, HalGetInterruptVector);
GUEST_FUNCTION_HOOK_STDCALL(__imp__ExAllocatePoolWithTag, ExAllocatePoolWithTag);


GUEST_FUNCTION_HOOK_STDCALL(__imp__ExFreePool, ExFreePool);
GUEST_FUNCTION_HOOK_STDCALL(__imp__AvGetSavedDataAddress, AvGetSavedDataAddress);
GUEST_FUNCTION_HOOK_STDCALL(__imp__AvSetSavedDataAddress, AvSetSavedDataAddress);
GUEST_FUNCTION_HOOK_STDCALL(__imp__AvSetDisplayMode, AvSetDisplayMode);
GUEST_FUNCTION_HOOK_STDCALL(__imp__AvSendTVEncoderOption, AvSendTVEncoderOption);
#ifdef SHADOW_RECOMP_VA_LIST_IS_STRUCT
GUEST_FUNCTION_HOOK(__imp__DbgPrint, DbgPrintFormat);
#else
GUEST_FUNCTION_HOOK(__imp__DbgPrint, DbgPrintV);
#endif

GUEST_FUNCTION_HOOK(sub_2ADB04, XGetLanguage);

//XAutoPowerDownResetTimer
GUEST_FUNCTION_STUB(sub_2B17E8);
