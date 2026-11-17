#include "video.h"
#include "surface_resize.h"

#include "imgui/imgui_common.h"
#include "imgui/imgui_snapshot.h"
#include "imgui/imgui_font_builder.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "vertexdeclaration.h"
#include "vertexshader.h"
#include "pixelshader.h"
#include "nv2a.h"
#include "pushbuffer.h"
#include "swizzle.h"
#include "FixedFunction.h"

#include "shared_constants.h"
#include "staged_uploads.h"
#include "ubershaders/fixed_function_pixel_shader_variants.h"
#include "ubershaders/fixed_function_vertex_shader_variants.h"
#include "frame_pacing/frame_pacer.h"
#include <app.h>
#include <bc_diff.h>
#include <cpu/guest_thread.h>
#include <decompressor.h>
#include <input/input.h>
#include <plume_vulkan.h>

#ifdef __ANDROID__
#include "android_video.h"
#include <ui/touch_controls.h>
#endif

#include <res/bc_diff/button_bc_diff_gc.bin.h>
#include <res/bc_diff/button_bc_diff_ps2.bin.h>
#include <res/font/im_font_atlas.png.h>

#include <kernel/function.h>
#include <kernel/heap.h>
#include <ui/game_window.h>
#include <user/config.h>
#include "x86_context.h"
#include <patches/aspect_ratio_patches.h>
#include <sdl_listener.h>
#include <memory>
#include <shader/shader_cache.h>
#include <ui/imgui_utils.h>
#include <ui/installer_wizard.h>
#include <xxHashMap.h>
#include <rapidhash.h>
#include <shared_mutex>
#include <patches/multi_core_patches.h>
#include <user/achievement_manager.h>
#include <os/startup_timeline.h>
#include "pipeline.h"

const uint32_t GuestDevicePtrAddr = 0x3E0A18;
const uint32_t GuestDeviceMemAddr = 0x3E0A20;
const uint32_t PresentMemAddr = 0x55F718;
const uint32_t AutoDepthStencilFormat = 0x57DD68;
const uint32_t g_DeferredTextureStateAddr = 0x3E0580;
const uint32_t g_RenderStateAddr = 0x3E0780;
const uint32_t g_cameraAddr = 0x540C40;

#define DepthStencilFormat RenderFormat::D32_FLOAT

#ifdef SHADOW_RECOMP_D3D12
#include "FixedFunctionVertexShader.hlsl.dxil.h"
#include "FixedFunctionPixelShader.hlsl.dxil.h"
#include "shader/PassthroughVertexShader.hlsl.dxil.h"

#include "shader/copy_vs.hlsl.dxil.h"
#include "shader/copy_color_ps.hlsl.dxil.h"
#include "shader/copy_depth_ps.hlsl.dxil.h"
#include "shader/gamma_correction_ps.hlsl.dxil.h"

#include "shader/imgui_ps.hlsl.dxil.h"
#include "shader/imgui_vs.hlsl.dxil.h"

#include "shader/R2D_vs0.hlsl.dxil.h"
#include "shader/R2D_vs1.hlsl.dxil.h"
#endif

#include "FixedFunctionVertexShader.hlsl.spirv.h"
#include "shader/PassthroughVertexShader.hlsl.spirv.h"
#include "FixedFunctionPixelShader.hlsl.spirv.h"

#include "shader/copy_vs.hlsl.spirv.h"
#include "shader/copy_color_ps.hlsl.spirv.h"
#include "shader/copy_depth_ps.hlsl.spirv.h"
#include "shader/gamma_correction_ps.hlsl.spirv.h"

#include "shader/imgui_ps.hlsl.spirv.h"
#include "shader/imgui_vs.hlsl.spirv.h"

#include "shader/R2D_vs0.hlsl.spirv.h"
#include "shader/R2D_vs1.hlsl.spirv.h"
#include <os/logger.h>
#include <os/process.h>
#include <ui/fader.h>
#include <ui/message_window.h>
#include <ui/options_menu.h>
#include <ui/button_guide.h>
#include <deque>
#include <fstream>
#include <filesystem>
#include "sdl_listener.h"
#include "user/paths.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include "cache/pipeline_cache.h"
#include <patches/pause_patches.h>
#include <patches/fps_patches.h>
#include <ui/achievement_menu.h>
#include <ui/achievement_overlay.h>
#include <ui/multiplayer_hud.h>
#include "vertexbuffer.h"
#include "guest_convert.h"
#include "render_commands.h"
#include <gpu/resources.h>

namespace plume
{
#ifdef SHADOW_RECOMP_D3D12
	extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
	extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

static_assert(sizeof(uint32_t) == 4, "expect 4-byte uint");
static_assert(TexSlotMax == SharedTextureSlotCount);

struct TextureCombinerState {
	uint32_t color_op = 1;
	uint32_t color_arg0 = D3DTA_TEXTURE;
	uint32_t color_arg1 = D3DTA_TEXTURE;
	uint32_t color_arg2 = D3DTA_CURRENT;
	uint32_t alpha_op = 1;
	uint32_t alpha_arg0 = D3DTA_TEXTURE;
	uint32_t alpha_arg1 = D3DTA_TEXTURE;
	uint32_t alpha_arg2 = D3DTA_CURRENT;
	uint32_t texcoord_index = 0;
};

struct RenderViewport_r
{
	RenderViewport vp;
	bool isFixed = true;
	bool flushed = false;
};

static GuestSurface* g_renderTarget;
static GuestSurface* g_depthStencil;
static GuestSurface* g_next_RenderTarget; //because of how heavy and fast the game swap the render target, our queue system sometimes faces a race issue, this second pointer copy the current RT earlier as a workaround
static RenderFramebuffer* g_framebuffer;
static RenderViewport_r g_renderviewport = { { 0.0f, 0.0f, 640.0f, 480.0f }, true, false };
static RenderViewport g_gameViewport = { 0.0f, 0.0f, 640.0f, 480.0f };

static PipelineState g_pipelineState;
static int32_t g_depthBias;
static float g_slopeScaledDepthBias;
int32_t g_RenderUpscaleFactor = 1;

GuestSurface* GetRenderTarget()
{
	return g_renderTarget;
}


uint32_t g_VertexShader_Handle = 0;
VertexShaderMode g_Xbox_VertexShaderMode = VertexShaderMode::FixedFunction;
static float g_ZScale = 1.0f;

//constants are variable send to the GPU
static uint32_t g_vertexShaderConstants[HOST_D3DVS_CONSTREG_FULL * 4];
static uint32_t g_vertexShaderConstantsFixed[HOST_D3DVS_CONSTREG_FULL * 4];
extern uint32_t vsh_constants[HOST_D3DVS_CONSTREG_FULL][4];

static uint32_t g_pixelShaderConstants[PSH_XBOX_CONSTANT_MAX * 4];
static SharedConstants g_sharedConstants; //TextureStage operation data, indice etc. Used with texture descriptor
static bool g_fixedFunctionPixelShaderStateDirty = true;
// Last variant key sent to the render thread via SetVertexShaderVariant.
static ShaderVariantKey g_lastSentVertexShaderVariant = GenericShaderVariant;

uint32_t g_PixelShader;


std::array<D3DMATRIX, X_D3DTS_MAX> g_Transforms;

static GuestTexture* g_textures[TexSlotMax];
static RenderSamplerDesc g_samplerDescs[TexSlotMax];
static TextureCombinerState g_sharedSamplerDescs[TexSlotMax];
GuestTexture* g_texturesPSDef[4];
static bool g_scissorTestEnable = false;
static RenderRect g_scissorRect;

static RenderVertexBufferView g_vertexBufferViews[16];
static RenderInputSlot g_inputSlots[16];
static RenderIndexBufferView g_indexBufferView({}, 0, RenderFormat::R16_UINT);

int32_t* g_texture_stage_states = nullptr;
int32_t g_dirty_texture_stage_states[4 * X_D3DTSS_MAX]; //128 because 4 stages * 32 texture stage state possible

struct DirtyStates
{
	bool renderTargetAndDepthStencil;
	bool viewport;
	bool pipelineState;
	bool depthBias;
	bool sharedConstants;
	bool scissorRect;
	bool vertexShaderConstants;
	uint8_t vertexStreamFirst;
	uint8_t vertexStreamLast;
	bool indices;
	bool pixelShaderConstants;
	bool textureStageState;
	bool vertexBufferLock;

	DirtyStates(bool value)
		: renderTargetAndDepthStencil(value)
		, viewport(value)
		, pipelineState(value)
		, depthBias(value)
		, sharedConstants(value)
		, scissorRect(value)
		, vertexShaderConstants(value)
		, vertexStreamFirst(value ? 0 : 255)
		, vertexStreamLast(value ? 15 : 0)
		, indices(value)
		, pixelShaderConstants(value)
		, vertexBufferLock(value)
	{
	}
};

static DirtyStates g_dirtyStates(true);

template<typename T>
static void SetDirtyValue(bool& dirtyState, T& dest, const T& src)
{
	if (dest != src)
	{
		dest = src;
		dirtyState = true;
	}
}

static constexpr size_t PROFILER_VALUE_COUNT = 256;
static size_t g_profilerValueIndex;

struct Profiler
{
	std::atomic<double> value;
	double values[PROFILER_VALUE_COUNT];
	std::chrono::steady_clock::time_point start;
	bool started = false;
	bool active = false; // true between Begin and End; used to attribute queue waits

	void Begin()
	{
		start = std::chrono::steady_clock::now();
		started = true;
		active = true;
	}

	void End()
	{
		if (!started)
			return;
		value = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		active = false;
	}

	void Set(double v)
	{
		value = v;
	}

	void Reset()
	{
		End();
		Begin();
	}

	double UpdateAndReturnAverage()
	{
		values[g_profilerValueIndex] = value;
		return std::accumulate(values, values + PROFILER_VALUE_COUNT, 0.0) / PROFILER_VALUE_COUNT;
	}
};

struct FrameAccumulator
{
	std::atomic<double> value{ 0.0 };
	double values[PROFILER_VALUE_COUNT];
	double accum = 0.0;

	void Add(double ms) { accum += ms; }
	void SnapshotAndReset()
	{
		value = accum;
		accum = 0.0;
	}
	double UpdateAndReturnAverage()
	{
		values[g_profilerValueIndex] = value.load();
		return std::accumulate(values, values + PROFILER_VALUE_COUNT, 0.0) / PROFILER_VALUE_COUNT;
	}
};

static std::atomic<bool> g_detailedProfilerEnabled{ false };

// RAII scope timer that adds elapsed time to a FrameAccumulator.
struct RtScopeTimer
{
	std::chrono::steady_clock::time_point start;
	FrameAccumulator& accum;
	bool enabled;
	RtScopeTimer(FrameAccumulator& a) : accum(a), enabled(g_detailedProfilerEnabled.load(std::memory_order_relaxed))
	{
		if (enabled)
			start = std::chrono::steady_clock::now();
	}
	~RtScopeTimer()
	{
		if (enabled)
			accum.Add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}
};

static double g_applicationValues[PROFILER_VALUE_COUNT];
static double g_frameIntervalMs = 0.0;
static Profiler g_gpuFrameProfiler;
static Profiler g_presentProfiler;
static Profiler g_updateDirectorProfiler;
static Profiler g_renderDirectorProfiler;
static Profiler g_frameFenceProfiler;
static Profiler g_presentWaitProfiler;
static Profiler g_swapChainAcquireProfiler;
static Profiler g_frameEndWaitProfiler;

// --- Per-frame command counters (reset at EndFrame) ---
// Render thread only, so plain integers: a lock-prefixed add per command is measurable.
static uint32_t g_drawCallCount = 0;      // DrawPrimitive + DrawIndexedPrimitive + DrawPrimitiveUP
static uint32_t g_stateChangeCount = 0;   // All non-draw, non-frame-boundary commands
static uint32_t g_totalCommandCount = 0;  // Everything
static uint32_t g_drawCallCountSnapshot = 0;
static uint32_t g_stateChangeCountSnapshot = 0;
static uint32_t g_totalCommandCountSnapshot = 0;
static uint32_t g_psoCacheHits = 0;
static uint32_t g_psoCacheMisses = 0;
static uint32_t g_psoCacheHitsSnapshot = 0;
static uint32_t g_psoCacheMissesSnapshot = 0;

#ifdef __ANDROID__
// What the log is told about a frame once in a while (AndroidVideo::CountFrame): how many
// draws were left out because the driver had refused their pipeline, and how often the
// frame buffer changed. Every change ends a render pass, which on the tile-based GPUs of
// phones means writing all its targets out and reading the next ones in.
static uint32_t g_skippedDrawCount = 0;
static uint32_t g_framebufferChangeCount = 0;

// Whether the pipeline of the current state is one the driver created. plume hands out an
// object for a pipeline the driver refused too; bound, it is a null handle to the driver,
// which some drivers crash on and others answer by drawing with whatever was bound before.
static bool g_pipelineUsable = true;
#endif

// --- Render-thread per-frame breakdown (cumulative; snapshot at frame end) ---
static FrameAccumulator g_rtPsoWorkAccum;
static FrameAccumulator g_rtFlushWorkAccum;
static FrameAccumulator g_rtDrawCallAccum;
static FrameAccumulator g_rtQueueWaitAccum;
// Queue idle while the render-thread frame bracket is open (inside Render-Thread Work).
static FrameAccumulator g_rtQueueWaitInFrameAccum;
static FrameAccumulator g_gtDrawRecordingAccum;
static FrameAccumulator g_gtStateFlushAccum;
static FrameAccumulator g_rtOtherCmdsAccum;
static FrameAccumulator g_rtDrawHandlerAccum;
static FrameAccumulator g_rtBeginCmdListWorkAccum;

static bool g_profilerVisible;
static bool g_profilerWasToggled;

#ifdef SHADOW_RECOMP_D3D12
static bool g_vulkan = false;
#else
static constexpr bool g_vulkan = true;
#endif

static bool g_hardwareResolve = true;
static bool g_hardwareDepthResolve = true;

static std::unique_ptr<RenderInterface> g_interface;
static std::unique_ptr<RenderDevice> g_device;
static RenderDeviceCapabilities g_capabilities;


RenderDevice* Video::GetRenderDevice()
{
	return g_device.get();
}

static constexpr size_t NUM_FRAMES = 2;
static constexpr size_t NUM_QUERIES = 2;

uint32_t g_frame = 0;
static uint32_t g_nextFrame = 1;

static std::unique_ptr<RenderCommandQueue> g_queue;
static std::unique_ptr<RenderCommandList> g_commandLists[NUM_FRAMES];
static std::unique_ptr<RenderCommandFence> g_commandFences[NUM_FRAMES];
static std::unique_ptr<RenderQueryPool> g_queryPools[NUM_FRAMES];
static bool g_commandListStates[NUM_FRAMES];

static Mutex g_copyMutex;
static std::unique_ptr<RenderCommandQueue> g_copyQueue;
static std::unique_ptr<RenderCommandList> g_copyCommandList;
static std::unique_ptr<RenderCommandFence> g_copyCommandFence;

static std::unique_ptr<RenderSwapChain> g_swapChain;
static bool g_swapChainValid;

static constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;

#ifdef __ANDROID__
// Android window surfaces only offer RGBA-ordered formats, and plume refuses to create a
// swap chain in a format the surface does not list. Everything the game renders stays
// BGRA: its render targets are, the precompiled pipeline cache was recorded with BGRA
// targets, and the back buffer is copied to and from BGRA textures. So on Android the
// frame always goes to the intermediary back buffer and only the presentation pass,
// which samples it, writes to the RGBA swap chain image.
static constexpr RenderFormat SWAPCHAIN_FORMAT = RenderFormat::R8G8B8A8_UNORM;
#else
static constexpr RenderFormat SWAPCHAIN_FORMAT = BACKBUFFER_FORMAT;
#endif

// Sewer sez: Settings the FF pixel shader reads on every pixel (Vulkan only).
//            We push these directly, rather than having GPU do mem read for faster
//            perf in the pixel shader. Hella ugly, I hate it, but it works.
//
//			  We should try this in D3D12 too; unify the memory structure,
// 			  having two different layouts is rather ass. (<= not the cool subtitles)
//
// See XShaderRecomp commit d3e1d431 for why.
// Must match the end of PushConstants in common_fixed_ps.hlsli.
struct PixelPushConstants
{
	uint32_t texture3DIndex;
	uint32_t textureCubeIndex;
	uint32_t texture2DIndices[4];
	uint32_t samplerIndices[4];
	float texCoordMin[2][2]; // Stages 0 and 1.
	float texCoordMax[2][2];
	uint32_t fogStart;
	uint32_t fogEnd;
	float fogColor[3];
	uint32_t alphaTestEnable;
	uint32_t alphaFunc;
	float alphaRef;
};

static constexpr uint32_t PIXEL_PUSH_CONSTANTS_OFFSET = 3 * sizeof(uint64_t);
static constexpr uint32_t PUSH_CONSTANTS_SIZE = PIXEL_PUSH_CONSTANTS_OFFSET + sizeof(PixelPushConstants);
static_assert(PUSH_CONSTANTS_SIZE == 128, "Every Vulkan device supports at least 128 bytes of push constants");

static std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[NUM_FRAMES];
static std::unique_ptr<RenderCommandSemaphore> g_renderSemaphores[NUM_FRAMES];
static uint32_t g_backBufferIndex;
static std::unique_ptr<GuestSurface> g_backBufferHolder;
static GuestSurface* g_backBuffer;
static GuestSurface* g_defaultStencil;

static std::unique_ptr<RenderTexture> g_intermediaryBackBufferTexture;
static uint32_t g_intermediaryBackBufferTextureWidth;
static uint32_t g_intermediaryBackBufferTextureHeight;
static uint32_t g_intermediaryBackBufferTextureDescriptorIndex;

struct std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
struct std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;

static xxHashMap<std::pair<uint32_t, std::unique_ptr<RenderSampler>>> g_samplerStates;

static xxHashMap<GuestBuffer*> g_indexBufferCache;

struct UploadBuffer
{
	static constexpr size_t SIZE = 16 * 1024 * 1024;

	std::unique_ptr<RenderBuffer> buffer;
	uint8_t* memory = nullptr;
	uint64_t deviceAddress = 0;
};

// Render thread only!!
struct UploadAllocator
{
	std::vector<UploadBuffer> buffers;
	uint32_t index = 0;
	uint32_t offset = 0;

	UploadAllocation allocate(uint32_t size, uint32_t alignment)
	{
		assert(size <= UploadBuffer::SIZE);

		offset = (offset + alignment - 1) & ~(alignment - 1);

		if (offset + size > UploadBuffer::SIZE)
		{
			++index;
			offset = 0;
		}

		if (buffers.size() <= index)
			buffers.resize(index + 1);

		auto& buffer = buffers[index];
		if (buffer.buffer == nullptr)
		{
			buffer.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(UploadBuffer::SIZE, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::DEVICE_ADDRESSABLE));
			buffer.memory = reinterpret_cast<uint8_t*>(buffer.buffer->map());

			buffer.deviceAddress = buffer.buffer->getDeviceAddress();
		}


		auto ref = buffer.buffer->at(offset);
		offset += size;

		return { ref.ref, ref.offset, buffer.memory + ref.offset, buffer.deviceAddress + ref.offset };
	}

	template<typename T>
	UploadAllocation allocate(const T* memory, uint32_t size, uint32_t alignment)
	{
		auto result = allocate(size, alignment);
		memcpy(result.memory, memory, size);
		return result;
	}

	void reset()
	{
		index = 0;
		offset = 0;
	}
};

static UploadAllocator g_uploadAllocators[NUM_FRAMES];

struct IntermediaryUploadAllocator
{
	static constexpr size_t SIZE = 16 * 1024 * 1024;

	// Guarded state: allocate() is called from guest draw threads while
	// reset() is called from the Present thread, and guests can draw
	// concurrently with each other.
	std::mutex mutex;
	std::vector<std::unique_ptr<uint8_t[]>> buffers;
	uint32_t index = 0;
	uint32_t offset = 0;

	uint8_t* allocate(uint32_t size)
	{
		assert(size <= SIZE);

		uint8_t* result;
		{
			std::lock_guard lock(mutex);

			if (offset + size > SIZE)
			{
				++index;
				offset = 0;
			}

			if (buffers.size() <= index)
				buffers.resize(index + 1);

			auto& buffer = buffers[index];
			if (buffer == nullptr)
				buffer = std::make_unique_for_overwrite<uint8_t[]>(SIZE);

			result = buffer.get() + offset;
			offset += ((size + 0xF) & ~0xF);
		}

		// After this line: caller owns [result, result + size) exclusively,
		// memcpy below races with no one.
		return result;
	}

	uint8_t* allocate(const void* memory, uint32_t size)
	{
		auto result = allocate(size);
		memcpy(result, memory, size);
		return result;
	}

	void reset()
	{
		std::lock_guard lock(mutex);
		index = 0;
		offset = 0;
	}
};

static std::atomic<uint32_t> g_gameFrame{ 0 };

static IntermediaryUploadAllocator g_intermediaryUploadAllocators[3];
static StagedBufferUploadAllocator g_stagedBufferUploads[4];

static IntermediaryUploadAllocator& GameFrameUploads()
{
	return g_intermediaryUploadAllocators[g_gameFrame.load(std::memory_order_relaxed) % std::size(g_intermediaryUploadAllocators)];
}

static StagedBufferUploadAllocator& GameFrameStagedUploads()
{
	return g_stagedBufferUploads[g_gameFrame.load(std::memory_order_relaxed) % std::size(g_stagedBufferUploads)];
}

// Allocates from a per-thread block.
static uint8_t* AllocateDrawData(uint32_t size)
{
	static constexpr uint32_t BLOCK_SIZE = 64 * 1024;

	struct Block
	{
		uint8_t* memory = nullptr;
		uint32_t offset = BLOCK_SIZE;
		uint32_t gameFrame = 0;
	};

	thread_local Block block; // <= PER THREAD. (Don't miss this!! Codo-reader-chan!)

	// Skip block for huge allocations. We want to group the smol.
	const uint32_t alignedSize = (size + 0xF) & ~0xF;
	if (alignedSize > BLOCK_SIZE / 4)
		return GameFrameUploads().allocate(size);

	// Make new block if needed.
	// On gem frem or if it's too big.
	const uint32_t gameFrame = g_gameFrame.load(std::memory_order_relaxed);
	if (block.gameFrame != gameFrame || block.offset + alignedSize > BLOCK_SIZE)
	{
		block.memory = GameFrameUploads().allocate(BLOCK_SIZE);
		block.offset = 0;
		block.gameFrame = gameFrame;
	}

	// Bump allocate.
	uint8_t* result = block.memory + block.offset;
	block.offset += alignedSize;
	return result;
}

static uint8_t* AllocateDrawData(const void* memory, uint32_t size)
{
	uint8_t* result = AllocateDrawData(size);
	memcpy(result, memory, size);
	return result;
}

static std::vector<GuestResource*> g_tempResources[NUM_FRAMES];
static std::vector<std::unique_ptr<RenderBuffer>> g_tempBuffers[NUM_FRAMES];

// RTs that are already zeroed this frame (see ProcSetRenderTarget)
static ankerl::unordered_dense::set<const RenderTexture*> g_clearedRenderTargetsThisFrame;

// Sewer note: Edgy the hedgy draws over the whole screen in all tested cases:
// - Event Mode
// - Stage without geo
// - Credits
//
// This may not apply to other games, and we may need clears.
// So if you're copying this to another game, consider changing this.
#ifndef CLEAR_BACKBUFFER
#define CLEAR_BACKBUFFER 0
#endif

float GetRenderTargetHeight()
{
	if (g_next_RenderTarget != nullptr && g_next_RenderTarget->height)
		return g_next_RenderTarget->height;

	return 0.0f;
}

bool isCapabilityTriangleFan()
{
	return g_capabilities.triangleFan;
}

bool Video::is_vulkan()
{
	return g_vulkan;
}

uint8_t* AllocIntermediaryUploadMemoryWrapper(const void* memory, uint32_t size)
{
	return AllocateDrawData(memory, size);
}

std::unique_ptr<RenderBuffer> PlumeCreateBufferWrapper(const uint32_t size)
{
	return g_device->createBuffer(RenderBufferDesc::UploadBuffer(size));
}

std::unique_ptr<RenderTexture> PlumeCreateTextureWrapper(const RenderTextureDesc& desc)
{
	return g_device->createTexture(desc);
}

void PlumeSetTextureWrapper(uint32_t descriptorIndex, const RenderTexture* texture, const RenderTextureLayout textureLayout, const RenderTextureView* textureView)
{
	g_textureDescriptorSet->setTexture(descriptorIndex, texture, textureLayout, textureView);
}

static std::atomic<bool> g_readyForCommands;
static std::atomic<bool> g_commandListOpen{ false };

// The main game loop and the loading thread both draw and present, this can happen at the same
// time and the game has no lock for it (OG Xbox monocore moment).
// To prevent a crash, we can use a shared mutex to lock the present and draw threads.
static std::shared_mutex g_presentMutex;

void Guard_Rendering()
{
	g_readyForCommands.wait(false);
}


static ankerl::unordered_dense::map<RenderTexture*, RenderTextureLayout> g_barrierMap;

const char* GetLayoutName(RenderTextureLayout layout)
{
	switch (layout)
	{
	default:
	case RenderTextureLayout::UNKNOWN:
		return "Unknown";
	case RenderTextureLayout::GENERAL:
		return "General";
	case RenderTextureLayout::SHADER_READ:
		return "Shader Read";
	case RenderTextureLayout::COLOR_WRITE:
		return "Color Write";
	case RenderTextureLayout::DEPTH_WRITE:
		return "Depth Write";
	case RenderTextureLayout::PRESENT:
		return "Present";
	}
}

// GetSurfaceLevel makes surfaces that uses an existing parent texture, so textureHolder is null.
// If parent dies, or get resized, texture will point at freed memory without being null,
// meaning we will barrier a dead resource. 
// we can avoid that by getting the parent texture instead.
static bool IsStaleAliasSurface(GuestBaseTexture* texture)
{
	if (texture->textureHolder != nullptr)
		return false;

	if (texture->type != ResourceType::RenderTarget && texture->type != ResourceType::DepthStencil)
		return false;

	const auto surface = static_cast<GuestSurface*>(texture);
	const auto parent = surface->parentTexture;

	return parent == nullptr || parent->texture != surface->texture
		|| parent->textureGeneration != surface->textureGeneration;
}

static void AddBarrier(GuestBaseTexture* texture, RenderTextureLayout layout)
{

	if (texture == nullptr || texture->texture == nullptr)
		return;

	if (IsStaleAliasSurface(texture))
	{
		// Texture is gone, null it properly to avoid playing with dead memory.
		// Would be nice to not have to do hacky fixes like that, but this is good enough for now.
		texture->texture = nullptr;
		return;
	}

	//printf("AddBarrier called, texture %x, current layout %d (%s) new layout requeted %d (%s) \n", texture->texture, texture->layout, GetLayoutName(texture->layout), layout, GetLayoutName(layout));
	if (texture->layout != layout)
	{
		g_barrierMap[texture->texture] = layout;
		texture->layout = layout;
	}

}

static std::vector<RenderTextureBarrier> g_barriers;

static void FlushBarriers()
{
	if (!g_barrierMap.empty())
	{
		for (auto& [texture, layout] : g_barrierMap)
		{
			//printf("FlushBarriers: Texture %x to layout %d (%s), count: %d\n", texture, layout, GetLayoutName(layout), ++countFail);
			g_barriers.emplace_back(texture, layout);
		}

		g_commandLists[g_frame]->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, g_barriers);

		g_barrierMap.clear();
		g_barriers.clear();
	}
}

// Ensure g_barrierMap doesn't hold dead pointer for BeginCommandList and FlushBarriers

static void DiscardStagedBarrier(GuestBaseTexture* texture)
{
	if (texture == nullptr)
		return;

	if (texture->texture != nullptr)
		g_barrierMap.erase(texture->texture);

	if (texture->textureHolder != nullptr)
		g_barrierMap.erase(texture->textureHolder.get());
}

std::unique_ptr<uint8_t[]> g_shaderCache;
std::unique_ptr<uint8_t[]> g_buttonBcDiff_GC;
std::unique_ptr<uint8_t[]> g_buttonBcDiff_PS2;

static void LoadEmbeddedResources()
{
	if (g_vulkan)
	{
		g_shaderCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
		ZSTD_decompress(g_shaderCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache, g_spirvCacheCompressedSize);
	}
#ifdef SHADOW_RECOMP_D3D12
	else
	{
		g_shaderCache = std::make_unique<uint8_t[]>(g_dxilCacheDecompressedSize);
		ZSTD_decompress(g_shaderCache.get(), g_dxilCacheDecompressedSize, g_compressedDxilCache, g_dxilCacheCompressedSize);
	}
#endif

	g_buttonBcDiff_GC = decompressZstd(g_button_bc_diff_gc, g_button_bc_diff_gc_uncompressed_size);
	g_buttonBcDiff_PS2 = decompressZstd(g_button_bc_diff_ps2, g_button_bc_diff_ps2_uncompressed_size);
}

constexpr float PIPELINE_PRECOMPILE_SHADER_STAGE_END = 0.55f;

static void PrecompileShaders(const std::function<void(float)>& progress = {})
{
	auto totalStart = std::chrono::steady_clock::now();
	for (size_t i = 0; i < g_shaderCacheEntryCount; i++)
	{
		auto& entry = g_shaderCacheEntries[i];
		GuestShader* guestShader = g_userHeap.AllocPhysical<GuestShader>(ResourceType::VertexShader);
		guestShader->shaderCacheEntry = &entry;
		entry.guestShader = guestShader;

		if (g_vulkan)
		{
			auto compressedSpirvData = g_shaderCache.get() + entry.spirvOffset;
			std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(compressedSpirvData, entry.spirvSize));
			bool result = smolv::Decode(compressedSpirvData, entry.spirvSize, decoded.data(), decoded.size());
			assert(result);
			guestShader->shader = g_device->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);
		}
#ifdef SHADOW_RECOMP_D3D12
		else
		{
			guestShader->shader = g_device->createShader(
				g_shaderCache.get() + entry.dxilOffset, entry.dxilSize, "main", RenderShaderFormat::DXIL);
		}
#endif
		if (progress)
		{
			progress(PIPELINE_PRECOMPILE_SHADER_STAGE_END * (float)(i + 1) / (float)g_shaderCacheEntryCount);
		}
	}
	auto totalElapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart).count();
	LOGFN("PrecompileShaders: {} shaders in {:.2f} ms", g_shaderCacheEntryCount, totalElapsed);
}

static std::unique_ptr<RenderPipeline> g_gammaCorrectionPipeline;

enum
{
	TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D,
	TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D,
	TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE,
	TEXTURE_DESCRIPTOR_NULL_COUNT
};

struct TextureDescriptorAllocator
{
	Mutex mutex;
	uint32_t capacity = TEXTURE_DESCRIPTOR_NULL_COUNT;
	std::vector<uint32_t> freed;

	uint32_t allocate()
	{
		std::lock_guard lock(mutex);

		uint32_t value;
		if (!freed.empty())
		{
			value = freed.back();
			freed.pop_back();
		}
		else
		{
			value = capacity;
			++capacity;
		}

		return value;
	}

	void free(uint32_t value)
	{
		assert(value != NULL);
		std::lock_guard lock(mutex);
		freed.push_back(value);
	}
};

static std::unique_ptr<RenderTexture> g_blankTextures[TEXTURE_DESCRIPTOR_NULL_COUNT];
static std::unique_ptr<RenderTextureView> g_blankTextureViews[TEXTURE_DESCRIPTOR_NULL_COUNT];

static TextureDescriptorAllocator g_textureDescriptorAllocator;

uint32_t AllocateTextureDescriptor()
{
	return g_textureDescriptorAllocator.allocate();
}

void FreeTextureDescriptor(const uint32_t descriptorIndex)
{
	g_textureDescriptorAllocator.free(descriptorIndex);
}

std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
static xxHashMap<std::unique_ptr<RenderPipeline>> g_pipelines;

// Checks incase plume changes something
static_assert(uint32_t(VertexShaderMode::ShaderProgram) == 2);
static_assert(uint32_t(RenderBlend::INV_SRC1_ALPHA) == 17);
static_assert(uint32_t(RenderCullMode::BACK) == 3);
static_assert(uint32_t(RenderComparisonFunction::ALWAYS) == 8);
static_assert(uint32_t(RenderStencilOp::DECREMENT_AND_WRAP) == 8);
static_assert(uint32_t(RenderBlendOperation::MAX) == 5);
static_assert(uint32_t(RenderPrimitiveTopology::TRIANGLE_FAN) == 6);
static_assert(uint32_t(RenderFormat::MAX) == 75);


static Mutex g_pipelineCacheMutex;
static std::atomic<bool> g_pipelineCacheShutdownDone{ false };
static ankerl::unordered_dense::map<uint64_t, PipelineCacheEntry> g_pipelineCacheEntries;
static bool g_pipelineCacheHasUnsavedEntries = false; // Guarded by g_pipelineCacheMutex.
static bool TryCompileFromCacheEntry(const PipelineCacheEntry& entry);


static std::filesystem::path GetCachePath()
{
	return GetUserPath() / "cache";
}

static std::filesystem::path GetPipelineCachePath()
{
	return GetCachePath() / "pipeline_cache.bin";
}

static void RecordPipelineState(const PipelineState& pipelineState)
{
	PipelineCacheEntry entry;
	if (!BuildPipelineCacheEntry(pipelineState, entry))
		return;

	XXH64_hash_t hash = ComputePipelineEntryHash(entry);
	entry.pipelineHash = hash;

	std::lock_guard lock(g_pipelineCacheMutex);
	if (!g_pipelineCacheEntries.contains(hash))
	{
		g_pipelineCacheEntries.emplace(hash, std::move(entry));
		g_pipelineCacheHasUnsavedEntries = true;
	}
}

// onlyIfChanged: write only if the game has used a pipeline state that the file does not
// have yet. For callers that save more than once per run.
static void SavePipelineCache(bool onlyIfChanged = false)
{
	std::lock_guard lock(g_pipelineCacheMutex);
	if (g_pipelineCacheEntries.empty())
		return;

	if (onlyIfChanged && !g_pipelineCacheHasUnsavedEntries)
		return;

	std::vector<PipelineCacheEntry> entries;
	entries.reserve(g_pipelineCacheEntries.size());
	for (auto& [hash, entry] : g_pipelineCacheEntries)
	{
		entries.push_back(entry);
	}

	size_t rawSize = entries.size() * sizeof(PipelineCacheEntry);
	std::vector<uint8_t> compressed(ZSTD_compressBound(rawSize));
	size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), entries.data(), rawSize, ZSTD_CLEVEL_DEFAULT);
	if (ZSTD_isError(compressedSize))
	{
		LOGFN("PipelineCache: ZSTD failed ({})", ZSTD_getErrorName(compressedSize));
		return;
	}

	auto path = GetPipelineCachePath();
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	// Written under another name and renamed once complete: a process that is killed while
	// it writes (the usual end of an Android process) must not leave half a file in the
	// place of a whole one.
	auto temporaryPath = path;
	temporaryPath += ".tmp";

	std::ofstream file(temporaryPath, std::ios::binary);
	if (!file.is_open())
	{
		LOGFN("PipelineCache: Failed to open {} for writing", temporaryPath.string());
		return;
	}

	uint32_t entryCount = (uint32_t)entries.size();

	file.write(reinterpret_cast<const char*>(&PIPELINE_CACHE_MAGIC), sizeof(PIPELINE_CACHE_MAGIC));
	file.write(reinterpret_cast<const char*>(&PIPELINE_CACHE_VERSION), sizeof(PIPELINE_CACHE_VERSION));
	file.write(reinterpret_cast<const char*>(&entryCount), sizeof(entryCount));
	file.write(reinterpret_cast<const char*>(compressed.data()), compressedSize);
	file.close();

	if (file.fail())
	{
		LOGFN("PipelineCache: Failed to write {}", temporaryPath.string());
		std::filesystem::remove(temporaryPath, ec);
		return;
	}

	std::filesystem::rename(temporaryPath, path, ec);
	if (ec)
	{
		LOGFN("PipelineCache: Failed to replace {} ({})", path.string(), ec.message());
		std::filesystem::remove(temporaryPath, ec);
		return;
	}

	g_pipelineCacheHasUnsavedEntries = false;

	LOGFN("PipelineCache: Saved {} entries", entryCount);
}

static void PrecompileUserPipelines()
{
	auto path = GetPipelineCachePath();
	if (!std::filesystem::exists(path))
	{
		LOGFN("PipelineCache: No cache file found at {}", path.string());
		return;
	}

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOGFN("PipelineCache: Failed to open {}", path.string());
		return;
	}

	std::error_code ec;
	auto fileSize = std::filesystem::file_size(path, ec);
	if (ec)
	{
		LOGFN("PipelineCache: Failed to get file size for {}", path.string());
		return;
	}

	constexpr size_t headerSize = sizeof(PIPELINE_CACHE_MAGIC) + sizeof(PIPELINE_CACHE_VERSION) + sizeof(uint32_t);
	if (fileSize < headerSize)
	{
		LOGFN("PipelineCache: File too small for header ({} bytes)", fileSize);
		return;
	}

	uint32_t magic, version, entryCount;
	file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	file.read(reinterpret_cast<char*>(&version), sizeof(version));
	file.read(reinterpret_cast<char*>(&entryCount), sizeof(entryCount));

	if (magic != PIPELINE_CACHE_MAGIC || version != PIPELINE_CACHE_VERSION)
	{
		LOGFN("PipelineCache: Invalid cache file (magic={:X} version={})", magic, version);
		return;
	}

	std::vector<uint8_t> compressed(fileSize - headerSize);
	file.read(reinterpret_cast<char*>(compressed.data()), compressed.size());

	size_t rawSize = size_t(entryCount) * sizeof(PipelineCacheEntry);
	if (entryCount == 0 || ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != rawSize)
	{
		LOGFN("PipelineCache: corrupt file ({} entries)", entryCount);
		return;
	}

	std::vector<PipelineCacheEntry> entries(entryCount);
	size_t decompressedSize = ZSTD_decompress(entries.data(), rawSize, compressed.data(), compressed.size());
	if (ZSTD_isError(decompressedSize) || decompressedSize != rawSize)
	{
		LOGFN("PipelineCache: ZSTD failed ({})", ZSTD_isError(decompressedSize) ? ZSTD_getErrorName(decompressedSize) : "unknown error");
		return;
	}

	LOGFN("PipelineCache: Loading {} entries...", entryCount);

	auto totalStart = std::chrono::steady_clock::now();
	uint32_t compiled = 0;
	uint32_t skipped = 0;
	uint32_t dropped = 0;

	for (auto& entry : entries)
	{
		if (!IsPipelineCacheEntryInRange(entry))
		{
			dropped++;
			continue;
		}

		entry.pipelineHash = ComputePipelineEntryHash(entry);

		if (TryCompileFromCacheEntry(entry))
			compiled++;
		else
		{
			EnqueueDeferredPipeline(entry);
			skipped++;
		}

		{
			std::lock_guard lock(g_pipelineCacheMutex);
			g_pipelineCacheEntries.emplace(entry.pipelineHash, entry);
		}
	}

	auto totalElapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart).count();
	LOGFN("PipelineCache: {} {} pipelines ({} skipped, {} dropped) in {:.2f} ms",
		PrecompilesPipelinesInBackground() ? "Queued" : "Precompiled", compiled, skipped, dropped, totalElapsed);
}

static bool TryCompileFromCacheEntry(const PipelineCacheEntry& entry)
{
	GuestShader* vertexShader = nullptr;
	GuestShader* pixelShader = nullptr;

	if (entry.vertexShaderMode == VertexShaderMode::FixedFunction ||
		entry.vertexShaderMode == VertexShaderMode::Passthrough)
	{
		vertexShader = nullptr;
	}
	else if (entry.vertexShaderHash != 0)
	{
		auto findVS = FindShaderCacheEntry(entry.vertexShaderHash);
		if (findVS != nullptr && findVS->guestShader != nullptr)
			vertexShader = findVS->guestShader;
		else
			return false;
	}
	else
		return false;

	if (entry.isFixedFunctionPS)
	{
		pixelShader = nullptr;
	}
	else if (entry.pixelShaderHash != 0)
	{
		auto findPS = FindShaderCacheEntry(entry.pixelShaderHash);
		if (findPS != nullptr && findPS->guestShader != nullptr)
			pixelShader = findPS->guestShader;
		else
			return false;
	}

	GuestVertexDeclaration* vertexDecl = FindVertexDeclarationByHash(entry.vertexDeclHash);
	if (vertexDecl == nullptr)
	{
		if (entry.vertexElementCount > 0 && entry.vertexElementCount <= MAX_VERTEX_ELEMENTS)
			vertexDecl = CreateVertexDeclarationWithoutAddRef(const_cast<D3DVERTEXELEMENT9*>(entry.vertexElements), entry.vertexShaderMode);
		if (vertexDecl == nullptr)
			return false;
	}

	PipelineState pipelineState;
	pipelineState.vertexShader = vertexShader;
	pipelineState.pixelShader = pixelShader;
	pipelineState.vertexDeclaration = vertexDecl;
	pipelineState.isFixedFunctionPS = entry.isFixedFunctionPS;
	ApplyCacheEntryVariants(entry, pipelineState);
	pipelineState.vertexShaderType = entry.vertexShaderMode;
	pipelineState.zEnable = entry.zEnable;
	pipelineState.zWriteEnable = entry.zWriteEnable;
	pipelineState.stencilEnable = entry.stencilEnable;
	pipelineState.srcBlend = entry.srcBlend;
	pipelineState.destBlend = entry.destBlend;
	pipelineState.cullMode = entry.cullMode;
	pipelineState.zFunc = entry.zFunc;
	pipelineState.stencilFunc = entry.stencilFunc;
	pipelineState.stencilFail = entry.stencilFail;
	pipelineState.stencilZFail = entry.stencilZFail;
	pipelineState.stencilPass = entry.stencilPass;
	pipelineState.stencilMask = entry.stencilMask;
	pipelineState.stencilWriteMask = entry.stencilWriteMask;
	pipelineState.stencilRef = entry.stencilRef;
	pipelineState.stencilFuncCCW = entry.stencilFuncCCW;
	pipelineState.alphaBlendEnable = entry.alphaBlendEnable;
	pipelineState.blendOp = entry.blendOp;
	pipelineState.slopeScaledDepthBias = entry.slopeScaledDepthBias;
	pipelineState.depthBias = entry.depthBias;
	pipelineState.srcBlendAlpha = entry.srcBlendAlpha;
	pipelineState.destBlendAlpha = entry.destBlendAlpha;
	pipelineState.blendOpAlpha = entry.blendOpAlpha;
	pipelineState.colorWriteEnable = entry.colorWriteEnable;
	pipelineState.primitiveTopology = entry.primitiveTopology;
	memcpy(pipelineState.vertexStrides, entry.vertexStrides, sizeof(pipelineState.vertexStrides));
	pipelineState.renderTargetFormat = entry.renderTargetFormat;
	pipelineState.depthStencilFormat = entry.depthStencilFormat;
	pipelineState.sampleCount = entry.sampleCount;
	pipelineState.enableAlphaToCoverage = entry.enableAlphaToCoverage;
	pipelineState.specConstants = entry.specConstants;

	SanitizePipelineState(pipelineState);

	XXH64_hash_t hash = ComputePipelineStateHash(pipelineState);

	// The render thread owns g_pipelines once the game runs; pipelines made elsewhere reach
	// it through the render queue.
	if (PrecompilesPipelinesInBackground())
		return CompilePipelineInBackground(hash, pipelineState);

	auto& pipeline = g_pipelines[hash];
	if (pipeline == nullptr)
	{
		pipeline = CreateGraphicsPipeline(pipelineState);
		return true;
	}
	return false;
}

static void PrecompileStaticPipelines(const std::function<void(float)>& progress = {})
{
	if constexpr (g_staticPipelineCacheCount == 0)
		return;

	LOGFN("PipelineCache: Precompiling {} static pipelines...", g_staticPipelineCacheCount);

	auto totalStart = std::chrono::steady_clock::now();
	uint32_t compiled = 0;
	uint32_t deferred = 0;

	auto entries = reinterpret_cast<const PipelineCacheEntry*>(g_staticPipelineCacheData);
	for (size_t i = 0; i < g_staticPipelineCacheCount; i++)
	{
		if (TryCompileFromCacheEntry(entries[i]))
		{
			compiled++;
		}
		else
		{
			EnqueueDeferredPipeline(entries[i]);
			deferred++;
		}

		if (progress)
		{
			progress(PIPELINE_PRECOMPILE_SHADER_STAGE_END + (1.0f - PIPELINE_PRECOMPILE_SHADER_STAGE_END) * (float)(i + 1) / (float)g_staticPipelineCacheCount);
		}
	}

	{
		std::lock_guard lock(g_pipelineCacheMutex);
		for (size_t i = 0; i < g_staticPipelineCacheCount; i++)
		{
			PipelineCacheEntry entry = entries[i];
			entry.pipelineHash = ComputePipelineEntryHash(entry);
			g_pipelineCacheEntries.emplace(entry.pipelineHash, entry);
		}
	}

	auto totalElapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart).count();
	LOGFN("PipelineCache: Static {} {} pipelines ({} deferred) in {:.2f} ms",
		PrecompilesPipelinesInBackground() ? "queued" : "precompiled", compiled, deferred, totalElapsed);
}


template<X_D3DPRIMITIVETYPE PrimitiveType>
struct PrimitiveIndexData
{
	std::vector<uint16_t> indexData;
	RenderBufferReference indexBuffer;
	uint32_t currentIndexCount = 0;

	uint32_t prepare(uint32_t guestPrimCount)
	{
		uint32_t primCount;
		uint32_t indexCountPerPrimitive;

		switch (PrimitiveType)
		{
		case X_D3DPT_TRIANGLEFAN:
			primCount = guestPrimCount - 2;
			indexCountPerPrimitive = 3;
			break;
		case X_D3DPT_QUADLIST:
			primCount = guestPrimCount / 4;
			indexCountPerPrimitive = 6;
			break;
		default:
			assert(false && "Unknown primitive type.");
			break;
		}

		uint32_t indexCount = primCount * indexCountPerPrimitive;

		if (indexData.size() < indexCount)
		{
			const size_t oldPrimCount = indexData.size() / indexCountPerPrimitive;
			indexData.resize(indexCount);

			for (size_t i = oldPrimCount; i < primCount; i++)
			{
				switch (PrimitiveType)
				{
				case X_D3DPT_TRIANGLEFAN:
				{
					indexData[i * 3 + 0] = 0;
					indexData[i * 3 + 1] = static_cast<uint16_t>(i + 1);
					indexData[i * 3 + 2] = static_cast<uint16_t>(i + 2);
					break;
				}
				case X_D3DPT_QUADLIST:
				{
					indexData[i * 6 + 0] = static_cast<uint16_t>(i * 4 + 0);
					indexData[i * 6 + 1] = static_cast<uint16_t>(i * 4 + 1);
					indexData[i * 6 + 2] = static_cast<uint16_t>(i * 4 + 2);

					indexData[i * 6 + 3] = static_cast<uint16_t>(i * 4 + 2); //was i + 4 +  0 //maintains consistent clockwise winding for both triangle
					indexData[i * 6 + 4] = static_cast<uint16_t>(i * 4 + 3); //was was i + 4 + 2
					indexData[i * 6 + 5] = static_cast<uint16_t>(i * 4 + 0); //was was i + 4 +  3
					break;
				}
				default:
					assert(false && "Unknown primitive type.");
					break;
				}
			}
		}

		if (indexBuffer == NULL || currentIndexCount < indexCount)
		{
			auto allocation = g_uploadAllocators[g_frame].allocate(indexData.data(), indexCount * 2, 2);
			indexBuffer = allocation.buffer->at(allocation.offset);
			currentIndexCount = indexCount;
		}

		SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, indexBuffer);
		SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, indexCount * 2);
		SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format, RenderFormat::R16_UINT);

		return indexCount;
	}

	void reset()
	{
		indexBuffer = {};
		currentIndexCount = 0;
	}
};

static PrimitiveIndexData<X_D3DPT_QUADLIST> g_quadIndexData;

std::vector<GuestResource*> g_tempUnreadyResource; // store VB that weren't ready to be deleted
// Resize the tracked textures and surfaces on the next frame.
std::atomic<bool> g_pendingSurfaceResizeFlag = false;
std::atomic<uint32_t> g_fmvFrameWidth = 0;
std::atomic<uint32_t> g_fmvFrameHeight = 0;
std::atomic<bool> g_frameReadsGuestMemory = false;
static Mutex g_surfaceRTsMutex; // Multiple threads can access Surface RT so we can prevent a potential race issue with mutex
static Mutex g_tempUnreadyResourceMutex; // for destruct resource (VB, texture stuff)

// Textures and surfaces to resize, protected by g_surfaceRTsMutex.
// Add when ready to use; remove before freeing the resource.
static ankerl::unordered_dense::set<GuestTexture*> g_resizableTextures;
static ankerl::unordered_dense::set<GuestSurface*> g_resizableSurfaces;

static std::atomic<uint64_t> g_guestTextureGeneration{ 1 };
uint64_t NextGuestTextureGeneration()
{
	return g_guestTextureGeneration.fetch_add(1, std::memory_order_relaxed);
}

// per surface
static ankerl::unordered_dense::map<const RenderTexture*, ankerl::unordered_dense::set<GuestSurface*>> g_framebufferCacheUsers;

static void InvalidateFramebufferCache(const RenderTexture* texture)
{
	const auto users = g_framebufferCacheUsers.find(texture);
	if (users == g_framebufferCacheUsers.end())
		return;

	for (auto* container : users->second)
		container->framebuffers.erase(texture);

	g_framebufferCacheUsers.erase(users);
}

static void UnregisterFramebufferCacheContainer(GuestSurface* container)
{
	for (auto it = g_framebufferCacheUsers.begin(); it != g_framebufferCacheUsers.end();)
	{
		it->second.erase(container);

		if (it->second.empty())
			it = g_framebufferCacheUsers.erase(it);
		else
			++it;
	}
}

static void DestructTempResources()
{
	bool valid = false;

	for (auto resource : g_tempResources[g_frame])
	{
		valid = false;

		switch (resource->type)
		{
		case ResourceType::Texture:
		case ResourceType::VolumeTexture:
		{
			const auto texture = reinterpret_cast<GuestTexture*>(resource);

			valid = texture->shouldFreeMemory;
			DiscardStagedBarrier(texture);
			DiscardStagedBarrier(texture->patchedTexture.get());

			{
				std::lock_guard lock(g_surfaceRTsMutex);
				g_resizableTextures.erase(texture);

				for (auto* surface : texture->surfaceRTs)
				{
					if (surface->parentTexture == texture)
						surface->parentTexture = nullptr;

					if (surface->texture == texture->texture)
					{
						surface->framebuffers.clear();
						surface->texture = nullptr;
					}
				}

				InvalidateFramebufferCache(texture->texture);
			}

			if (texture->mappedMemory != nullptr)
				g_userHeap.Free(texture->mappedMemory);

			if (texture->descriptorIndex != NULL)
			{
				g_textureDescriptorSet->setTexture(texture->descriptorIndex, nullptr, {});
				g_textureDescriptorAllocator.free(texture->descriptorIndex);
			}

			if (texture->patchedTexture != nullptr && texture->patchedTexture->descriptorIndex != NULL)
			{
				g_textureDescriptorSet->setTexture(texture->patchedTexture->descriptorIndex, nullptr, {});
				g_textureDescriptorAllocator.free(texture->patchedTexture->descriptorIndex);
			}


			texture->~GuestTexture();


			break;
		}

		case ResourceType::VertexBuffer:
		case ResourceType::IndexBuffer:
		{
			const auto buffer = reinterpret_cast<GuestBuffer*>(resource);
			bool invalid = false;
			for (int i = 0; i < 16; i++)
			{
				if (buffer->buffer->at(0) == g_vertexBufferViews[i].buffer) // ensure Render Thread isn't about to use the VB while we try to delete it
				{
					std::lock_guard lock(g_tempUnreadyResourceMutex);
					g_tempUnreadyResource.push_back(resource); //we can store the resource to delete it later when the render thread isn't trying to use it
					invalid = true;
					break;
				}
			}

			if (invalid) //fine to not delete now, it will be deleted next time 
				continue;

			//printf("destruct buffer %x\n", buffer);
			if (buffer->mappedMemory != nullptr)
				g_userHeap.Free(buffer->mappedMemory);

			buffer->~GuestBuffer();

			valid = true;
			break;
		}

		case ResourceType::RenderTarget:
		case ResourceType::DepthStencil:
		{

			const auto surface = reinterpret_cast<GuestSurface*>(resource);

			std::lock_guard lock(g_surfaceRTsMutex);

			g_resizableSurfaces.erase(surface);
			UnregisterFramebufferCacheContainer(surface);

			if (surface->parentTexture != nullptr)
			{
				const auto parent = surface->parentTexture;
				if (parent->type == ResourceType::Texture || parent->type == ResourceType::VolumeTexture)
				{
					auto& vec = parent->surfaceRTs;
					auto it = std::find(vec.begin(), vec.end(), surface);
					if (it != vec.end())
						vec.erase(it);
				}
				surface->parentTexture = nullptr;
			}

			if (surface->descriptorIndex != NULL)
			{
				g_textureDescriptorSet->setTexture(surface->descriptorIndex, nullptr, {});
				g_textureDescriptorAllocator.free(surface->descriptorIndex);
			}

			DiscardStagedBarrier(surface);

			surface->~GuestSurface();

			valid = true;
			break;
		}

		case ResourceType::VertexDeclaration:
			reinterpret_cast<GuestVertexDeclaration*>(resource)->~GuestVertexDeclaration();
			break;
		case ResourceType::VertexShader:
		case ResourceType::PixelShader:
		{
			reinterpret_cast<GuestShader*>(resource)->~GuestShader();
			break;
		}
		}

		if (valid)
			g_userHeap.Free(resource);
	}

	g_tempResources[g_frame].clear();
	g_tempBuffers[g_frame].clear();
}


#define SPEC_CONSTANT_ALPHA_TEST        (1 << 2)
#define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
// Records a change to shared pixel shader constants.
// `changed` comes from a SetDirtyValue call; controls that feed the
// fixed-function selector also reselect the specialized pixel shader.
static void MarkSharedPixelConstantsDirty(bool changed, bool affectsSelector)
{
	g_dirtyStates.sharedConstants |= changed;
	if (affectsSelector)
		g_fixedFunctionPixelShaderStateDirty |= changed;
}

static void SetAlphaTestMode(bool enable)
{
	uint32_t specConstants = 0;

	if (enable)
	{
		specConstants = SPEC_CONSTANT_ALPHA_TEST;
	}

	specConstants |= (g_pipelineState.specConstants & ~(SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE));
	bool selectorDirty = false;
	SetDirtyValue(selectorDirty, g_sharedConstants.alphaTestEnable, uint32_t(enable != 0));
	MarkSharedPixelConstantsDirty(selectorDirty, true);
}

moodycamel::BlockingConcurrentQueue<RenderCommand> g_renderQueue;


//specify the "config" of the rendering, essentially prepare the GPU to how it should render. (alpha, blending, culling etc.)
void SetRenderState(X_D3DRENDERSTATETYPE state, uint32_t value)
{
	RenderCommand cmd;
	cmd.type = RenderCommandType::SetRenderState;
	cmd.setRenderState.type = state;
	cmd.setRenderState.value = value;
	g_pendingRenderCommands.enqueue() = cmd;
}

static void ProcSetRenderState(const RenderCommand& cmd)
{
	uint32_t value = cmd.setRenderState.value;

	switch (cmd.setRenderState.type)
	{
	case X_D3DRS_VERTEXBLEND:
	{

		break;
	}
	case X_D3DRS_FOGCOLOR:
	{
		uint32_t fog = value;

		float r = ((fog >> 16) & 0xFF) / 255.0f;
		float g = ((fog >> 8) & 0xFF) / 255.0f;
		float b = ((fog >> 0) & 0xFF) / 255.0f;

		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogColor[0], r);
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogColor[1], g);
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogColor[2], b);

		break;
	}
	case X_D3DRS_FILLMODE:
	{

		break;
	}
	case X_D3DRS_BACKFILLMODE:
	{

		break;
	}
	case X_D3DRS_TWOSIDEDLIGHTING:
	{

		break;
	}
	case X_D3DRS_NORMALIZENORMALS:
	{

		break;
	}
	case X_D3DRS_ZENABLE:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zEnable, value != 0);
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilEnable, value != 0);
		g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
		break;
	}
	case X_D3DRS_STENCILENABLE:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilEnable, value != 0);
		g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
		break;
	}
	case X_D3DRS_STENCILFAIL:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilFail, ConvertStencilOp(value));
		break;
	}
	case X_D3DRS_CULLMODE:
	{
		RenderCullMode cullMode;

		switch (value) {
		case X_D3DCULL_NONE:
			cullMode = RenderCullMode::NONE;
			break;
		case X_D3DCULL_CW:
			cullMode = RenderCullMode::FRONT;
			break;
		case X_D3DCULL_CCW:
			cullMode = RenderCullMode::BACK;
			break;
		default:
			assert(false && "Invalid cull mode");
			cullMode = RenderCullMode::NONE;
			break;
		}

		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.cullMode, cullMode);
		break;
	}
	case X_D3DRS_FRONTFACE:
	{

		break;
	}
	case X_D3DRS_TEXTUREFACTOR:
	{
		uint32_t tf = value;


		float r = ((tf >> 16) & 0xFF) / 255.0f;
		float g = ((tf >> 8) & 0xFF) / 255.0f;
		float b = ((tf >> 0) & 0xFF) / 255.0f;
		float a = ((tf >> 24) & 0xFF) / 255.0f;

		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.TextureFactor[0], r);
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.TextureFactor[1], g);
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.TextureFactor[2], b);
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.TextureFactor[3], a);

		break;
	}
	case X_D3DRS_ZBIAS:
	{
		FLOAT Biased = static_cast<FLOAT>(value) * -0.000005f;
		value = *reinterpret_cast<const DWORD*>(&Biased);

		if (g_capabilities.dynamicDepthBias)
			SetDirtyValue(g_dirtyStates.depthBias, g_depthBias, int32_t(*reinterpret_cast<float*>(&value)));
		else
			SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthBias, int32_t(*reinterpret_cast<float*>(&value)));

		break;
	}
	case X_D3DRS_LOGICOP:
	{
		break;
	}
	case X_D3DRS_EDGEANTIALIAS:
	{
		break;
	}
	case X_D3DRS_MULTISAMPLEANTIALIAS:
	{
		break;
	}
	case X_D3DRS_MULTISAMPLEMASK:
	{
		break;
	}
	case X_D3DRS_MULTISAMPLEMODE:
	{
		break;
	}
	case X_D3DRS_MULTISAMPLERENDERTARGETMODE:
	{

		break;
	}
	case X_D3DRS_SHADOWFUNC:
	{

		break;
	}
	case X_D3DRS_LINEWIDTH:
	{

		break;
	}
	case X_D3DRS_SAMPLEALPHA:
	{

		break;
	}
	case X_D3DRS_DXT1NOISEENABLE:
	{
		//SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
		break;
	}
	case X_D3DRS_YUVENABLE:
	{
		//SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
		break;
	}
	case X_D3DRS_OCCLUSIONCULLENABLE:
	{
		//SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
		break;
	}
	case X_D3DRS_STENCILCULLENABLE:
	{
		//SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
		break;
	}

	case X_D3DRS_SRCBLEND:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlend, ConvertBlendMode(value));
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlendAlpha, ConvertAlphaBlendMode(value)); //DX8 originally does not have src blend alpha so we mirror "regular" blend.
		break;
	}
	case X_D3DRS_DESTBLEND:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlend, ConvertBlendMode(value));
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlendAlpha, ConvertAlphaBlendMode(value)); //DX8 originally does not have dst blend alpha so we mirror "regular" blend.
		break;
	}
	case X_D3DRS_ZFUNC:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zFunc, ConvertD3DCMP(value));
		break;
	}
	case X_D3DRS_STENCILFUNC:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilFunc, ConvertD3DCMP(value));
		break;
	}
	case X_D3DRS_STENCILZFAIL:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilZFail, ConvertStencilOp(value));
		break;
	}
	case X_D3DRS_STENCILPASS:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.stencilPass, ConvertStencilOp(value));
		break;
	}
	case X_D3DRS_ZWRITEENABLE:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
		break;
	}
	case X_D3DRS_ALPHAFUNC:
	{
		bool selectorDirty = false;
		SetDirtyValue(selectorDirty, g_sharedConstants.alphaFunc, (uint32_t)ConvertD3DCMP(value));
		MarkSharedPixelConstantsDirty(selectorDirty, true);
		break;
	}
	case X_D3DRS_ALPHATESTENABLE:
	{
		SetAlphaTestMode(value != 0);
		break;
	}
	case X_D3DRS_ALPHAREF:
	{
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.alphaRef, float(value) / 255.0f);
		break;
	}
	case X_D3DRS_ALPHABLENDENABLE:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.alphaBlendEnable, value != 0);
		break;
	}
	case X_D3DRS_BLENDOP:
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.blendOp, ConvertBlendOp(value));
		break;
	}
	case X_D3DRS_COLORWRITEENABLE:
	{
		DWORD OrigValue = value;
		value = 0;

		if (OrigValue & (1L << 16))
			value |= D3DCOLORWRITEENABLE_RED;


		if (OrigValue & (1L << 8))
			value |= D3DCOLORWRITEENABLE_GREEN;


		if (OrigValue & (1L << 0))
			value |= D3DCOLORWRITEENABLE_BLUE;


		if (OrigValue & (1L << 24))
			value |= D3DCOLORWRITEENABLE_ALPHA;


		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.colorWriteEnable, value);
		g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
	}
	break;
	//fog is handled in shader constant directly
	case X_D3DRS_FOGTABLEMODE:
	{
		bool selectorDirty = false;
		SetDirtyValue(selectorDirty, g_sharedConstants.FogTableMode, value);
		MarkSharedPixelConstantsDirty(selectorDirty, true);
		break;
	}
	case X_D3DRS_FOGDENSITY:
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogDensity, value);
		break;
	case X_D3DRS_FOGSTART:
	{
		float fogValue = *(float*)&value;
		if (fogValue < 0.0f) {
			fogValue = std::abs(fogValue);
			value = *(DWORD*)&fogValue;
		}

		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogStart, value);
	}
	break;
	case X_D3DRS_FOGEND:
	{
		float fogValue = *(float*)&value;
		if (fogValue < 0.0f) {
			fogValue = std::abs(fogValue);
			value = *(DWORD*)&fogValue;
		}

		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.FogEnd, value);
	}
	break;
	case X_D3DRS_FOGENABLE:
	{
		bool selectorDirty = false;
		SetDirtyValue(selectorDirty, g_sharedConstants.FogEnable, value);
		MarkSharedPixelConstantsDirty(selectorDirty, true);
		break;
	}
	case X_D3DRS_SPECULARENABLE:
	{
		bool selectorDirty = false;
		SetDirtyValue(selectorDirty, g_sharedConstants.SpecularEnable, value);
		MarkSharedPixelConstantsDirty(selectorDirty, true);
		break;
	}
	}
}

static std::unique_ptr<RenderShader> g_copyShader;
static std::unique_ptr<RenderShader> g_copyColorShader;
static std::unique_ptr<RenderPipeline> g_copyDepthPipeline;

std::unique_ptr<RenderShader> FixedFuncVS = nullptr;
std::unique_ptr<RenderShader> PassthroughVS = nullptr; //this is for vertex shader that don't have a program, similar to fixed, but vertex are already transformed.
std::unique_ptr<RenderShader> FixedFuncPS = nullptr;

#ifdef SHADOW_RECOMP_D3D12

#define CREATE_SHADER(NAME) \
    g_device->createShader( \
        g_vulkan ? g_##NAME##_spirv : g_##NAME##_dxil, \
        g_vulkan ? sizeof(g_##NAME##_spirv) : sizeof(g_##NAME##_dxil), \
        "main", \
        g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL)

#define CREATE_RUNTIME_SHADER(data, size) \
    g_device->createShader(data, size, "main", \
	g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL)

#else

#define CREATE_SHADER(NAME) \
    g_device->createShader(g_##NAME##_spirv, sizeof(g_##NAME##_spirv), "main", RenderShaderFormat::SPIRV)

#define CREATE_RUNTIME_SHADER(data, size) \
   g_device->createShader(data, size, "main", RenderShaderFormat::SPIRV)

#endif


static constexpr size_t TEXTURE_DESCRIPTOR_SIZE = 65536;
static constexpr size_t SAMPLER_DESCRIPTOR_SIZE = 1024;

static std::unique_ptr<GuestTexture> g_imFontTexture;
static std::unique_ptr<RenderPipelineLayout> g_imPipelineLayout;
static std::unique_ptr<RenderPipeline> g_imPipeline;
static std::unique_ptr<RenderPipeline> g_imAdditivePipeline;

// Not a template so textures.cpp can call it too. The command list is passed in,
// that way g_copyCommandList stays private to this file.
void ExecuteCopyCommandList(const std::function<void(RenderCommandList*)>& function)
{
	std::lock_guard lock(g_copyMutex);

	g_copyCommandList->begin();
	function(g_copyCommandList.get());
	g_copyCommandList->end();
	g_copyQueue->executeCommandLists(g_copyCommandList.get(), g_copyCommandFence.get());
	g_copyQueue->waitForCommandFence(g_copyCommandFence.get());
}

// this new commandList is needed for the unitialized resource (RT / Depth) that need to be cleared before being used
// To avoid race issues with the main command list, we use a separate command list and queue.
static std::mutex g_initMutex;
static std::unique_ptr<RenderCommandList> g_initCommandList;
static std::unique_ptr<RenderCommandFence> g_initCommandFence;
static std::unique_ptr<RenderCommandQueue> g_initQueue;


template<typename T>
static void ExecuteInitCommandList(const T& function)
{
	std::lock_guard lock(g_initMutex);
	g_initCommandList->begin();
	function();
	g_initCommandList->end();
	g_initQueue->executeCommandLists(g_initCommandList.get(), g_initCommandFence.get());
	g_initQueue->waitForCommandFence(g_initCommandFence.get());
}


struct ImGuiPushConstants
{
	ImVec2 boundsMin{};
	ImVec2 boundsMax{};
	ImU32 gradientTopLeft{};
	ImU32 gradientTopRight{};
	ImU32 gradientBottomRight{};
	ImU32 gradientBottomLeft{};
	uint32_t shaderModifier{};
	uint32_t texture2DDescriptorIndex{};
	ImVec2 displaySize{};
	ImVec2 inverseDisplaySize{};
	ImVec2 origin{ 0.0f, 0.0f };
	ImVec2 scale{ 1.0f, 1.0f };
	ImVec2 proceduralOrigin{ 0.0f, 0.0f };
	float outline{};
};

extern ImFontBuilderIO g_fontBuilderIO;

static void CreateImGuiBackend()
{
	LOG_UTILITY("Initializing ImGui...");
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

#ifdef ENABLE_IM_FONT_ATLAS_SNAPSHOT
	IM_DELETE(io.Fonts);
	io.Fonts = ImFontAtlasSnapshot::Load();

#else
	ImFontAtlasSnapshot::GenerateGlyphRanges();
	ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-DB.otf");
	ImFontAtlasSnapshot::GetFont("DFPZongYiW5-B5.ttc");

#endif


	AchievementMenu::Init();
	AchievementOverlay::Init();
	MultiplayerHud::Init();


	InitImGuiUtils();
	ButtonGuide::Init();
	InstallerWizard::Init();
	OptionsMenu::Init();
	MessageWindow::Init();


	ImGui_ImplSDL3_InitForOther(GameWindow::s_pWindow);
	ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);


#ifdef ENABLE_IM_FONT_ATLAS_SNAPSHOT 
	uint8_t* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	g_imFontTexture = LoadCustomTexture(decompressZstd(g_im_font_atlas_texture, g_im_font_atlas_texture_uncompressed_size).get(), g_im_font_atlas_texture_uncompressed_size);
#else
	io.Fonts->FontBuilderIO = &g_fontBuilderIO;
	io.Fonts->Build();

	g_imFontTexture = std::make_unique<GuestTexture>(ResourceType::Texture);

	uint8_t* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	RenderTextureDesc textureDesc;
	textureDesc.dimension = RenderTextureDimension::TEXTURE_2D;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.depth = 1;
	textureDesc.mipLevels = 1;
	textureDesc.arraySize = 1;
	textureDesc.format = RenderFormat::R8G8B8A8_UNORM;

	g_imFontTexture->textureHolder = g_device->createTexture(textureDesc);
	g_imFontTexture->texture = g_imFontTexture->textureHolder.get();
	g_imFontTexture->textureGeneration = NextGuestTextureGeneration();

	uint32_t rowPitch = (width * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
	uint32_t slicePitch = (rowPitch * height + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
	auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch));
	uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

	if (rowPitch == (width * 4))
	{
		memcpy(mappedMemory, pixels, slicePitch);
	}
	else
	{
		for (size_t i = 0; i < height; i++)
		{
			memcpy(mappedMemory, pixels, width * 4);
			pixels += width * 4;
			mappedMemory += rowPitch;
		}
	}

	uploadBuffer->unmap();

	ExecuteCopyCommandList([&](RenderCommandList* commandList)
		{
			commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g_imFontTexture->texture, RenderTextureLayout::COPY_DEST));

			commandList->copyTextureRegion(
				RenderTextureCopyLocation::Subresource(g_imFontTexture->texture, 0),
				RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, rowPitch / 4, 0));
		});

	g_imFontTexture->layout = RenderTextureLayout::COPY_DEST;

	RenderTextureViewDesc textureViewDesc;
	textureViewDesc.format = textureDesc.format;
	textureViewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
	textureViewDesc.mipLevels = 1;
	g_imFontTexture->textureView = g_imFontTexture->texture->createTextureView(textureViewDesc);

	g_imFontTexture->descriptorIndex = g_textureDescriptorAllocator.allocate();
	g_textureDescriptorSet->setTexture(g_imFontTexture->descriptorIndex, g_imFontTexture->texture, RenderTextureLayout::SHADER_READ, g_imFontTexture->textureView.get());
#endif

	io.Fonts->SetTexID(g_imFontTexture.get());

	RenderPipelineLayoutBuilder pipelineLayoutBuilder;
	pipelineLayoutBuilder.begin(false, true);

	RenderDescriptorSetBuilder descriptorSetBuilder;
	descriptorSetBuilder.begin();
	descriptorSetBuilder.addTexture(0, TEXTURE_DESCRIPTOR_SIZE);
	descriptorSetBuilder.end(true, TEXTURE_DESCRIPTOR_SIZE);
	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

	descriptorSetBuilder.begin();
	descriptorSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_SIZE);
	descriptorSetBuilder.end(true, SAMPLER_DESCRIPTOR_SIZE);
	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

	pipelineLayoutBuilder.addPushConstant(0, 2, sizeof(ImGuiPushConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);

	pipelineLayoutBuilder.end();
	g_imPipelineLayout = pipelineLayoutBuilder.create(g_device.get());

	auto vertexShader = CREATE_SHADER(imgui_vs);
	auto pixelShader = CREATE_SHADER(imgui_ps);

	RenderInputElement inputElements[3];
	inputElements[0] = RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, pos));
	inputElements[1] = RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, uv));
	inputElements[2] = RenderInputElement("COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col));

	RenderInputSlot inputSlot(0, sizeof(ImDrawVert));

	RenderGraphicsPipelineDesc pipelineDesc;
	pipelineDesc.pipelineLayout = g_imPipelineLayout.get();
	pipelineDesc.vertexShader = vertexShader.get();
	pipelineDesc.pixelShader = pixelShader.get();
	pipelineDesc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
	pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
	pipelineDesc.renderTargetCount = 1;
	pipelineDesc.inputElements = inputElements;
	pipelineDesc.inputElementsCount = std::size(inputElements);
	pipelineDesc.inputSlots = &inputSlot;
	pipelineDesc.inputSlotsCount = 1;
	pipelineDesc.depthClipEnabled = true; // See g_copyDepthPipeline.
	g_imPipeline = g_device->createGraphicsPipeline(pipelineDesc);

	pipelineDesc.renderTargetBlend[0].dstBlend = RenderBlend::ONE;
	g_imAdditivePipeline = g_device->createGraphicsPipeline(pipelineDesc);

#ifndef ENABLE_IM_FONT_ATLAS_SNAPSHOT
	ImFontAtlasSnapshot snapshot;
	snapshot.Snap();

	FILE* file = fopen("im_font_atlas.bin", "wb");
	if (file)
	{
		fwrite(snapshot.data.data(), 1, snapshot.data.size(), file);
		fclose(file);
	}
	uint8_t* pixelsCopy;
	int atlasW = 0, atlasH = 0, bpp = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixelsCopy, &atlasW, &atlasH, &bpp);

	stbi_write_png("im_font_atlas.png", atlasW, atlasH, bpp, pixelsCopy, atlasW * bpp);
#endif

	LOG_UTILITY("ImGui Initialized");
}

static VulkanSwapChain* GetVulkanSwapChain()
{
	return static_cast<VulkanSwapChain*>(g_swapChain.get());
}

static void SetSwapChainVsync()
{
	g_swapChain->setVsyncEnabled(Config::VSync);
	// FIFO is default, MAILBOX if supported
	if (g_vulkan && Config::VSync && GetVulkanSwapChain()->mailboxPresentModeSupported)
	{
		GetVulkanSwapChain()->requiredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
	}
}

static const char* Vulkan_GetCurPresentModeName(VkPresentModeKHR mode)
{
	switch (mode)
	{
	case VK_PRESENT_MODE_IMMEDIATE_KHR:
		return "immediate_khr";
	case VK_PRESENT_MODE_MAILBOX_KHR:
		return "mailbox_khr";
	case VK_PRESENT_MODE_FIFO_KHR:
		return "fifo_khr";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
		return "fifo_relaxed_khr";
	default:
		return "unknown";
	}
}

// PLume rebuild the swap chain by using a previous instance (oldSwapChain)
// Vulkan then remove the old one even when the creation fail, meaning a bad rebuild will make it use
// a dead swap chain even if it retries after, this can happen during a vsync toggle.
// Fix: Destroy the old instance so Plume always build from scratch.

static bool Vulkan_RebuildSwapChain(const char* failReason)
{
	static uint32_t failedTries = 0;
	auto* curSwapChain = GetVulkanSwapChain();
	auto* cmdQueue = curSwapChain->commandQueue->queue;

	uint32_t windowWidth, windowHeight;
	curSwapChain->getWindowSize(windowWidth, windowHeight);
	const bool isWindowMinimized = windowWidth == 0 || windowHeight == 0;

	// The Presents calls are not covered by our fences, so empty the queue before the old 
	// images go away.
	VkResult vkqueueState;
	{
		std::scoped_lock lock(*cmdQueue->mutex);
		vkqueueState = vkQueueWaitIdle(cmdQueue->vk);
	}

	curSwapChain->releaseImageViews();
	curSwapChain->releaseSwapChain();

	const bool swapchainRebuilt = curSwapChain->resize();

	if (!swapchainRebuilt)
	{
		curSwapChain->vk = VK_NULL_HANDLE;

		if (!isWindowMinimized)
		{
			failedTries++;
		}

		return false;
	}

	if (failedTries != 0)
	{
		LOGF_WARNING("Swap chain rebuilt after {} failed tries: {}x{} {}, {} images", failedTries,
			curSwapChain->width, curSwapChain->height, Vulkan_GetCurPresentModeName(curSwapChain->createdPresentMode),
			curSwapChain->getTextureCount());
		failedTries = 0;
	}

	return true;
}

static void ComputeViewportSize();

static std::atomic<bool> g_swapChainRebuildRequested{ false };

static void CheckSwapChain(bool canRebuild)
{
	SetSwapChainVsync();

#ifdef __ANDROID__
	// Android takes the window surface away in the background and hands out a new one
	// afterwards; see gpu/android_video.h.
	switch (AndroidVideo::GetSurfaceState(g_swapChain.get(), GameWindow::s_pWindow))
	{
	case AndroidVideo::ESurfaceState::Unavailable:
		// Nothing to present to. The frame is still rendered, just not shown.
		g_swapChainValid = false;
		g_swapChainRebuildRequested = false;
		g_backBuffer->width = Video::s_viewportWidth;
		g_backBuffer->height = Video::s_viewportHeight;
		return;

	case AndroidVideo::ESurfaceState::Replaced:
		// Take the rebuild path below, which recreates the surface on the new window.
		g_swapChainValid = false;
		break;

	case AndroidVideo::ESurfaceState::Ready:
		break;
	}

	// From here the game is up and a window exists: if nothing presentable comes out for a
	// long while, the driver never puts a frame on the screen.
	AndroidVideo::WatchdogFrameTick();
#endif

	const bool isSwapChainInvalid = !g_swapChainValid;
	const bool needsRebuild = isSwapChainInvalid || g_swapChain->needsResize();
	g_swapChainRebuildRequested = needsRebuild && !canRebuild;

	if (needsRebuild && canRebuild)
	{
		Video::WaitForGPU();
		{
			std::lock_guard lock(g_surfaceRTsMutex);
			// These framebuffers may still use the old swapchain images.
			g_backBuffer->framebuffers.clear();
			for (auto* surface : g_resizableSurfaces)
				surface->framebuffers.clear();
			for (auto* texture : g_resizableTextures)
			{
				for (auto* alias : texture->surfaceRTs)
				{
					alias->framebuffers.clear();
				}
			}
		}

		bool hasSurface = true;

#ifdef __ANDROID__
		// The GPU is idle and nothing references the swap chain images any more, which is
		// what replacing the surface needs. A no-op when the window did not change. If the
		// window is gone again there is nothing to build a swap chain on; a later frame retries.
		hasSurface = AndroidVideo::RecreateSurface(g_swapChain.get(), GameWindow::s_pWindow);
#endif

		if (!hasSurface)
			g_swapChainValid = false;
		else if (g_vulkan)
			g_swapChainValid = Vulkan_RebuildSwapChain(isSwapChainInvalid ? "Swap Chain was invalid" : "size or present mode changed");
		else
			g_swapChainValid = g_swapChain->resize();

		g_needsResize = g_swapChainValid;
	}

	if (g_swapChainValid)
	{
		g_swapChainAcquireProfiler.Begin();
		g_swapChainValid = g_swapChain->acquireTexture(g_acquireSemaphores[g_frame].get(), &g_backBufferIndex);
		g_swapChainAcquireProfiler.End();

#ifdef __ANDROID__
		if (g_swapChainValid)
			AndroidVideo::OnFramePresentable();
#endif
	}

	if (g_needsResize)
		ComputeViewportSize();

	g_backBuffer->width = Video::s_viewportWidth;
	g_backBuffer->height = Video::s_viewportHeight;
}

// Scale surfaces to the viewport while holding g_surfaceRTsMutex.
static void GetUpscaledSurfaceResolution(uint32_t widthOrigin, uint32_t heightOrigin,
	uint32_t* widthOut, uint32_t* heightOut)
{
	const auto [width, height] = surface_resize::UpscaledExtent(widthOrigin, heightOrigin,
		Video::s_viewportWidth, Video::s_viewportHeight, Config::ResolutionScale.Value);
	*widthOut = width;
	*heightOut = height;
}

#ifdef __ANDROID__
// The depth buffer of the game is one of 640x480 on the Xbox, used with the back buffer
// and with the 512x512 texture the scene is drawn into alike: the Xbox does not mind a
// depth buffer that is smaller than what is drawn into, as long as nothing is drawn
// outside it. Here it is created as 512x512, so that it covers that texture, which makes
// it narrower than the back buffer. Vulkan wants every part of a frame buffer to cover
// the frame buffer. Desktop drivers and Turnip draw regardless; a driver that goes by the
// rules, as the tile-based ones of other phone GPUs have reason to, need not draw
// anything into such a frame buffer. So this depth buffer is made to cover both.
static void CoverBackBuffer(uint32_t* width, uint32_t* height)
{
	*width = std::max(*width, Video::s_viewportWidth);
	*height = std::max(*height, Video::s_viewportHeight);
}
#endif

// This is needed because uninitialized RT / depth can crash the game under specific windows version if (and only if) VS is installed.
void DiscardInitRenderResource(RenderTexture* texture, const bool isRT)
{

	auto targetLayout = (isRT == false)
		? RenderTextureLayout::DEPTH_WRITE
		: RenderTextureLayout::COLOR_WRITE;

	ExecuteInitCommandList([&]
		{
			g_initCommandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, targetLayout));
			g_initCommandList->discardTexture(texture);
			g_initCommandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
		});
}

// This is used when user request new resolution / aspect ratio or when
// the window changes size. We have to remake all RT and Depth stencil texture.
//
// Wait for GPU work to finish and hold g_surfaceRTsMutex before calling.
static void RecreateGuestSurfaceTexture(GuestBaseTexture* tex, bool isRT)
{
	g_barrierMap.erase(tex->texture);
	InvalidateFramebufferCache(tex->texture);

	// explicitly discard the textureView
	tex->textureView = nullptr;

	auto targetLayout = (isRT == false)
		? RenderTextureLayout::DEPTH_WRITE
		: RenderTextureLayout::COLOR_WRITE;

	uint32_t newWidth, newHeight;

	GetUpscaledSurfaceResolution(tex->g_width, tex->g_height, &newWidth, &newHeight);

#ifdef __ANDROID__
	if (tex->coversBackBuffer)
		CoverBackBuffer(&newWidth, &newHeight);
#endif

	tex->textureHolder = g_device->createTexture(RenderTextureDesc::Texture2D(newWidth, newHeight, 1, tex->format, isRT ? RenderTextureFlag::RENDER_TARGET : RenderTextureFlag::DEPTH_TARGET));
	tex->texture = tex->textureHolder.get();
	tex->textureGeneration = NextGuestTextureGeneration();

	// Keep this framebuffer alive until the GPU finishes clearing the texture.
	std::unique_ptr<RenderFramebuffer> tempFB;
	ExecuteInitCommandList([&] // Discard texture, this is needed, see the function DiscardInitRenderResource for more info. 
		{
			g_initCommandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(tex->textureHolder.get(), targetLayout));
			g_initCommandList->discardTexture(tex->textureHolder.get());

			RenderFramebufferDesc fbDesc;

			if (isRT == true)
			{
				fbDesc.colorAttachments = const_cast<const RenderTexture**>(&tex->texture);
				fbDesc.colorAttachmentsCount = 1;
			}
			else
			{
				fbDesc.depthAttachment = tex->textureHolder.get();

			}

			tempFB = g_device->createFramebuffer(fbDesc);
			g_initCommandList->setFramebuffer(tempFB.get());

			if (isRT == false)
				g_initCommandList->clearDepthStencil(true, false); // D32_FLOAT has no stencil.
			else
				g_initCommandList->clearColor();

			g_initCommandList->setFramebuffer(nullptr);

			g_initCommandList->barriers(RenderBarrierStage::GRAPHICS,
				RenderTextureBarrier(tex->textureHolder.get(), RenderTextureLayout::SHADER_READ));
		});

	tex->width = newWidth;
	tex->height = newHeight;
	tex->layout = RenderTextureLayout::SHADER_READ;

	if (!isRT)
	{
		// needed for DX12 (handle null view race on boot)
		RenderTextureViewDesc viewDesc;
		viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
		viewDesc.format = tex->format;
		viewDesc.mipLevels = 1;
		tex->textureView = tex->textureHolder->createTextureView(viewDesc);
		// end null view DX12 patch
	}

	g_textureDescriptorSet->setTexture(tex->descriptorIndex, tex->texture,
		RenderTextureLayout::SHADER_READ, tex->textureView.get());
}

static void BeginCommandList(bool canResizeSurfaces)
{
	g_renderTarget = g_backBuffer;
	g_depthStencil = nullptr;
	g_framebuffer = nullptr;


	g_pipelineState.renderTargetFormat = BACKBUFFER_FORMAT;
	g_pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;

	if (g_swapChainValid)
	{
		// Keep other threads from changing the tracked resources during resize.
		std::lock_guard lock(g_surfaceRTsMutex);
		uint32_t width = Video::s_viewportWidth;
		uint32_t height = Video::s_viewportHeight;

		// Clear the request first so a new request isn't lost.
		const bool resizedSurfaces = canResizeSurfaces && g_pendingSurfaceResizeFlag.exchange(false);
		if (resizedSurfaces)
		{
			for (size_t i = 0; i < NUM_FRAMES; i++)
			{
				if (g_commandListStates[i])
				{
					g_queue->waitForCommandFence(g_commandFences[i].get());
					g_commandListStates[i] = false;
				}
			}

			surface_resize::RecreateTracked(g_resizableTextures, g_resizableSurfaces,
				DepthStencilFormat, RecreateGuestSurfaceTexture);
		}

		bool usingIntermediaryTexture = (width != g_swapChain->getWidth()) || (height != g_swapChain->getHeight()) ||
			(abs(Config::Brightness.Value - 0.5f) > 0.001f) || resizedSurfaces || (SWAPCHAIN_FORMAT != BACKBUFFER_FORMAT);

		if (usingIntermediaryTexture)
		{
			if (g_intermediaryBackBufferTextureWidth != width ||
				g_intermediaryBackBufferTextureHeight != height)
			{
				if (g_intermediaryBackBufferTextureDescriptorIndex == NULL)
					g_intermediaryBackBufferTextureDescriptorIndex = g_textureDescriptorAllocator.allocate();


				Video::WaitForGPU(); // Fine to wait for GPU, this'll only happen during resize.

				InvalidateFramebufferCache(g_intermediaryBackBufferTexture.get());
				g_backBuffer->framebuffers.clear();
				g_intermediaryBackBufferTexture = g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, BACKBUFFER_FORMAT, RenderTextureFlag::RENDER_TARGET));

				ExecuteInitCommandList([&] // Discard texture, this is needed, see the function DiscardInitRenderResource for more info. 
					{
						g_initCommandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_intermediaryBackBufferTexture.get(), RenderTextureLayout::COLOR_WRITE));
						g_initCommandList->discardTexture(g_intermediaryBackBufferTexture.get());
						g_initCommandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_intermediaryBackBufferTexture.get(), RenderTextureLayout::SHADER_READ));
					});

				g_textureDescriptorSet->setTexture(g_intermediaryBackBufferTextureDescriptorIndex, g_intermediaryBackBufferTexture.get(), RenderTextureLayout::SHADER_READ);

				g_intermediaryBackBufferTextureWidth = width;
				g_intermediaryBackBufferTextureHeight = height;
			}

			g_backBuffer->texture = g_intermediaryBackBufferTexture.get();
		}
		else
		{
			g_backBuffer->texture = g_swapChain->getTexture(g_backBufferIndex);
		}

		g_backBuffer->width = width;
		g_backBuffer->height = height;
	}
	else
	{
		g_backBuffer->texture = g_backBuffer->textureHolder.get();
	}

	g_backBuffer->textureGeneration = 0;
	g_backBuffer->layout = RenderTextureLayout::UNKNOWN;

	// Below is commented out as it was causing Stage1 textures to render as null for a few frames under certain camera angles
	// See https://github.com/FoxWorkshop/ShadowRecomp/issues/31
	/*
	for (size_t i = 0; i < 16; i++)
	{
		g_sharedConstants.texture2DIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D;
		g_sharedConstants.texture3DIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D;
		g_sharedConstants.textureCubeIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE;
	}

	memset(g_textures, 0, sizeof(g_textures));
	*/

	/**if (Config::GITextureFiltering == EGITextureFiltering::Bicubic)
		g_pipelineState.specConstants |= SPEC_CONSTANT_BICUBIC_GI_FILTER;
	else*/
	//g_pipelineState.specConstants &= ~SPEC_CONSTANT_BICUBIC_GI_FILTER;

	auto& commandList = g_commandLists[g_frame];

	commandList->begin();
	commandList->resetQueryPool(g_queryPools[g_frame].get(), 0, NUM_QUERIES);
	commandList->writeTimestamp(g_queryPools[g_frame].get(), 0);
	commandList->setGraphicsPipelineLayout(g_pipelineLayout.get()); //also set root signature
	commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0); //texture 2D also set RootDescriptorTable
	commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1); //texture 3D
	commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2); //texture cube
	commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3); //sampler


	g_clearedRenderTargetsThisFrame.clear();

	g_commandListOpen = true;
	g_readyForCommands = true;
	g_readyForCommands.notify_all();
}

template<typename T>
static void ApplyLowEndDefault(ConfigDef<T>& configDef, T newDefault, bool& changed)
{
	if (configDef.IsDefaultValue() && !configDef.IsLoadedFromConfig)
	{
		configDef = newDefault;
		changed = true;
	}

	configDef.DefaultValue = newDefault;
}

GuestBuffer* CreateVertexBuffer(uint32_t length);

bool Video::CreateHostDevice(const char* sdlVideoDriver, bool graphicsApiRetry)
{
	//the most important loop
	for (uint32_t i = 0; i < 16; i++)
		g_inputSlots[i].index = i; //plume check the index in setVertexBuffers function.

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();

	GameWindow::Init(sdlVideoDriver);
	STARTUP_PHASE("window created");


#ifdef SHADOW_RECOMP_D3D12
	g_vulkan = Config::GraphicsAPI == EGraphicsAPI::Vulkan;
#endif

	// Attempt to create the possible backends using a vector of function pointers. Whichever succeeds first will be the chosen API.
	using RenderInterfaceFunction = std::unique_ptr<RenderInterface>(void);
	std::vector<RenderInterfaceFunction*> interfaceFunctions;

#ifdef SHADOW_RECOMP_D3D12
	if (graphicsApiRetry)
	{
		// If we are attempting to create again after a reboot due to a crash, swap the order.
		g_vulkan = !g_vulkan;
	}

	interfaceFunctions.push_back(g_vulkan ? CreateVulkanInterface : CreateD3D12Interface);
	interfaceFunctions.push_back(g_vulkan ? CreateD3D12Interface : CreateVulkanInterface);
#else
	interfaceFunctions.push_back(CreateVulkanInterface);
#endif

	for (size_t i = 0; i < interfaceFunctions.size(); i++)
	{
		RenderInterfaceFunction* interfaceFunction = interfaceFunctions[i];

#ifdef SHADOW_RECOMP_D3D12
		// Wrap the device creation in __try/__except to survive from driver crashes.
		__try
#endif
		{
			g_interface = interfaceFunction();
			if (g_interface == nullptr)
			{
				continue;
			}

#ifdef __ANDROID__
			// Found out here, where it can be put in words, rather than by a renderer
			// that fails somewhere in a driver without the features it is built on.
			if (!AndroidVideo::CheckDeviceRequirements(g_interface.get()))
			{
				// Another driver may have the features; the next launch tries it.
				AndroidVideo::NoteRenderFailed("requirements");
				return false;
			}
#endif

			g_device = g_interface->createDevice(Config::GraphicsDevice);
			if (g_device != nullptr)
			{
				const RenderDeviceDescription& deviceDescription = g_device->getDescription();

#ifdef SHADOW_RECOMP_D3D12
				if (interfaceFunction == CreateD3D12Interface)
				{
					// TODO: This may be no longer be true with newer drivers
					// Hardware resolve seems to be completely bugged on Intel D3D12 drivers.
					g_hardwareResolve = (deviceDescription.vendor != RenderDeviceVendor::INTEL);
					g_hardwareDepthResolve = (deviceDescription.vendor != RenderDeviceVendor::INTEL);
				}

				g_vulkan = (interfaceFunction == CreateVulkanInterface);
#endif

				break;
			}
		}
#ifdef SHADOW_RECOMP_D3D12
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (graphicsApiRetry)
			{
				// If we were retrying, and this also failed, then we'll show the user neither of the graphics APIs succeeded.
				return false;
			}
			else
			{
				// If this is the first crash we ran into, reboot and try the other graphics API.
				os::process::StartProcess(os::process::GetExecutablePath(), { "--graphics-api-retry" });
				std::_Exit(0);
			}
		}
#endif
	}

	if (g_device == nullptr)
	{
#ifdef __ANDROID__
		// The driver stayed up but the renderer could not be created on it (an old Mali
		// failure, for example, is an allocation failure here). Noted as broken: the next
		// launch runs the other driver path.
		AndroidVideo::NoteRenderFailed("device");
#endif
		return false;
	}

#ifdef SHADOW_RECOMP_D3D12
	if (graphicsApiRetry)
	{
		// If we managed to create a device after retrying it in a reboot, remember the one we picked.
		Config::GraphicsAPI = g_vulkan ? EGraphicsAPI::Vulkan : EGraphicsAPI::D3D12;
	}
#endif

	g_capabilities = g_device->getCapabilities();

#ifdef __ANDROID__
	// What plume made of the driver, and of the driver's what the renderer has to adapt to.
	if (!AndroidVideo::CheckCreatedDevice(g_device.get()))
	{
		AndroidVideo::NoteRenderFailed("device");
		return false;
	}

	// Mobile GPUs differ a lot in what they can sample; record it for bug reports.
	AndroidVideo::LogDeviceSupport(g_device.get());
#endif

	STARTUP_PHASE("graphics driver loaded and device created");

#ifdef __ANDROID__
	// Before the first shader is created: a driver without 64-bit integers gets every one
	// of them rewritten.
	AndroidVideo::InitShaderRewrite(g_device.get());

	// Before the first pipeline is created: from here on the driver reuses what it compiled
	// in earlier runs.
	AndroidVideo::InitPipelineCache(g_device.get());
	STARTUP_PHASE("pipeline cache of the driver read");
#endif

	FixedFunctionVertexShaderVariants::Initialize(g_device.get(), g_vulkan);
	FixedFunctionPixelShaderVariants::Initialize(g_device.get(), g_vulkan);

	LoadEmbeddedResources();
	STARTUP_PHASE("embedded resources unpacked");

	constexpr uint64_t LowEndMemoryLimit = 2048ULL * 1024ULL * 1024ULL;
	RenderDeviceDescription deviceDescription = g_device->getDescription();
	bool lowEndType = deviceDescription.type != RenderDeviceType::UNKNOWN && deviceDescription.type != RenderDeviceType::DISCRETE;
	bool lowEndMemory = deviceDescription.dedicatedVideoMemory < LowEndMemoryLimit;
	bool lowEndUMA = deviceDescription.type == RenderDeviceType::UNKNOWN && g_capabilities.uma;

	const RenderSampleCounts colourSampleCount = g_device->getSampleCountsSupported(RenderFormat::R16G16B16A16_FLOAT);
	const RenderSampleCounts depthSampleCount = g_device->getSampleCountsSupported(DepthStencilFormat);
	const RenderSampleCounts commonSampleCount = colourSampleCount & depthSampleCount;

	g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);

	for (auto& commandList : g_commandLists)
		commandList = g_queue->createCommandList(); // ->createCommandList(RenderCommandListType::DIRECT);

	for (auto& commandFence : g_commandFences)
		commandFence = g_device->createCommandFence();

	for (auto& queryPool : g_queryPools)
		queryPool = g_device->createQueryPool(NUM_QUERIES);

	g_copyQueue = g_device->createCommandQueue(RenderCommandListType::COPY);
	g_copyCommandList = g_copyQueue->createCommandList();
	g_copyCommandFence = g_device->createCommandFence();

	// used for unitial resource (RT / Depth) as explained above
	g_initQueue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
	g_initCommandList = g_initQueue->createCommandList();
	g_initCommandFence = g_device->createCommandFence();

	uint32_t bufferCount = 2;

	switch (Config::TripleBuffering)
	{
	case ETripleBuffering::Auto:
		if (g_vulkan)
		{
			// Defaulting to 3 is fine if presentWait as supported, as the maximum frame latency allowed is only 1.
			bufferCount = g_device->getCapabilities().presentWait ? 3 : 2;
		}
		else
		{
			// Defaulting to 3 is fine on D3D12 thanks to flip discard model.
			bufferCount = 3;
		}

		break;
	case ETripleBuffering::On:
		bufferCount = 3;
		break;
	case ETripleBuffering::Off:
		bufferCount = 2;
		break;
	}

	RenderSwapChainDesc swapChainDesc;
	swapChainDesc.renderWindow = GameWindow::s_renderWindow;
	swapChainDesc.textureCount = bufferCount;
	swapChainDesc.format = SWAPCHAIN_FORMAT;
	swapChainDesc.maxFrameLatency = Config::MaxFrameLatency;
	swapChainDesc.enablePresentWait = g_capabilities.presentWait;

	g_swapChain = g_queue->createSwapChain(swapChainDesc);
	SetSwapChainVsync();
	g_swapChainValid = !g_swapChain->needsResize();
	STARTUP_PHASE("swap chain created");

	for (auto& acquireSemaphore : g_acquireSemaphores)
		acquireSemaphore = g_device->createCommandSemaphore();

	for (auto& renderSemaphore : g_renderSemaphores)
		renderSemaphore = g_device->createCommandSemaphore();

	RenderPipelineLayoutBuilder pipelineLayoutBuilder;
	pipelineLayoutBuilder.begin(false, true);

	RenderDescriptorSetBuilder descriptorSetBuilder;
	descriptorSetBuilder.begin();
	descriptorSetBuilder.addTexture(0, TEXTURE_DESCRIPTOR_SIZE);
	descriptorSetBuilder.end(true, TEXTURE_DESCRIPTOR_SIZE);

	g_textureDescriptorSet = descriptorSetBuilder.create(g_device.get());

	for (size_t i = 0; i < TEXTURE_DESCRIPTOR_NULL_COUNT; i++)
	{
		auto& texture = g_blankTextures[i];
		auto& textureView = g_blankTextureViews[i];

		RenderTextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.depth = 1;
		desc.mipLevels = 1;
		desc.format = RenderFormat::R8_UNORM;

		RenderTextureViewDesc viewDesc;
		viewDesc.format = desc.format;
		viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO);
		viewDesc.mipLevels = 1;

		switch (i)
		{
		case TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D:
			desc.dimension = RenderTextureDimension::TEXTURE_2D;
			desc.arraySize = 1;
			viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
			break;

		case TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D:
			desc.dimension = RenderTextureDimension::TEXTURE_3D;
			desc.arraySize = 1;
			viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
			break;

		case TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE:
			desc.dimension = RenderTextureDimension::TEXTURE_2D;
			desc.arraySize = 6;
			desc.flags = RenderTextureFlag::CUBE;
			viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
			break;

		default:
			assert(false && "Unknown null descriptor dimension");
			break;
		}

		texture = g_device->createTexture(desc);
		textureView = texture->createTextureView(viewDesc);

		g_textureDescriptorSet->setTexture(i, texture.get(), RenderTextureLayout::SHADER_READ, textureView.get());
	}

	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);
	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);
	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

	descriptorSetBuilder.begin();
	descriptorSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_SIZE);
	descriptorSetBuilder.end(true, SAMPLER_DESCRIPTOR_SIZE);

	g_samplerDescriptorSet = descriptorSetBuilder.create(g_device.get());
	auto& [descriptorIndex, sampler] = g_samplerStates[XXH3_64bits(&g_samplerDescs[0], sizeof(RenderSamplerDesc))];
	descriptorIndex = 1;
	sampler = g_device->createSampler(g_samplerDescs[0]);
	g_samplerDescriptorSet->setSampler(0, sampler.get());

	pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

	if (g_vulkan)
	{
		pipelineLayoutBuilder.addPushConstant(0, 4, PUSH_CONSTANTS_SIZE, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
	}
	else
	{
		pipelineLayoutBuilder.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
		pipelineLayoutBuilder.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
		pipelineLayoutBuilder.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
		pipelineLayoutBuilder.addPushConstant(3, 4, 4, RenderShaderStageFlag::PIXEL); // For copy/resolve shaders.
	}
	pipelineLayoutBuilder.end();

	g_pipelineLayout = pipelineLayoutBuilder.create(g_device.get());

	g_copyShader = CREATE_SHADER(copy_vs);
	g_copyColorShader = CREATE_SHADER(copy_color_ps);
	auto copyDepthShader = CREATE_SHADER(copy_depth_ps);

	RenderGraphicsPipelineDesc desc;
	desc.pipelineLayout = g_pipelineLayout.get();
	desc.vertexShader = g_copyShader.get();
	desc.pixelShader = copyDepthShader.get();
	desc.depthFunction = RenderComparisonFunction::ALWAYS;
	desc.depthEnabled = true;
	desc.depthWriteEnabled = true;
	desc.depthTargetFormat = DepthStencilFormat;

	// Left at its default this asks Vulkan for depth clamping, an optional feature that many
	// mobile GPUs do not have and whose pipelines their drivers then refuse. These passes
	// draw at depth 0, inside the range, so they look the same without it.
	desc.depthClipEnabled = true;
	g_copyDepthPipeline = g_device->createGraphicsPipeline(desc);
	STARTUP_PHASE("descriptor sets and copy pipeline created");

	CreateImGuiBackend();
	STARTUP_PHASE("interface fonts and textures loaded");

	auto gammaCorrectionShader = CREATE_SHADER(gamma_correction_ps);

	desc = {};
	desc.pipelineLayout = g_pipelineLayout.get();
	desc.vertexShader = g_copyShader.get();
	desc.pixelShader = gammaCorrectionShader.get();
	desc.renderTargetFormat[0] = SWAPCHAIN_FORMAT; // The presentation pass draws to the swap chain image.
	desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
	desc.renderTargetCount = 1;
	desc.depthClipEnabled = true; // See g_copyDepthPipeline.
	g_gammaCorrectionPipeline = g_device->createGraphicsPipeline(desc);

#ifdef __ANDROID__
	// Without these four not one frame can be shown. A driver that refuses one is reported
	// here, by the name of the pipeline, instead of crashing where it is first used.
	{
		const std::pair<const char*, const RenderPipeline*> startupPipelines[] =
		{
			{ "depth copy", g_copyDepthPipeline.get() },
			{ "interface", g_imPipeline.get() },
			{ "additive interface", g_imAdditivePipeline.get() },
			{ "presentation", g_gammaCorrectionPipeline.get() },
		};

		bool usable = true;
		for (const auto& [name, pipeline] : startupPipelines)
		{
			if (!AndroidVideo::IsPipelineUsable(pipeline))
			{
				LOGFN_ERROR("The graphics driver could not create the {} pipeline.", name);
				usable = false;
			}
		}

		if (!usable)
			return false;
	}
#endif

	FixedFuncVS = CREATE_SHADER(FixedFunctionVertexShader);
	FixedFuncPS = CREATE_SHADER(FixedFunctionPixelShader);
	PassthroughVS = CREATE_SHADER(PassthroughVertexShader);


	// NOTE: We initially allocate this on host memory to make the installer work, even if the 4 GB memory allocation fails.
	g_backBufferHolder = std::make_unique<GuestSurface>(ResourceType::RenderTarget);

	g_backBuffer = g_backBufferHolder.get();
	g_backBuffer->width = 640;
	g_backBuffer->height = 480;
	g_backBuffer->g_width = g_backBuffer->width;
	g_backBuffer->g_height = g_backBuffer->height;
	g_backBuffer->format = BACKBUFFER_FORMAT;
	g_backBuffer->textureHolder = g_device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, BACKBUFFER_FORMAT, RenderTextureFlag::RENDER_TARGET));

	DiscardInitRenderResource(g_backBuffer->textureHolder.get(), true);
	Video::ComputeViewportDimensions();
	CheckSwapChain(true);
	AspectRatioPatches::ComputeOffsets();
	BeginCommandList(true);

	RenderTextureBarrier blankTextureBarriers[TEXTURE_DESCRIPTOR_NULL_COUNT];
	for (size_t i = 0; i < TEXTURE_DESCRIPTOR_NULL_COUNT; i++)
		blankTextureBarriers[i] = RenderTextureBarrier(g_blankTextures[i].get(), RenderTextureLayout::SHADER_READ);

	g_commandLists[g_frame]->barriers(RenderBarrierStage::NONE, blankTextureBarriers, std::size(blankTextureBarriers));
	STARTUP_PHASE("first command list opened");

	return true;
}

static uint32_t g_waitForGPUCount = 0;

void Video::WaitForGPU()
{
	g_waitForGPUCount++;

	// Wait for all queued frames to finish.
	for (size_t i = 0; i < NUM_FRAMES; i++)
	{
		if (g_commandListStates[i])
		{
			g_queue->waitForCommandFence(g_commandFences[i].get());
			g_commandListStates[i] = false;
		}
	}

	// Execute an empty command list and wait for it to end to guarantee that any remaining presentation has finished.
	g_commandLists[0]->begin();
	g_commandLists[0]->end();
	g_queue->executeCommandLists(g_commandLists[0].get(), g_commandFences[0].get());
	g_queue->waitForCommandFence(g_commandFences[0].get());
}

static void ProcSetVertexDeclaration(const RenderCommand& cmd)
{
	auto& args = cmd.setVertexDeclaration;
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexDeclaration, args.vertexDeclaration);
}

GuestShader* CreateShader(ResourceType resourceType)
{
	auto base = g_memory.base;
	const bool isVertexShader = resourceType == ResourceType::VertexShader;
	uint32_t psDefReg = 0;

	if (isVertexShader == false && GetXboxPixelShader())
	{
		psDefReg = g_memory.MapVirtual(GetPixelShaderRenderStatePointer());
	}

	uint64_t hash = psDefReg > 0 ? GetPixelShaderKey(psDefReg) : GetVertexShaderKey();
	auto findCache = FindShaderCacheEntry(hash);

	GuestShader* shader = nullptr;

	if (findCache != nullptr)
	{
		if (findCache->guestShader == nullptr)
		{
			LOGFN("CreateShader: {} cache entry found but NO pre-compiled GuestShader hash={:016X} (creating new)",
				isVertexShader ? "VS" : "PS", hash);

			shader = g_userHeap.AllocPhysical<GuestShader>(resourceType);

			shader->shaderCacheEntry = findCache;
			shader->runtimeHash = hash;

			findCache->guestShader = shader;

			CompilePendingPipelinesForShader(hash, shader, isVertexShader);
		}
		else
		{
			shader = findCache->guestShader;
		}
	}


	if (shader)
	{
		shader->AddRef();
	}
	else 	// old system, will be used if shader_cache.cpp is missing
	{
		// B2B7A0A5E7D5805F
		// CEFA8AE484BFF7A2
		if (hash != 0xB2B7A0A5E7D5805FULL && hash != 0xCEFA8AE484BFF7A2ULL) {
			LOGFN("CreateShader: {} NOT in shader cache hash={:016X} (falling back to runtime compilation)",
				isVertexShader ? "VS" : "PS", hash);
		}

		shader = isVertexShader ? FindShaderCacheEntryNew() : FindPixelShaderCacheEntryNew(psDefReg);

		size_t hostVSHandle = (size_t)g_memory.Translate(g_VertexShader_Handle);

		if (shader != nullptr)
			shader->runtimeHash = hash;

		if (shader == nullptr)
		{
			auto runtimeStart = std::chrono::steady_clock::now();

			shader = g_userHeap.AllocPhysical<GuestShader>(resourceType);
			shader->runtimeHash = hash;

			if (resourceType == ResourceType::PixelShader)
			{
				size_t sizeShader = 0;

				X_D3DPIXELSHADERDEF* pXboxPixelShaderDef = (X_D3DPIXELSHADERDEF*)(g_memory.Translate(psDefReg));
				pixelShader_GuestInfo psdGuestInfo;
				memset(&psdGuestInfo, 0, sizeof(psdGuestInfo));
				psdGuestInfo.PSTextureModes = GetGuestRenderState(X_D3DRS_PSTEXTUREMODES);
				psdGuestInfo.renderstateFogEnable = GetGuestRenderState(X_D3DRS_FOGENABLE) > 0;
				psdGuestInfo.renderStateSpecularEnable = GetGuestRenderState(X_D3DRS_SPECULARENABLE) > 0;

				for (int i = 0; i < 4; i++)
				{
					if (g_texturesPSDef[i])
					{
						unsigned long type = (unsigned long)GetXboxTextureType((X_D3DResource*)((size_t)&g_texturesPSDef[i]->guestResource));
						psdGuestInfo.ActiveTextureTypes[i] = (uint32_t)type;
					}
				}

				auto shaderData = CreateRecompiledPixelShader(pXboxPixelShaderDef, &psdGuestInfo, &sizeShader);
				if (shaderData != nullptr)
				{
					shader->shader = CREATE_RUNTIME_SHADER(shaderData, sizeShader);
				}
				else
				{
					// No runtime shader compiler in this build (Android): draw with the generic
					// fixed-function shader rather than crash on the missing one.
					LOGFN_ERROR("CreateShader: PS hash={:016X} is missing from the shader cache and cannot be compiled at runtime.", hash);
					shader->shader = CREATE_SHADER(FixedFunctionPixelShader);
				}

				UpdatePixelShaderCacheEntryNew(shader, hash);
			}
			else
			{

				if (hash == 0xb2b7a0a5e7d5805f)
				{
					shader->shader = CREATE_SHADER(R2D_vs0);
				}
				else if (hash == 0xcefa8ae484bff7a2)
				{
					shader->shader = CREATE_SHADER(R2D_vs1);
				}
				else
				{
					size_t sizeShader = 0;
					auto shaderData = CreateRecompiledShader(hostVSHandle, &sizeShader);
					if (shaderData != nullptr)
					{
						shader->shader = CREATE_RUNTIME_SHADER(shaderData, sizeShader);
					}
					else
					{
						// See the pixel shader branch above.
						LOGFN_ERROR("CreateShader: VS hash={:016X} is missing from the shader cache and cannot be compiled at runtime.", hash);
						shader->shader = CREATE_SHADER(FixedFunctionVertexShader);
					}
				}

				UpdateShaderCacheEntryNew(shader, hash);
			}

			auto runtimeElapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runtimeStart).count();
			LOGFN("CreateShader: {} RUNTIME compilation hash={:016X} -> {:.2f} ms",
				isVertexShader ? "VS" : "PS", hash, runtimeElapsed);
		}
	}

	if (shader != nullptr && shader->shaderCacheEntry == nullptr && shader->runtimeHash != 0)
		CompilePendingPipelinesForShader(shader->runtimeHash, shader, isVertexShader);

	return shader;
}
// Get viewport offset and scale values intended to match the XDK calculations
void GetXboxViewportOffsetAndScale(float(&vOffset)[4], float(&vScale)[4])
{
	// Antialiasing mode affects the viewport offset and scale
	float aaScaleX = 1.0f;
	float aaScaleY = 1.0f;
	float aaOffsetX = 0.0f;
	float aaOffsetY = 0.0f;
	//GetScreenScaleFactors(aaScaleX, aaScaleY); //todo check if this is needed

	auto g_viewport = g_gameViewport;

	float scaledX = g_viewport.x * aaScaleX;
	float scaledY = g_viewport.y * aaScaleY;
	float scaledWidth = g_viewport.width * aaScaleX;
	float scaledHeight = g_viewport.height * aaScaleY;

	// D3D9 viewport info
	// https://docs.microsoft.com/en-us/windows/win32/direct3d9/viewports-and-clipping
	// Unlike D3D9, Xbox should scale by the z buffer scale

	auto zRange = g_viewport.maxDepth - g_viewport.minDepth;
	vScale[0] = scaledWidth / 2;
	vScale[1] = -scaledHeight / 2;
	vScale[2] = zRange * g_ZScale;
	vScale[3] = 1;
	vOffset[0] = scaledWidth / 2 + scaledX;
	vOffset[1] = scaledHeight / 2 + scaledY;
	vOffset[2] = g_viewport.minDepth * g_ZScale;
	vOffset[3] = 0;
}


void UpdateHostViewPortOffsetAndScaleConstants()
{
	float vScaleOffset[2][4]; // 0 - scale 1 - offset
	GetXboxViewportOffsetAndScale(vScaleOffset[1], vScaleOffset[0]);

	// Xbox outputs vertex positions in rendertarget pixel coordinate space, with non-normalized Z
	// e.g. 0 < x < 640 and 0 < y < 480
	// We want to scale it back to normalized device coordinates i.e. XY are (-1, +1) and Z is (0, 1)

	// The screenspace is a combination of the rendertarget
	// and various scale factors
	// Get the rendertarget width and height
	float xboxRenderTargetWidth = g_next_RenderTarget->g_width;
	float xboxRenderTargetHeight = g_next_RenderTarget->g_height;
	//GetRenderTargetBaseDimensions(xboxRenderTargetWidth, xboxRenderTargetHeight);

	float screenScaleX = 1.0f;
	float screenScaleY = 1.0f;
	float aaOffsetX = 0.0f; // used to be 0.5 but this creates visible white bar at the bottom and right side of the screen 
	float aaOffsetY = 0.0f;

	//GetScreenScaleFactors(screenScaleX, screenScaleY);
	//GetMultiSampleOffset(aaOffsetX, aaOffsetY);


	float xboxScreenspaceWidth = xboxRenderTargetWidth * screenScaleX;
	float xboxScreenspaceHeight = xboxRenderTargetHeight * screenScaleY;

	// Passthrough should range 0 to 1, instead of 0 to zbuffer depth
	float zOutputScale = g_Xbox_VertexShaderMode == VertexShaderMode::Passthrough ? 1 : g_ZScale;

	float screenspaceScale[4] = { xboxScreenspaceWidth / 2,  -xboxScreenspaceHeight / 2, zOutputScale, 1 };
	float screenspaceOffset[4] = { xboxScreenspaceWidth / 2 + aaOffsetX, xboxScreenspaceHeight / 2 + aaOffsetY, 0, 0 };

	if (memcmp(&vsh_constants[HOST_D3DVS_SCREENSPACE_SCALE_BASE][0], screenspaceScale, sizeof(screenspaceScale)) != 0)
		CopyVertexShaderConstant(HOST_D3DVS_SCREENSPACE_SCALE_BASE, (DWORD*)screenspaceScale, HOST_D3DVS_NORMALIZE_SCALE_SIZE);
	if (memcmp(&vsh_constants[HOST_D3DVS_SCREENSPACE_OFFSET_BASE][0], screenspaceOffset, sizeof(screenspaceOffset)) != 0)
		CopyVertexShaderConstant(HOST_D3DVS_SCREENSPACE_OFFSET_BASE, (DWORD*)screenspaceOffset, HOST_D3DVS_NORMALIZE_OFFSET_SIZE);
	// Store viewport offset and scale in constant registers 58 (c-38) and
	// 59 (c-37) used for screen space transformation.
	// We only do this if X_D3DSCM_NORESERVEDCONSTANTS is not set,
	// since enabling this flag frees up these registers for shader use
	if (!(g_Xbox_VertexShaderConstantMode & X_D3DSCM_NORESERVEDCONSTANTS))
	{
		if (memcmp(&vsh_constants[X_D3DSCM_RESERVED_CONSTANT_SCALE_CORRECTED][0], vScaleOffset, sizeof(vScaleOffset)) != 0)
			CopyVertexShaderConstant(X_D3DSCM_RESERVED_CONSTANT_SCALE_CORRECTED, reinterpret_cast<DWORD*>(vScaleOffset), 2);
	}
}

static void ProcSetVertexShader(const RenderCommand& cmd)
{

	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShaderType, cmd.setVertexShader.type);
	const bool isFixed = cmd.setVertexShader.type == VertexShaderMode::FixedFunction;

	size_t host_handle = (size_t)g_memory.Translate(cmd.setVertexShader.handle);

	if (isFixed)
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShader, (GuestShader*)host_handle);
	}
	else
	{
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShader, cmd.setVertexShader.shader);
	}

}


static void ProcSetPixelShader(const RenderCommand& cmd)
{
	GuestShader* shader = cmd.setPixelShader.shader;
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.pixelShader, shader);
	bool fixedFunctionChanged = false;
	SetDirtyValue(fixedFunctionChanged, g_pipelineState.isFixedFunctionPS, cmd.setPixelShader.isFixed);
	g_dirtyStates.pipelineState |= fixedFunctionChanged;
	if (fixedFunctionChanged && cmd.setPixelShader.isFixed)
		g_fixedFunctionPixelShaderStateDirty = true;
}


static void ProcSetStreamSource(const RenderCommand& cmd)
{
	const auto& args = cmd.setStreamSource;

	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[args.StreamNumber], uint8_t(args.vertexBuffer != nullptr ? args.stride : 0));

	bool dirty = false;

	SetDirtyValue(dirty, g_vertexBufferViews[args.StreamNumber].buffer, args.vertexBuffer != nullptr ? args.vertexBuffer->buffer->at(0) : RenderBufferReference{});
	SetDirtyValue(dirty, g_vertexBufferViews[args.StreamNumber].size, args.vertexBuffer != nullptr ? (args.vertexBuffer->dataSize - 0) : 0u);
	SetDirtyValue(dirty, g_inputSlots[args.StreamNumber].stride, args.vertexBuffer != nullptr ? args.stride : 0u);

	if (dirty)
	{
		g_dirtyStates.vertexStreamFirst = std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, args.StreamNumber);
		g_dirtyStates.vertexStreamLast = std::max<uint8_t>(g_dirtyStates.vertexStreamLast, args.StreamNumber);
	}
}

// dynamic vertex buffer for particles / matFX stuff, rebuild every frame from RenderWare pipeline.
static void ProcSetStreamSourceUP(const RenderCommand& cmd)
{
	const auto& args = cmd.setStreamSourceUP;

	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[args.StreamNumber], uint8_t(args.stride));

	auto allocation = g_uploadAllocators[g_frame].allocate(reinterpret_cast<const uint32_t*>(args.vertexData), args.size, 0x4);

	g_vertexBufferViews[args.StreamNumber].buffer = allocation.buffer->at(allocation.offset);
	g_vertexBufferViews[args.StreamNumber].size = args.size;
	g_inputSlots[args.StreamNumber].stride = args.stride;

	g_dirtyStates.vertexStreamFirst = std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, args.StreamNumber);
	g_dirtyStates.vertexStreamLast = std::max<uint8_t>(g_dirtyStates.vertexStreamLast, args.StreamNumber);
}

static const char* DeviceTypeName(RenderDeviceType type)
{
	switch (type)
	{
	case RenderDeviceType::INTEGRATED:
		return "Integrated";
	case RenderDeviceType::DISCRETE:
		return "Discrete";
	case RenderDeviceType::VIRTUAL:
		return "Virtual";
	case RenderDeviceType::CPU:
		return "CPU";
	default:
		return "Unknown";
	}
}

static const char* PresentModeName(VkPresentModeKHR mode)
{
	switch (mode)
	{
	case VK_PRESENT_MODE_IMMEDIATE_KHR:
		return "Immediate";
	case VK_PRESENT_MODE_MAILBOX_KHR:
		return "Mailbox";
	case VK_PRESENT_MODE_FIFO_KHR:
		return "FIFO";
	default:
		return "Unknown";
	}
}

static std::atomic<uint32_t> g_bufferUploadCount = 0;

static void DrawProfiler()
{
	bool toggleProfiler = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1] != 0;

	if (!g_profilerWasToggled && toggleProfiler)
	{
		g_profilerVisible = !g_profilerVisible;

		GameWindow::SetFullscreenCursorVisibility(App::s_isInit ? g_profilerVisible : true);
	}

	g_profilerWasToggled = toggleProfiler;
	g_detailedProfilerEnabled.store(g_profilerVisible, std::memory_order_relaxed);

	if (!g_profilerVisible)
		return;

	ImFont* font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-DB.otf"); //ImGui::GetIO().Fonts->Fonts[0];

	float defaultScale = font->Scale;
	font->Scale = (ImGui::GetDefaultFont()->FontSize / font->FontSize) * Config::ProfilerFontScale.Value;
	ImGui::PushFont(font);

#define IMGUI_GENERIC_ROW(name, value, ...) \
	ImGui::TableNextColumn(); \
	ImGui::Text(name); \
	ImGui::TableNextColumn(); \
	ImGui::Text(value, __VA_ARGS__);

	if (ImGui::Begin("Profiler", &g_profilerVisible))
	{
		g_applicationValues[g_profilerValueIndex] = g_frameIntervalMs;

		const double applicationAvg = std::accumulate(g_applicationValues, g_applicationValues + PROFILER_VALUE_COUNT, 0.0) / PROFILER_VALUE_COUNT;
		double gpuFrameAvg = g_gpuFrameProfiler.UpdateAndReturnAverage();
		double presentAvg = g_presentProfiler.UpdateAndReturnAverage();
		double gameThreadAvg = g_updateDirectorProfiler.UpdateAndReturnAverage();
		double renderThreadWorkAvg = g_renderDirectorProfiler.UpdateAndReturnAverage();
		double frameFenceAvg = g_frameFenceProfiler.UpdateAndReturnAverage();
		double presentWaitAvg = g_presentWaitProfiler.UpdateAndReturnAverage();
		double swapChainAcquireAvg = g_swapChainAcquireProfiler.UpdateAndReturnAverage();
		double frameEndWaitAvg = g_frameEndWaitProfiler.UpdateAndReturnAverage();
		double rtPsoWorkAvg = g_rtPsoWorkAccum.UpdateAndReturnAverage();
		double rtFlushWorkAvg = g_rtFlushWorkAccum.UpdateAndReturnAverage();
		double rtDrawCallAvg = g_rtDrawCallAccum.UpdateAndReturnAverage();
		double rtOtherCmdsAvg = g_rtOtherCmdsAccum.UpdateAndReturnAverage();
		double rtBeginCmdListWorkAvg = g_rtBeginCmdListWorkAccum.UpdateAndReturnAverage();
		double rtDrawHandlerAvg = g_rtDrawHandlerAccum.UpdateAndReturnAverage();
		double rtQueueWaitAvg = g_rtQueueWaitAccum.UpdateAndReturnAverage();
		double rtQueueWaitInFrameAvg = g_rtQueueWaitInFrameAccum.UpdateAndReturnAverage();
		double gtDrawRecordingAvg = g_gtDrawRecordingAccum.UpdateAndReturnAverage();
		double gtStateFlushAvg = g_gtStateFlushAccum.UpdateAndReturnAverage();

		if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImPlot::BeginPlot("Frame Time"))
			{
				ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 20.0);
				ImPlot::SetupAxis(ImAxis_Y1, "ms", ImPlotAxisFlags_None);
				ImPlot::PlotLine<double>("Application", g_applicationValues, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("GPU Frame", g_gpuFrameProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Present", g_presentProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Game Thread", g_updateDirectorProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Render-Thread Work", g_renderDirectorProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Present Wait", g_presentWaitProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Frame Fence", g_frameFenceProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Swap Chain Acquire", g_swapChainAcquireProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::PlotLine<double>("Frame End Wait", g_frameEndWaitProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
				ImPlot::EndPlot();
			}

			if (ImGui::Button("Copy Stats"))
			{
				double psoCur = g_rtPsoWorkAccum.value.load();
				double flushCur = g_rtFlushWorkAccum.value.load();
				double drawHandlerCur = g_rtDrawHandlerAccum.value.load();
				double drawCallCur = g_rtDrawCallAccum.value.load();
				double queueWaitCur = g_rtQueueWaitAccum.value.load();
				std::string text = fmt::format(
					"AVERAGES:\n"
					"Application          {:.4g} ms\n"
					"GPU Frame            {:.4g} ms\n"
					"Present              {:.4g} ms\n"
					"Game Thread          {:.4g} ms\n"
					"Render-Thread Work   {:.4g} ms\n"
					"Present Wait         {:.4g} ms\n"
					"Frame Fence          {:.4g} ms\n"
					"Swap Chain Acquire   {:.4g} ms\n"
					"Frame End Wait       {:.4g} ms\n"
					"\n"
					"RENDER-THREAD BREAKDOWN (averages):\n"
					"PSO Work             {:.4g} ms\n"
					"Per-Draw Flush       {:.4g} ms\n"
					"  (excl. PSO)        {:.4g} ms\n"
					"Draw Calls (API)     {:.4g} ms\n"
					"Draw Handlers (tot)  {:.4g} ms\n"
					"  Draw Pre-Work      {:.4g} ms\n"
					"Other Cmds           {:.4g} ms\n"
					"Begin CmdList Work   {:.4g} ms\n"
					"Queue Wait (starved) {:.4g} ms\n"
					"\n"
					"COMMAND COUNTS (last frame):\n"
					"Draw Calls            {}\n"
					"State Changes         {}\n"
					"Total Commands        {}\n"
					"PSO per Draw          {:.2f} us\n"
					"API Call per Draw     {:.2f} us\n"
					"PSO Cache Hits        {} / {} ({:.1f}%)\n"
					"\n"
					"GAME-THREAD BREAKDOWN (averages):\n"
					"Draw Recording        {:.4g} ms\n"
					"  State Flush         {:.4g} ms\n"
					"Present (in GT)       {:.4g} ms\n"
					"Game Logic            {:.4g} ms\n"
					"\n"
					"CURRENT:\n"
					"Application          {:.4g} ms ({:.1f} FPS)\n"
					"GPU Frame            {:.4g} ms ({:.1f} FPS)\n"
					"Render-Thread Work   {:.4g} ms\n"
					"PSO Work (current)   {:.4g} ms\n"
					"Per-Draw Flush (cur) {:.4g} ms\n"
					"Draw Handlers (cur)  {:.4g} ms\n"
					"  Draw Pre-Work(cur) {:.4g} ms\n"
					"Draw Calls (cur)     {:.4g} ms\n"
					"Other Cmds (cur)     {:.4g} ms\n"
					"Queue Wait (cur)     {:.4g} ms\n"
					"Draw Recording (cur) {:.4g} ms\n"
					"Present (in GT, cur) {:.4g} ms\n"
					"Game Logic (cur)     {:.4g} ms\n",
					applicationAvg, gpuFrameAvg, presentAvg, gameThreadAvg, renderThreadWorkAvg,
					presentWaitAvg, frameFenceAvg, swapChainAcquireAvg, frameEndWaitAvg,
					rtPsoWorkAvg, rtFlushWorkAvg, std::max(rtFlushWorkAvg - rtPsoWorkAvg, 0.0), rtDrawCallAvg,
					rtDrawHandlerAvg, std::max(rtDrawHandlerAvg - rtFlushWorkAvg - rtDrawCallAvg, 0.0),
					rtOtherCmdsAvg, rtBeginCmdListWorkAvg, rtQueueWaitAvg,
					g_drawCallCountSnapshot, g_stateChangeCountSnapshot, g_totalCommandCountSnapshot,
					g_drawCallCountSnapshot > 0 ? rtPsoWorkAvg * 1000.0 / g_drawCallCountSnapshot : 0.0,
					g_drawCallCountSnapshot > 0 ? rtDrawCallAvg * 1000.0 / g_drawCallCountSnapshot : 0.0,
					g_psoCacheHitsSnapshot, g_psoCacheHitsSnapshot + g_psoCacheMissesSnapshot,
					(g_psoCacheHitsSnapshot + g_psoCacheMissesSnapshot) > 0 ? 100.0 * g_psoCacheHitsSnapshot / (g_psoCacheHitsSnapshot + g_psoCacheMissesSnapshot) : 0.0,
					gtDrawRecordingAvg, gtStateFlushAvg, presentAvg, std::max(gameThreadAvg - gtDrawRecordingAvg - presentAvg, 0.0),
					g_frameIntervalMs, 1000.0 / std::max(g_frameIntervalMs, 0.001),
					g_gpuFrameProfiler.value.load(), (g_gpuFrameProfiler.value.load() > 0.0 ? 1000.0 / g_gpuFrameProfiler.value.load() : 0.0),
					g_renderDirectorProfiler.value.load(),
					psoCur, flushCur, drawHandlerCur, std::max(drawHandlerCur - flushCur - drawCallCur, 0.0), drawCallCur, g_rtOtherCmdsAccum.value.load(), queueWaitCur,
					g_gtDrawRecordingAccum.value.load(), g_presentProfiler.value.load(), std::max(g_updateDirectorProfiler.value.load() - g_gtDrawRecordingAccum.value.load() - g_presentProfiler.value.load(), 0.0));
				ImGui::SetClipboardText(text.c_str());
			}

			g_profilerValueIndex = (g_profilerValueIndex + 1) % PROFILER_VALUE_COUNT;

			if (ImGui::BeginTable("Performance", 5))
			{
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Current Time");
				ImGui::TableSetupColumn("Average Time");
				ImGui::TableSetupColumn("Current FPS");
				ImGui::TableSetupColumn("Average FPS");
				ImGui::TableHeadersRow();

				// Indentation = nesting. "(wait)" = blocked. "(async)" = other worker.
				auto drawPerfRow = [&](const char* name, double ms, double msAvg, bool showFPS = false, const char* note = nullptr)
					{
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(name);

						if (note != nullptr && ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", note);

						ImGui::TableNextColumn();
						ImGui::Text("%.3f ms (%.1f%%)", ms, g_frameIntervalMs > 0.0 ? ms / g_frameIntervalMs * 100.0 : 0.0);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f ms (%.1f%%)", msAvg, applicationAvg > 0.0 ? msAvg / applicationAvg * 100.0 : 0.0);
						ImGui::TableNextColumn();

						if (showFPS)
							ImGui::Text("%.3f FPS", 1000.0 / std::max(ms, 0.001));
						else
							ImGui::TextUnformatted("N/A");

						ImGui::TableNextColumn();

						if (showFPS)
							ImGui::Text("%.3f FPS", 1000.0 / std::max(msAvg, 0.001));
						else
							ImGui::TextUnformatted("N/A");
					};

				// Gray group label row; the value columns stay empty.
				auto drawPerfGroup = [&](const char* label, const char* note = nullptr)
					{
						ImGui::TableNextColumn();
						ImGui::TextDisabled("%s", label);

						if (note != nullptr && ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", note);

						ImGui::TableNextColumn();
						ImGui::TableNextColumn();
						ImGui::TableNextColumn();
						ImGui::TableNextColumn();
					};

				// Percentages are shares of the frame period (Application).
				drawPerfRow("Application", g_frameIntervalMs, applicationAvg, true, "Whole frame period, wall clock, Present to Present.\n\nThe budget: compare the Work rows below against it.\nWall-clock thread totals are in Copy Stats.");
				drawPerfGroup("Work (non-idle)",
					"Busy time only: what each stage actually spends working.\n\nThe biggest Work row against Application is the bottleneck.");
				drawPerfRow("    Game Thread", std::max(g_updateDirectorProfiler.value.load() - g_presentProfiler.value.load(), 0.0), std::max(gameThreadAvg - presentAvg, 0.0), true, "Game logic plus draw recording.\n\nThe CPU work that builds the frame; its waiting lives in the Idle rows below.");
				drawPerfRow("    Render Thread (async)", std::max(g_renderDirectorProfiler.value.load() - g_rtQueueWaitInFrameAccum.value.load(), 0.0), std::max(renderThreadWorkAvg - rtQueueWaitInFrameAvg, 0.0), true, "The render thread translating game commands into GPU API calls,\nwith its idle queue waits removed.\n\nRuns alongside the game thread; the wall-clock total is in Copy Stats.");
				drawPerfRow("    GPU (async)", g_gpuFrameProfiler.value.load(), gpuFrameAvg, true, "Time the GPU spends drawing the frame.\n\nMeasured with GPU timestamp queries; runs on its own clock,\noverlaps the frame period.");
				drawPerfGroup("Idle",
					"Waiting, not work: deliberate pacing or one stage blocked on another.\n\nIt does not count toward the Work rows above.");
				drawPerfGroup("    Game Thread");
				drawPerfRow("        Present", g_presentProfiler.value.load(), presentAvg, false, "Video::Present on the game thread: draws the overlay UI and\nqueues the frame end for the render thread.\n\nThe (wait) row below is the time it was blocked.");
				drawPerfRow("            Frame End (wait)", g_frameEndWaitProfiler.value.load(), frameEndWaitAvg, false, "Blocked waiting for the render thread to finish\na frame end.\n\nNear zero unless the render thread is the bottleneck,\nor the game thread waits on purpose (VSync, GPU bound).");
				drawPerfGroup("    Render Thread");
				drawPerfRow("        Frame Fence (wait)", g_frameFenceProfiler.value.load(), frameFenceAvg, false, "Blocked waiting for the GPU to finish an older frame\n(backpressure from the buffered-frame pipeline).");
				drawPerfRow("        Swap Chain Acquire", g_swapChainAcquireProfiler.value.load(), swapChainAcquireAvg, false, "Acquiring a free backbuffer image from the swap chain.");
				drawPerfRow("        Present Wait (VSync)", g_presentWaitProfiler.value.load(), presentWaitAvg, false, "VSync pacing: waiting on the swap chain between frames\n(Vsync ON FIFO only).");
				drawPerfRow("        Queue Wait", g_rtQueueWaitInFrameAccum.value.load(), rtQueueWaitInFrameAvg, false, "Starved render thread: commands exhausted, waiting for the\ngame thread to send more.\n\nSame value as Queue Wait (idle) in the Render-Thread Breakdown.");

				ImGui::EndTable();
			}

			if (ImGui::CollapsingHeader("Render-Thread Breakdown"))
			{
				if (ImGui::BeginTable("RT Breakdown", 4))
				{
					ImGui::TableSetupColumn("Name");
					ImGui::TableSetupColumn("Current");
					ImGui::TableSetupColumn("Average");
					ImGui::TableSetupColumn("% of Render Thread Work");
					ImGui::TableHeadersRow();

					auto drawRtRow = [&](const char* name, double cur, double avg, const char* note = nullptr)
						{
							double rtCur = g_renderDirectorProfiler.value.load();
							double rtAvg = renderThreadWorkAvg;
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(name);

							if (note != nullptr && ImGui::IsItemHovered())
								ImGui::SetTooltip("%s", note);

							ImGui::TableNextColumn();
							ImGui::Text("%.3f ms", cur);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f ms", avg);
							ImGui::TableNextColumn();
							ImGui::Text("%.1f%% / %.1f%%", rtCur > 0.0 ? (cur / rtCur * 100.0) : 0.0, rtAvg > 0.0 ? (avg / rtAvg * 100.0) : 0.0);
						};

					// Gray group label row; the value columns stay empty.
					auto drawRtGroup = [&](const char* label)
						{
							ImGui::TableNextColumn();
							ImGui::TextDisabled("%s", label);
							ImGui::TableNextColumn();
							ImGui::TableNextColumn();
							ImGui::TableNextColumn();
						};

					// Roughly one frame on the render thread, in order: open the command
					// list, apply state changes, run the draws. Idle time is grouped last.
					double psoCur = g_rtPsoWorkAccum.value.load();
					double flushCur = g_rtFlushWorkAccum.value.load();
					double drawHandlerCur = g_rtDrawHandlerAccum.value.load();
					double drawCallCur = g_rtDrawCallAccum.value.load();
					drawRtGroup("Work (non-idle)");
					drawRtRow("Begin CmdList Work", g_rtBeginCmdListWorkAccum.value.load(), rtBeginCmdListWorkAvg,
						"Resetting the command list for the next frame and freeing\n"
						"one-frame resources.\n\n"
						"Runs outside the main render-thread timer, so it comes on top\n"
						"of Render-Thread Work.");
					drawRtRow("Other Cmds", g_rtOtherCmdsAccum.value.load(), rtOtherCmdsAvg,
						"Every command that is not a draw: texture and shader binds,\n"
						"constant uploads, clears, viewport changes, cleanup.\n\n"
						"High values mean too many small state changes between draws.");
					// Draw Handlers total is inclusive of pre-work + FlushWork + DrawCall.
					drawRtRow("Draw Handlers (total)", drawHandlerCur, rtDrawHandlerAvg,
						"All time spent handling draw commands on the render thread.\n"
						"Covers setup, state flush and the driver draw call.\n\n"
						"Split into the three rows below.");
					drawRtRow("  Draw Pre-Work", std::max(drawHandlerCur - flushCur - drawCallCur, 0.0),
						std::max(rtDrawHandlerAvg - rtFlushWorkAvg - rtDrawCallAvg, 0.0),
						"Draw handler time minus flush and driver draw call.\n\n"
						"Leftover per-draw handling: primitive conversion and small\n"
						"checks before the draw goes out.");
					// FlushWork is inclusive of PSO work.
					drawRtRow("  Per-Draw Flush", flushCur, rtFlushWorkAvg,
						"Applying everything that changed before a draw can run:\n"
						"barriers, multisample (MSAA) resolves, bindings, constants.\n\n"
						"Includes PSO Work; the rows below split it out.");
					drawRtRow("    PSO Work", psoCur, rtPsoWorkAvg,
						"PSO = Pipeline State Object: the complete GPU setup for one draw,\n"
						"including shaders, blend and depth handling.\n"
						"This row is the cache lookup for that setup, plus building it on a miss.\n\n"
						"High values mean cache misses or first-time shader builds.");
					drawRtRow("    (excl. PSO)", std::max(flushCur - psoCur, 0.0), std::max(rtFlushWorkAvg - rtPsoWorkAvg, 0.0),
						"Per-Draw Flush with the PSO time removed.\n\n"
						"Isolates the cost of applying state from pipeline lookups.");
					drawRtRow("  Draw Calls (API)", drawCallCur, rtDrawCallAvg,
						"Time inside the actual GPU driver draw call.\n\n"
						"High here with many draws means the driver call itself is\n"
						"expensive, often from frequent state changes between draws.");
					drawRtGroup("Idle");
					drawRtRow("Queue Wait (idle)", g_rtQueueWaitInFrameAccum.value.load(), rtQueueWaitInFrameAvg,
						"Idle while the frame is open: the render thread ran out of\n"
						"commands and waits for the game thread to send more.\n\n"
						"Not real work. High values mean the render thread is starved\n"
						"and the bottleneck is earlier in the pipeline.\n"
						"Waits while the next command list is still closed are excluded.");
					// Begin CmdList Work runs OUTSIDE the g_renderDirectorProfiler bracket.
					double totalAcctCur = flushCur + drawCallCur + g_rtOtherCmdsAccum.value.load() + g_rtBeginCmdListWorkAccum.value.load() + g_rtQueueWaitInFrameAccum.value.load();
					double totalAcctAvg = rtFlushWorkAvg + rtDrawCallAvg + rtOtherCmdsAvg + rtBeginCmdListWorkAvg + rtQueueWaitInFrameAvg;
					ImGui::TableNextColumn(); ImGui::TextDisabled("Total Acct.");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", "Sum of the rows above: flush, draws, other commands,\ncommand list open and queue wait.\n\nCompare with Render-Thread Work to see how much of the\nrender thread's time is explained.");
					ImGui::TableNextColumn(); ImGui::TextDisabled("%.3f ms", totalAcctCur);
					ImGui::TableNextColumn(); ImGui::TextDisabled("%.3f ms", totalAcctAvg);
					ImGui::TableNextColumn(); ImGui::TextDisabled("(vs Render-Thread Work: %.3f ms)", renderThreadWorkAvg);

					ImGui::EndTable();
				}
				ImGui::Separator();
				ImGui::TextUnformatted("Per-Frame Command Counts");
				ImGui::SameLine();
				ImGui::TextDisabled("(last frame)");
				{
					double psoPerDraw = g_drawCallCountSnapshot > 0 ? g_rtPsoWorkAccum.value.load() / g_drawCallCountSnapshot : 0.0;
					double drawApiPerDraw = g_drawCallCountSnapshot > 0 ? g_rtDrawCallAccum.value.load() / g_drawCallCountSnapshot : 0.0;
					ImGui::Text("  Draw Calls:        %u", g_drawCallCountSnapshot);
					ImGui::Text("  State Changes:     %u", g_stateChangeCountSnapshot);
					ImGui::Text("  Total Commands:    %u", g_totalCommandCountSnapshot);
					ImGui::Text("  PSO / Draw:        %.2f us", psoPerDraw * 1000.0);
					ImGui::Text("  API Call / Draw:   %.2f us", drawApiPerDraw * 1000.0);
					uint32_t psoTotal = g_psoCacheHitsSnapshot + g_psoCacheMissesSnapshot;
					ImGui::Text("  PSO Cache Hits:    %u / %u (%.1f%%)", g_psoCacheHitsSnapshot, psoTotal,
						psoTotal > 0 ? 100.0 * g_psoCacheHitsSnapshot / psoTotal : 0.0);
				}
			}

			if (ImGui::CollapsingHeader("Game-Thread Breakdown"))
			{
				if (ImGui::BeginTable("GT Breakdown", 4))
				{
					ImGui::TableSetupColumn("Name");
					ImGui::TableSetupColumn("Current");
					ImGui::TableSetupColumn("Average");
					ImGui::TableSetupColumn("% of Game Thread");
					ImGui::TableHeadersRow();

					auto drawGtRow = [&](const char* name, double cur, double avg, const char* note = nullptr)
						{
							double gtCur = g_updateDirectorProfiler.value.load();
							double gtAvg = gameThreadAvg;
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(name);

							if (note != nullptr && ImGui::IsItemHovered())
								ImGui::SetTooltip("%s", note);

							ImGui::TableNextColumn();
							ImGui::Text("%.3f ms", cur);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f ms", avg);
							ImGui::TableNextColumn();
							ImGui::Text("%.1f%% / %.1f%%", gtCur > 0.0 ? (cur / gtCur * 100.0) : 0.0, gtAvg > 0.0 ? (avg / gtAvg * 100.0) : 0.0);
						};

					double drawRecCur = g_gtDrawRecordingAccum.value.load();
					double stateFlushCur = g_gtStateFlushAccum.value.load();
					double presentCur = g_presentProfiler.value.load();
					double gameLogicCur = std::max(g_updateDirectorProfiler.value.load() - drawRecCur - presentCur, 0.0); // Game Thread includes present, so subtract (sub_57630)
					double gameLogicAvg = std::max(gameThreadAvg - gtDrawRecordingAvg - presentAvg, 0.0);
					// Roughly one frame on the game thread, in order: run the game logic,
					// record draws, then hand the frame over (mostly waiting).
					drawGtRow("Game Logic", gameLogicCur, gameLogicAvg,
						"Everything else the game does in a frame: simulation, scripts,\n"
						"animation, collision.\n\n"
						"Computed as the wall-clock Game Thread total minus Draw Recording minus Present.");
					drawGtRow("Draw Recording", drawRecCur, gtDrawRecordingAvg,
						"The game thread converting its draw calls into commands for the\n"
						"render thread: vertex data copies, vertex patching, queueing.\n\n"
						"Runs while the game builds the frame. Includes State Flush.");
					drawGtRow("  State Flush", stateFlushCur, gtStateFlushAvg,
						"Sending the settings that changed since the last draw:\n"
						"textures, shaders, constants, blend modes.");
					drawGtRow("Present", presentCur, presentAvg,
						"Handing the finished frame over: waiting for the render thread\n"
						"and the GPU, showing the image, then pacing to the target FPS.\n\n"
						"Mostly waiting, not CPU work.\n"
						"The (wait) rows under Present in the main table show which\n"
						"side it waited on.");
					ImGui::EndTable();
				}
			}
		}

		if (ImGui::CollapsingHeader("Configuration", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextUnformatted("Profiler Font Size");
			ImGui::PushItemWidth(-1);
			ImGui::SliderFloat("##ProfilerFontSize", &Config::ProfilerFontScale.Value, 0.1f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			ImGui::PopItemWidth();

			ImGui::Separator();

			ImGui::Checkbox("Show FPS", &Config::ShowFPS.Value);
		}

		if (g_userHeap.heap != nullptr && g_userHeap.physicalHeap != nullptr)
		{
			if (ImGui::CollapsingHeader("Memory"))
			{
				O1HeapDiagnostics diagnostics, physicalDiagnostics;
				{
					std::lock_guard lock(g_userHeap.mutex);
					diagnostics = o1heapGetDiagnostics(g_userHeap.heap);
				}
				{
					std::lock_guard lock(g_userHeap.physicalMutex);
					physicalDiagnostics = o1heapGetDiagnostics(g_userHeap.physicalHeap);
				}

				if (ImGui::BeginTable("Memory", 2))
				{
					IMGUI_GENERIC_ROW("Heap Allocated", "%d MB", int32_t(diagnostics.allocated / (1024 * 1024)));
					IMGUI_GENERIC_ROW("Physical Heap Allocated", "%d MB", int32_t(physicalDiagnostics.allocated / (1024 * 1024)));

					ImGui::EndTable();
				}
			}
		}

		if (ImGui::CollapsingHeader("GPU"))
		{
			if (ImGui::BeginTable("GPU", 2))
			{
				IMGUI_GENERIC_ROW("API", "%s", g_vulkan ? "Vulkan" : "D3D12");

				if (auto pSDLVideoDriver = SDL_GetCurrentVideoDriver())
				{
					IMGUI_GENERIC_ROW("SDL Video Driver", "%s", pSDLVideoDriver);
				}

				IMGUI_GENERIC_ROW("Device", "%s", g_device->getDescription().name.c_str());
				IMGUI_GENERIC_ROW("Device Type", "%s", DeviceTypeName(g_device->getDescription().type));
				IMGUI_GENERIC_ROW("VRAM", "%.2f MiB", (double)(g_device->getDescription().dedicatedVideoMemory) / (1024.0 * 1024.0));
				IMGUI_GENERIC_ROW("GPU Waits", "%d", int32_t(g_waitForGPUCount));
				IMGUI_GENERIC_ROW("Buffer Uploads", "%d", int32_t(g_bufferUploadCount));

				IMGUI_GENERIC_ROW("Resolution", "%dx%d (%dx%d)",
					Video::s_viewportWidth, Video::s_viewportHeight,
					uint32_t(round(Video::s_viewportWidth * Config::ResolutionScale)),
					uint32_t(round(Video::s_viewportHeight * Config::ResolutionScale)));

				ImGui::EndTable();
			}

			ImGui::Separator();

			if (ImGui::TreeNode("Devices"))
			{
				ImGui::Indent();

				if (ImGui::BeginTable("Devices", 2))
				{
					auto deviceIndex = 0;

					for (const auto& deviceName : g_interface->getDeviceNames())
					{
						ImGui::TableNextColumn();
						ImGui::Text("Device #%d", deviceIndex++);
						ImGui::TableNextColumn();
						ImGui::Text("%s", deviceName.c_str());
						ImGui::SameLine();
					}

					ImGui::EndTable();
				}

				ImGui::Unindent();
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Features"))
			{
				ImGui::Indent();

				if (ImGui::BeginTable("Features", 2))
				{
					IMGUI_GENERIC_ROW("Dynamic Depth Bias", "%s", g_capabilities.dynamicDepthBias ? "Supported" : "Unsupported");
					IMGUI_GENERIC_ROW("GPU Upload Heap", "%s", g_capabilities.gpuUploadHeap ? "Supported" : "Unsupported");
					IMGUI_GENERIC_ROW("Hardware Resolve Modes", "%s", g_capabilities.resolveModes ? "Supported" : "Unsupported");
					IMGUI_GENERIC_ROW("Hardware Resolve", "%s", g_hardwareResolve ? "Enabled" : "Disabled");
					IMGUI_GENERIC_ROW("Hardware Depth Resolve", "%s", g_hardwareDepthResolve ? "Enabled" : "Disabled");
					IMGUI_GENERIC_ROW("Present Wait", "%s", g_capabilities.presentWait ? "Supported" : "Unsupported");
					if (g_vulkan)
					{
						IMGUI_GENERIC_ROW("Mailbox Present Mode", "%s", GetVulkanSwapChain()->mailboxPresentModeSupported ? "Supported" : "Unsupported");
						IMGUI_GENERIC_ROW("Present Mode", "%s", PresentModeName(GetVulkanSwapChain()->createdPresentMode));
					}
					IMGUI_GENERIC_ROW("Triangle Fan", "%s", g_capabilities.triangleFan ? "Supported" : "Unsupported");
					IMGUI_GENERIC_ROW("UMA", "%s", g_capabilities.uma ? "Supported" : "Unsupported");

					ImGui::EndTable();
				}

				ImGui::Unindent();
				ImGui::TreePop();
			}
		}
	}

#undef IMGUI_GENERIC_ROW

	ImGui::End();
	ImGui::PopFont();

	font->Scale = defaultScale;
}

// The refresh rate (Hz) of the display holding the game window, or 0 when
// the display or its mode cannot be determined.
static double GetDisplayRefreshRate()
{
	// SDL caches display modes in memory; this is a cheap read.
	auto mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(GameWindow::s_pWindow));
	if (mode && mode->refresh_rate > 0.0f)
	{
		return std::round((double)mode->refresh_rate);
	}

	return 0.0;
}

// 240 or Max Hz
int32_t GetMaxFPS()
{
	return std::max(FPS_MAX, (int32_t)GetDisplayRefreshRate());
}

// 241 or MaxHz+1 represents 'unlocked' mode
int32_t GetUnlockedFPS()
{
	return GetMaxFPS() + 1;
}

bool IsFPSUnlocked()
{
	return GetTargetFPS() >= GetUnlockedFPS();
}

// The configured FPS, or the display refresh rate when VSync is on.
double GetTargetFPS()
{
	double fps = std::min(Config::FPS.Value, GetUnlockedFPS());
	if (!Config::VSync)
		return fps;

	double refreshRate = GetDisplayRefreshRate();
	if (refreshRate > 0.0 && fps > refreshRate)
		return refreshRate;

	return fps;
}

static void DrawFPS()
{
	static std::deque<double> deltaTimes;
	static double totalDeltaTime = 0.0;
	static double fps = 0.0;

	double deltaTime = g_frameIntervalMs;
	if (deltaTime > 0.0)
	{
		deltaTimes.push_back(deltaTime);
		totalDeltaTime += deltaTime;
		while (deltaTimes.size() > 1 && totalDeltaTime - deltaTimes.front() >= 1000.0)
		{
			totalDeltaTime -= deltaTimes.front();
			deltaTimes.pop_front();
		}

		double averageFps = 1000.0 * deltaTimes.size() / totalDeltaTime;
		double blend = 1.0 - std::exp(-deltaTime / (0.25 * std::min(totalDeltaTime, 1000.0)));
		fps = fps > 0.0 ? fps + blend * (averageFps - fps) : averageFps;
	}

	if (!Config::ShowFPS)
		return;

	auto drawList = ImGui::GetBackgroundDrawList();

	auto fmt = fmt::format("FPS: {:.2f}", fps);
	//auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");
	auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-DB.otf");
	auto fontSize = Scale(10);
	auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, fmt.c_str());

	ImVec2 min = { Scale(40), Scale(30) };
	ImVec2 max = { min.x + std::max(Scale(75), textSize.x + Scale(10)), min.y + Scale(15) };
	ImVec2 textPos = { min.x + Scale(2), CENTRE_TEXT_VERT(min, max, textSize) + Scale(0.2f) };

	drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 200));
	drawList->AddText(font, fontSize, textPos, IM_COL32_WHITE, fmt.c_str());
}

static void DrawImGui(X86Context* ctx)
{

	// Must happen on this thread, before NewFrame the SDL event watch can run on the main thread or via SDL_PushEvent on a guest thread 
	// and ImGui's input queue is originally not thread safe. We drain the queue here to ensure that all events are processed before NewFrame.
	GameWindow::DrainImGuiEvents();

	ImGui_ImplSDL3_NewFrame();

	auto& io = ImGui::GetIO();
	io.DisplaySize = { float(Video::s_viewportWidth), float(Video::s_viewportHeight) };

	// ImGui doesn't know that we center the screen for specific aspect ratio
	// settings, which causes mouse events to not work correctly. To fix this, 
	// we can adjust the mouse events before ImGui processes them.
	uint32_t width = g_swapChain->getWidth();
	uint32_t height = g_swapChain->getHeight();
	float mousePosScaleX = float(width) / float(GameWindow::s_width);
	float mousePosScaleY = float(height) / float(GameWindow::s_height);
	float mousePosOffsetX = (width - Video::s_viewportWidth) / 2.0f;
	float mousePosOffsetY = (height - Video::s_viewportHeight) / 2.0f;
	for (int i = 0; i < io.Ctx->InputEventsQueue.Size; i++)
	{
		auto& e = io.Ctx->InputEventsQueue[i];
		if (e.Type == ImGuiInputEventType_MousePos)
		{
			if (e.MousePos.PosX != -FLT_MAX)
			{
				e.MousePos.PosX *= mousePosScaleX;
				e.MousePos.PosX -= mousePosOffsetX;
			}

			if (e.MousePos.PosY != -FLT_MAX)
			{
				e.MousePos.PosY *= mousePosScaleY;
				e.MousePos.PosY -= mousePosOffsetY;
			}
		}
	}

	ImGui::NewFrame();

	ResetImGuiCallbacks();

	MultiplayerHud::Draw();
	DrawPauseText(ctx);

	AchievementMenu::Draw(ctx);
	OptionsMenu::Draw(ctx);
	AchievementOverlay::Draw(ctx);
	ButtonGuide::Draw();
	InstallerWizard::Draw(ctx);
#ifdef __ANDROID__
	TouchControls::Draw(float(width), float(height));
#endif
	MessageWindow::Draw(ctx);
	Fader::Draw();

	assert(ImGui::GetBackgroundDrawList()->_ClipRectStack.Size == 1 && "Some clip rects were not removed from the stack!");

	DrawFPS();
	DrawProfiler();
	ImGui::Render();

	auto drawData = ImGui::GetDrawData();
	if (drawData->CmdListsCount != 0)
	{
		RenderCommand cmd;
		cmd.type = RenderCommandType::DrawImGui;
		SendRenderCommand(cmd);
	}
}

static void SetFramebuffer(GuestSurface* renderTarget, GuestSurface* depthStencil, bool settingForClear);

static void ProcDrawImGui(const RenderCommand& cmd)
{
	// Make sure the backbuffer is the current target.
	AddBarrier(g_backBuffer, RenderTextureLayout::COLOR_WRITE);
	FlushBarriers();
	SetFramebuffer(g_backBuffer, nullptr, false);

	auto& commandList = g_commandLists[g_frame];
	auto pipeline = g_imPipeline.get();

	commandList->setGraphicsPipelineLayout(g_imPipelineLayout.get());
	commandList->setPipeline(pipeline);
	commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
	commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 1);

	auto& drawData = *ImGui::GetDrawData();
	commandList->setViewports(RenderViewport(drawData.DisplayPos.x, drawData.DisplayPos.y, drawData.DisplaySize.x, drawData.DisplaySize.y));

	ImGuiPushConstants pushConstants{};
	pushConstants.displaySize = drawData.DisplaySize;
	pushConstants.inverseDisplaySize = { 1.0f / drawData.DisplaySize.x, 1.0f / drawData.DisplaySize.y };
	commandList->setGraphicsPushConstants(0, &pushConstants);

	size_t pushConstantRangeMin = ~0;
	size_t pushConstantRangeMax = 0;

	auto setPushConstants = [&](void* destination, const void* source, size_t size)
		{
			bool dirty = memcmp(destination, source, size) != 0;

			memcpy(destination, source, size);

			if (dirty)
			{
				size_t offset = reinterpret_cast<size_t>(destination) - reinterpret_cast<size_t>(&pushConstants);
				pushConstantRangeMin = std::min(pushConstantRangeMin, offset);
				pushConstantRangeMax = std::max(pushConstantRangeMax, offset + size);
			}
		};

	ImRect clipRect{};

	for (int i = 0; i < drawData.CmdListsCount; i++)
	{
		auto& drawList = drawData.CmdLists[i];

		auto vertexBufferAllocation = g_uploadAllocators[g_frame].allocate(drawList->VtxBuffer.Data, drawList->VtxBuffer.Size * sizeof(ImDrawVert), alignof(ImDrawVert));
		auto indexBufferAllocation = g_uploadAllocators[g_frame].allocate(drawList->IdxBuffer.Data, drawList->IdxBuffer.Size * sizeof(uint16_t), alignof(uint16_t));

		const RenderVertexBufferView vertexBufferView(vertexBufferAllocation.buffer->at(vertexBufferAllocation.offset), drawList->VtxBuffer.Size * sizeof(ImDrawVert));
		const RenderInputSlot inputSlot(0, sizeof(ImDrawVert));
		commandList->setVertexBuffers(0, &vertexBufferView, 1, &inputSlot);

		const RenderIndexBufferView indexBufferView(indexBufferAllocation.buffer->at(indexBufferAllocation.offset), drawList->IdxBuffer.Size * sizeof(uint16_t), RenderFormat::R16_UINT);
		commandList->setIndexBuffer(&indexBufferView);

		for (int j = 0; j < drawList->CmdBuffer.Size; j++)
		{
			auto& drawCmd = drawList->CmdBuffer[j];
			if (drawCmd.UserCallback != nullptr)
			{
				auto callbackData = reinterpret_cast<const ImGuiCallbackData*>(drawCmd.UserCallbackData);

				switch (static_cast<ImGuiCallback>(reinterpret_cast<size_t>(drawCmd.UserCallback)))
				{
				case ImGuiCallback::SetGradient:
					setPushConstants(&pushConstants.boundsMin, &callbackData->setGradient, sizeof(callbackData->setGradient));
					break;
				case ImGuiCallback::SetShaderModifier:
					setPushConstants(&pushConstants.shaderModifier, &callbackData->setShaderModifier, sizeof(callbackData->setShaderModifier));
					break;
				case ImGuiCallback::SetOrigin:
					setPushConstants(&pushConstants.origin, &callbackData->setOrigin, sizeof(callbackData->setOrigin));
					break;
				case ImGuiCallback::SetScale:
					setPushConstants(&pushConstants.scale, &callbackData->setScale, sizeof(callbackData->setScale));
					break;
				case ImGuiCallback::SetMarqueeFade:
					setPushConstants(&pushConstants.boundsMin, &callbackData->setMarqueeFade, sizeof(callbackData->setMarqueeFade));
					break;
				case ImGuiCallback::SetOutline:
					setPushConstants(&pushConstants.outline, &callbackData->setOutline, sizeof(callbackData->setOutline));
					break;
				case ImGuiCallback::SetProceduralOrigin:
					setPushConstants(&pushConstants.proceduralOrigin, &callbackData->setProceduralOrigin, sizeof(callbackData->setProceduralOrigin));
					break;
				case ImGuiCallback::SetAdditive:
				{
					auto pipelineToSet = callbackData->setAdditive.enabled ? g_imAdditivePipeline.get() : g_imPipeline.get();
					if (pipeline != pipelineToSet)
					{
						commandList->setPipeline(pipelineToSet);
						pipeline = pipelineToSet;
					}
					break;
				}
				default:
					assert(false && "Unknown ImGui callback type.");
					break;
				}
			}
			else
			{
				if (drawCmd.ClipRect.z <= drawCmd.ClipRect.x || drawCmd.ClipRect.w <= drawCmd.ClipRect.y)
					continue;

				auto texture = reinterpret_cast<GuestTexture*>(drawCmd.TextureId);
				uint32_t descriptorIndex = TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D;
				if (texture != nullptr)
				{
					if (texture->layout != RenderTextureLayout::SHADER_READ)
					{
						commandList->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
							RenderTextureBarrier(texture->texture, RenderTextureLayout::SHADER_READ));

						texture->layout = RenderTextureLayout::SHADER_READ;
					}

					descriptorIndex = texture->descriptorIndex;

					if (texture == g_imFontTexture.get())
						descriptorIndex |= 0x80000000;

					setPushConstants(&pushConstants.texture2DDescriptorIndex, &descriptorIndex, sizeof(descriptorIndex));
				}

				if (pushConstantRangeMin < pushConstantRangeMax)
				{
					commandList->setGraphicsPushConstants(0, reinterpret_cast<const uint8_t*>(&pushConstants) + pushConstantRangeMin, pushConstantRangeMin, pushConstantRangeMax - pushConstantRangeMin);
					pushConstantRangeMin = ~0;
					pushConstantRangeMax = 0;
				}

				if (memcmp(&clipRect, &drawCmd.ClipRect, sizeof(clipRect)) != 0)
				{
					commandList->setScissors(RenderRect(int32_t(drawCmd.ClipRect.x), int32_t(drawCmd.ClipRect.y), int32_t(drawCmd.ClipRect.z), int32_t(drawCmd.ClipRect.w)));
					clipRect = drawCmd.ClipRect;
				}

				commandList->drawIndexedInstanced(drawCmd.ElemCount, 1, drawCmd.IdxOffset, drawCmd.VtxOffset, 0);
			}
		}
	}
}

static std::atomic<uint32_t> g_frameEndsDone{ 0 };
static constexpr double GPU_BOUND_FENCE_WAIT_MS = 0.1;

static void WaitForFrameEnds(uint32_t count)
{
	uint32_t done = g_frameEndsDone.load();
	while (done != count)
	{
		g_frameEndsDone.wait(done);
		done = g_frameEndsDone.load();
	}
}

static std::atomic<bool> presentExecuted(true);

static int g_drawPrimitiveUP_ID = 0;
// Debug helper: dumps present/swap-chain/window state to stdout. Intended to
// be called from a paused debugger via:
//   (gdb) call VideoDebug_DumpPresentState()
// Remove this once the Wayland backgrounding hang is root-caused.
void VideoDebug_DumpPresentState()
{
	const SDL_WindowFlags sdlFlags = (GameWindow::s_pWindow != nullptr)
		? SDL_GetWindowFlags(GameWindow::s_pWindow)
		: 0u;
	int winW = 0, winH = 0;
	if (GameWindow::s_pWindow != nullptr)
		SDL_GetWindowSizeInPixels(GameWindow::s_pWindow, &winW, &winH);

	LOGN("=== VideoDebug_DumpPresentState ===");
	LOGFN("  swap chain: g_swapChain={} g_swapChainValid={} presentWait_capable={} g_backBufferIndex={}",
		(void*)g_swapChain.get(), (int)g_swapChainValid, (int)g_capabilities.presentWait, g_backBufferIndex);
	LOGFN("  present pipeline: presentExecuted={} gameFrame={} frameEndsDone={}",
		(int)presentExecuted.load(), g_gameFrame.load(), g_frameEndsDone.load());
	LOGFN("  frames: g_frame={} g_nextFrame={}", g_frame, g_nextFrame);
	std::string commandListStates;
	for (size_t i = 0; i < NUM_FRAMES; ++i)
		commandListStates += fmt::format(" [{}]={}", i, (int)g_commandListStates[i]);
	LOGFN("  commandListStates per frame:{}", commandListStates);
	LOGFN("  renderQueue size_approx={}", g_renderQueue.size_approx());
	LOGFN("  loadingThreadID={} currentThreadId={}",
		GetLoadingThreadID(), GuestThread::GetCurrentThreadId());
	LOGFN("  window: ptr={} SDL flags=0x{:X} (HIDDEN={} MINIMIZED={} INPUT_FOCUS={} MOUSE_FOCUS={} MAXIMIZED={} FULLSCREEN={})",
		(void*)GameWindow::s_pWindow, (unsigned)sdlFlags,
		(int)!!(sdlFlags & SDL_WINDOW_HIDDEN),
		(int)!!(sdlFlags & SDL_WINDOW_MINIMIZED),
		(int)!!(sdlFlags & SDL_WINDOW_INPUT_FOCUS),
		(int)!!(sdlFlags & SDL_WINDOW_MOUSE_FOCUS),
		(int)!!(sdlFlags & SDL_WINDOW_MAXIMIZED),
		(int)!!(sdlFlags & SDL_WINDOW_FULLSCREEN));
	LOGFN("  window size in pixels: {}x{}", winW, winH);
	LOGN("===================================");
}

void Video::BeginGameThreadProfiler() { g_updateDirectorProfiler.Begin(); }
void Video::EndGameThreadProfiler()
{
	g_updateDirectorProfiler.End();
	g_gtDrawRecordingAccum.SnapshotAndReset();
	g_gtStateFlushAccum.SnapshotAndReset();
}

// When the game calls Present, it checks if it should present immediately or wait for the next frame. 
// The main loop always asks for 1, but the loading screen asks for immediate mode. 
// This can create issues with the game thread and the host's render thread being out of sync, so we track the state here
// and do a hacky method to fix that.
static thread_local bool isPresentIntervalOne = true;
//rwD3D8RasterShowRaster (used to Present)
X86_FUNC_IMPL(__imp__sub_2DDC00);
X86_OVERRIDE_FUNC(sub_2DDC00)
{
	isPresentIntervalOne = (X86_LOAD_U8(ctx.esp.u32 + 0xC) & 1) != 0;
	__imp__sub_2DDC00(ctx, base);
	isPresentIntervalOne = true;
}

void Video::Present(X86Context* ctx)
{
	g_presentProfiler.Begin();

	std::unique_lock presentLock(g_presentMutex);

	// Get actual frame delivery rate seen by the OS.
	{
		static std::chrono::steady_clock::time_point s_prevPresentTime;
		static bool s_presentTimeInit = false;
		auto now = std::chrono::steady_clock::now();
		if (s_presentTimeInit)
			g_frameIntervalMs = std::chrono::duration<double, std::milli>(now - s_prevPresentTime).count();
		s_presentTimeInit = true;
		s_prevPresentTime = now;
	}

	const uint32_t gameFrame = g_gameFrame.load(std::memory_order_relaxed);
	g_frameEndWaitProfiler.Begin();
	WaitForFrameEnds(gameFrame);
	g_frameEndWaitProfiler.End();

	DrawImGui(ctx);

	const bool gpuBound = g_frameFenceProfiler.value.load(std::memory_order_relaxed) > GPU_BOUND_FENCE_WAIT_MS;
	const bool resizing = g_pendingSurfaceResizeFlag || g_swapChainRebuildRequested;
	const bool readsGuestMemory = g_frameReadsGuestMemory.exchange(false);
	const bool waitForFrameEnd = Config::VSync || gpuBound || resizing || readsGuestMemory || !isPresentIntervalOne || ctx == nullptr;

	g_intermediaryUploadAllocators[(gameFrame + 1) % std::size(g_intermediaryUploadAllocators)].reset();
	g_stagedBufferUploads[(gameFrame + 1) % std::size(g_stagedBufferUploads)].reset();

	RenderCommand cmd;
	cmd.type = RenderCommandType::EndFrame;
	cmd.endFrame.gameThreadWaits = waitForFrameEnd;
	SendRenderCommand(cmd);
	g_gameFrame.store(gameFrame + 1, std::memory_order_relaxed);

	if (waitForFrameEnd)
	{
		WaitForFrameEnds(gameFrame + 1);

		static uint32_t s_offsetsWidth = 0;
		static uint32_t s_offsetsHeight = 0;
		if (s_offsetsWidth != s_viewportWidth || s_offsetsHeight != s_viewportHeight)
		{
			AspectRatioPatches::ComputeOffsets();
			s_offsetsWidth = s_viewportWidth;
			s_offsetsHeight = s_viewportHeight;
		}
	}

	presentLock.unlock();

	if (isPresentIntervalOne == false)
	{
		// As explained above, we can instantly swap since this is the loading screen. 
		thread_local FramePacer immediateFramePacer;
		immediateFramePacer.SetFpsLimit(60.0);
		immediateFramePacer.EndFrame(FramePacer::WaitMode::SleepOnly);
	}

	g_presentProfiler.End();
}

void Video::PrecompileShadersAndPipelines()
{
	PrecompileShaders();
	STARTUP_PHASE("shaders created");

	{
		// Where pipelines are compiled in the background, the two calls below only queue
		// them, and the background threads take the lot as one batch.
		BackgroundPipelineBatch batch;
		PrecompileStaticPipelines();
		PrecompileUserPipelines();
	}

#ifdef __ANDROID__
	// Otherwise the background threads report this when they are done.
	if (!PrecompilesPipelinesInBackground())
		AndroidVideo::OnCachedPipelinesCreated();
#endif

	STARTUP_PHASE(PrecompilesPipelinesInBackground() ? "pipelines queued for background compilation" : "pipelines created");
}

void Video::SaveCaches()
{
	SavePipelineCache(true);

#ifdef __ANDROID__
	AndroidVideo::SavePipelineCache();
#endif
}

void Video::PrecompileShadersAndPipelinesForInstall(const std::function<void(float)>& progress)
{
	PrecompileShaders(progress);
	PrecompileStaticPipelines(progress);
}

static PixelPushConstants BuildPixelPushConstants(const SharedConstants& constants)
{
	PixelPushConstants result;

	// Textures and samplers
	result.texture3DIndex = constants.texture3DIndices[0];
	result.textureCubeIndex = constants.textureCubeIndices[0];
	memcpy(result.texture2DIndices, constants.texture2DIndices, sizeof(result.texture2DIndices));
	memcpy(result.samplerIndices, constants.samplerIndices, sizeof(result.samplerIndices));

	// Texture coordinate clamp
	memcpy(result.texCoordMin, constants.texCoordMin, sizeof(result.texCoordMin));
	memcpy(result.texCoordMax, constants.texCoordMax, sizeof(result.texCoordMax));

	// Fog
	result.fogStart = constants.FogStart;
	result.fogEnd = constants.FogEnd;
	memcpy(result.fogColor, constants.FogColor, sizeof(result.fogColor));

	// Alpha test
	result.alphaTestEnable = constants.alphaTestEnable;
	result.alphaFunc = constants.alphaFunc;
	result.alphaRef = constants.alphaRef;

	return result;
}

static void SetRootDescriptor(const UploadAllocation& allocation, size_t index)
{
	auto& commandList = g_commandLists[g_frame];

	if (g_vulkan)
		commandList->setGraphicsPushConstants(0, &allocation.deviceAddress, 8 * index, 8);
	else
		commandList->setGraphicsRootDescriptor(allocation.buffer->at(allocation.offset), index);
}

static void ProcExecuteCommandList(const RenderCommand& cmd)
{
	g_commandListOpen = false;

	if (g_swapChainValid)
	{
		auto swapChainTexture = g_swapChain->getTexture(g_backBufferIndex);
		if (g_backBuffer->texture == g_intermediaryBackBufferTexture.get())
		{
			struct
			{
				float gammaR;
				float gammaG;
				float gammaB;
				uint32_t textureDescriptorIndex;

				int32_t viewportOffsetX;
				int32_t viewportOffsetY;
				int32_t viewportWidth;
				int32_t viewportHeight;
			} constants;

			constants.gammaR = 1.0f;
			constants.gammaG = 1.0f;
			constants.gammaB = 1.0f;

			float offset = (Config::Brightness.Value - 0.5f) * 1.2f;

			constants.gammaR = 1.0f / std::clamp(constants.gammaR + offset, 0.1f, 4.0f);
			constants.gammaG = 1.0f / std::clamp(constants.gammaG + offset, 0.1f, 4.0f);
			constants.gammaB = 1.0f / std::clamp(constants.gammaB + offset, 0.1f, 4.0f);
			constants.textureDescriptorIndex = g_intermediaryBackBufferTextureDescriptorIndex;

			constants.viewportOffsetX = (int32_t(g_swapChain->getWidth()) - int32_t(Video::s_viewportWidth)) / 2;
			constants.viewportOffsetY = (int32_t(g_swapChain->getHeight()) - int32_t(Video::s_viewportHeight)) / 2;
			constants.viewportWidth = Video::s_viewportWidth;
			constants.viewportHeight = Video::s_viewportHeight;

			auto& framebuffer = g_backBuffer->framebuffers[swapChainTexture];
			if (!framebuffer.framebuffer)
			{
				RenderFramebufferDesc desc;
				desc.colorAttachments = const_cast<const RenderTexture**>(&swapChainTexture);
				desc.colorAttachmentsCount = 1;
				framebuffer = { g_device->createFramebuffer(desc), 0 };
			}

			RenderTextureBarrier srcBarriers[] =
			{
				RenderTextureBarrier(g_intermediaryBackBufferTexture.get(), RenderTextureLayout::SHADER_READ),
				RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE)
			};

			auto& commandList = g_commandLists[g_frame];
			commandList->barriers(RenderBarrierStage::GRAPHICS, srcBarriers, std::size(srcBarriers));
			commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
			commandList->setPipeline(g_gammaCorrectionPipeline.get());
			commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);

			SetRootDescriptor(g_uploadAllocators[g_frame].allocate(&constants, sizeof(constants), 0x100), 2);
			commandList->setFramebuffer(framebuffer.framebuffer.get());
			commandList->setViewports(RenderViewport(0.0f, 0.0f, g_swapChain->getWidth(), g_swapChain->getHeight()));
			commandList->setScissors(RenderRect(0, 0, g_swapChain->getWidth(), g_swapChain->getHeight()));
			commandList->drawInstanced(6, 1, 0, 0);

			commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::PRESENT));
		}
		else
		{
			AddBarrier(g_backBuffer, RenderTextureLayout::PRESENT);
			FlushBarriers();
		}
	}

	auto& commandList = g_commandLists[g_frame];
	commandList->writeTimestamp(g_queryPools[g_frame].get(), 1);
	commandList->end();

	if (g_swapChainValid)
	{
		const RenderCommandList* commandLists[] = { commandList.get() };
		RenderCommandSemaphore* waitSemaphores[] = { g_acquireSemaphores[g_frame].get() };
		RenderCommandSemaphore* signalSemaphores[] = { g_renderSemaphores[g_frame].get() };

		g_queue->executeCommandLists(
			commandLists, std::size(commandLists),
			waitSemaphores, std::size(waitSemaphores),
			signalSemaphores, std::size(signalSemaphores),
			g_commandFences[g_frame].get());
	}
	else
	{
		g_queue->executeCommandLists(commandList.get(), g_commandFences[g_frame].get());
	}

	g_commandListStates[g_frame] = true;
}

// hold commands that arrive late (previously discarded)
static std::vector<RenderCommand> g_deferredRenderCommands;
static void ProcessRenderCommand(const RenderCommand& cmd);
static void ProcBeginCommandList(const RenderCommand& cmd)
{
	RtScopeTimer _scope(g_rtBeginCmdListWorkAccum);
	DestructTempResources();

	// ProcEndFrame has already waited.
	// So this is safe to clear here 
	// - Sewer-chan
	g_uploadAllocators[g_frame].reset();
	BeginCommandList(cmd.endFrame.gameThreadWaits);

	// Run deferred commands; TODO: Maybe just discard them like before, needs more testing
	std::vector<RenderCommand> deferred = std::move(g_deferredRenderCommands);
	g_deferredRenderCommands.clear();
	for (const RenderCommand& deferredCmd : deferred)
		ProcessRenderCommand(deferredCmd);
}

static void ProcEndFrame(const RenderCommand& cmd)
{
	ProcExecuteCommandList(cmd);

	presentExecuted = false;
	if (g_swapChainValid)
	{
		RenderCommandSemaphore* signalSemaphores[] = { g_renderSemaphores[g_frame].get() };
		g_swapChainValid = g_swapChain->present(g_backBufferIndex, signalSemaphores, std::size(signalSemaphores));

#ifdef __ANDROID__
		if (g_swapChainValid)
			AndroidVideo::OnFramePresented();
#endif
	}
	presentExecuted = true;

	g_frame = g_nextFrame;
	g_nextFrame = (g_frame + 1) % NUM_FRAMES;

	if (g_commandListStates[g_frame])
	{
		g_frameFenceProfiler.Begin();
		g_queue->waitForCommandFence(g_commandFences[g_frame].get());
		g_frameFenceProfiler.End();
		g_commandListStates[g_frame] = false;

		g_queryPools[g_frame]->queryResults();
		const uint64_t* frameTimestamps = g_queryPools[g_frame]->getResults();
		g_gpuFrameProfiler.Set(double(frameTimestamps[1] - frameTimestamps[0]) / 1000000.0);
	}

	g_dirtyStates = DirtyStates(true);
	g_quadIndexData.reset();

	g_presentWaitProfiler.Begin();

	// wait on fifo only
	if (g_vulkan && g_swapChainValid && g_capabilities.presentWait && Config::VSync && GetVulkanSwapChain()->createdPresentMode == VK_PRESENT_MODE_FIFO_KHR)
	{
		g_swapChain->wait();
	}
	g_presentWaitProfiler.End();

	CheckSwapChain(cmd.endFrame.gameThreadWaits);
	ProcBeginCommandList(cmd);

	g_frameEndsDone.fetch_add(1);
	g_frameEndsDone.notify_all();
}

static void ComputeViewportSize()
{
	uint32_t width = g_swapChain->getWidth();
	uint32_t height = g_swapChain->getHeight();
	float aspectRatio = float(width) / float(height);

	uint32_t numerator;
	uint32_t denominator;

	switch (Config::AspectRatio)
	{
	case EAspectRatio::Narrow5x4:
		numerator = 5;
		denominator = 4;
		break;

	case EAspectRatio::Original:
		numerator = 4;
		denominator = 3;
		break;

	case EAspectRatio::Wide16x10:
		numerator = 16;
		denominator = 10;
		break;

	case EAspectRatio::Wide16x9:
		numerator = 16;
		denominator = 9;
		break;

	case EAspectRatio::Wide21x9:
		numerator = 21;
		denominator = 9;
		break;

	default:
		numerator = 0;
		denominator = 0;
		break;
	}

	if (denominator != 0 && aspectRatio > float(numerator) / float(denominator))
	{
		width = height * numerator / denominator;
	}
	else if (denominator != 0)
	{
		height = width * denominator / numerator;
	}

	{
		std::lock_guard lock(g_surfaceRTsMutex);
		// The swapchain can change size without another window resize event.
		surface_resize::SetViewportSize(Video::s_viewportWidth, Video::s_viewportHeight, width, height,
			g_pendingSurfaceResizeFlag);
	}
}

void Video::ComputeViewportDimensions()
{
	ComputeViewportSize();
	AspectRatioPatches::ComputeOffsets();
}

GuestTexture* CreateTexture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, X_D3DFORMAT format, X_D3DRESOURCETYPE type)
{
	// Only resizable textures need the resize lock.
	std::unique_lock resizeLock(g_surfaceRTsMutex, std::defer_lock);
	if (usage == 1)
		resizeLock.lock();

	auto texture = g_userHeap.AllocPhysical<GuestTexture>(type == X_D3DRTYPE_VOLUMETEXTURE ? ResourceType::VolumeTexture : ResourceType::Texture);

	texture->g_width = width; // store original info for resolution scaling later 
	texture->g_height = height;

	RenderTextureDesc desc;
	desc.dimension = RenderTextureDimension::TEXTURE_2D;

	if (usage == 1) //will be RenderTarget
	{
		uint32_t newWidth, newHeight;
		GetUpscaledSurfaceResolution(width, height, &newWidth, &newHeight);
		width = newWidth;
		height = newHeight;
	}

	desc.width = width;
	desc.height = height;
	desc.depth = depth;
	desc.mipLevels = levels;
	desc.arraySize = 1;
	desc.format = ConvertFormat(format);

#ifdef __ANDROID__
	// A GPU without block-compressed textures gets a plain one, which CopyTextureToGPU()
	// unpacks the data into.
	{
		BcDecode::EFormat decode;
		desc.format = AndroidVideo::HostTextureFormat(desc.format, decode);
	}
#endif

	if (desc.format == DepthStencilFormat && FormatIsDepthBuffer(format))
		desc.flags = RenderTextureFlag::DEPTH_TARGET;
	else if (FormatIsRenderTarget(format) && usage == 1)
		desc.flags = RenderTextureFlag::RENDER_TARGET;
	else
		desc.flags = RenderTextureFlag::NONE;


	texture->isRenderTarget = (desc.flags & RenderTextureFlag::RENDER_TARGET) != 0;

	texture->textureHolder = g_device->createTexture(desc);
	texture->texture = texture->textureHolder.get();
	texture->textureGeneration = NextGuestTextureGeneration();

	RenderTextureViewDesc viewDesc;
	viewDesc.format = desc.format;
	viewDesc.dimension = texture->type == ResourceType::VolumeTexture ? RenderTextureViewDimension::TEXTURE_3D : RenderTextureViewDimension::TEXTURE_2D;
	viewDesc.mipLevels = levels;

	switch (desc.format)
	{
	case RenderFormat::R8_UNORM: //todo check if this is correct
		viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::ONE);
		break;
	case RenderFormat::B8G8R8A8_UNORM:
	case RenderFormat::R8G8B8A8_UNORM: //(confirmed) no swizzling needed, color order is already correct!
		viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::IDENTITY, RenderSwizzle::IDENTITY, RenderSwizzle::IDENTITY, RenderSwizzle::IDENTITY);
		break;
	}

	texture->textureView = texture->texture->createTextureView(viewDesc);

	texture->width = width;
	texture->height = height;
	texture->depth = depth;
	texture->format = desc.format;
	texture->viewDimension = viewDesc.dimension;
	texture->descriptorIndex = g_textureDescriptorAllocator.allocate();

	const bool isRT = ((desc.flags & RenderTextureFlag::RENDER_TARGET) != 0);
	const bool isStencil = ((desc.flags & RenderTextureFlag::DEPTH_TARGET) != 0);

	if (isStencil || isRT)
	{
		DiscardInitRenderResource(texture->texture, isRT);
	}

	g_textureDescriptorSet->setTexture(texture->descriptorIndex, texture->texture, RenderTextureLayout::SHADER_READ, texture->textureView.get());

	if (usage == 1)
		g_resizableTextures.insert(texture);

	return texture;
}


static RenderHeapType GetBufferHeapType()
{
	return g_capabilities.gpuUploadHeap ? RenderHeapType::GPU_UPLOAD : RenderHeapType::DEFAULT;
}

GuestBuffer* CreateVertexBuffer(uint32_t length)
{
	auto buffer = g_userHeap.AllocPhysical<GuestBuffer>(ResourceType::VertexBuffer);
	if (buffer)
	{
		buffer->buffer = g_device->createBuffer(RenderBufferDesc::VertexBuffer(length, GetBufferHeapType(), RenderBufferFlag::INDEX));
		buffer->dataSize = length;
		buffer->guestResource.Common = 1;


#ifdef _DEBUG 
		buffer->buffer->setName(fmt::format("Vertex Buffer {:X}", g_memory.MapVirtual(buffer)));
		buffer->isVertexBuffer = true;
#endif
	}

	return buffer;
}

static GuestBuffer* CreateIndexBuffer(uint32_t IndexCount, X_D3DFORMAT format)
{
	uint32_t length = IndexCount * sizeof(int16_t);

	auto buffer = g_userHeap.AllocPhysical<GuestBuffer>(ResourceType::IndexBuffer);
	if (buffer)
	{
		buffer->buffer = g_device->createBuffer(RenderBufferDesc::IndexBuffer(length, GetBufferHeapType()));
		buffer->dataSize = length;
		buffer->format = ConvertFormat(format);
		buffer->guestFormat = format;
#ifdef _DEBUG 
		buffer->buffer->setName(fmt::format("Index Buffer {:X}", g_memory.MapVirtual(buffer)));
#endif
	}
	return buffer;
}


static GuestSurface* CreateSurface(uint32_t width, uint32_t height, X_D3DFORMAT format, bool formatSet = false, bool coversBackBuffer = false)
{
	std::lock_guard resizeLock(g_surfaceRTsMutex);
	auto ogFormat = formatSet ? static_cast<plume::RenderFormat>(format) : ConvertFormat(format);
	auto isStencil = ogFormat == DepthStencilFormat;
	uint32_t ogWidth = width;
	uint32_t ogHeight = height;
	uint32_t newWidth, newHeight;

	GetUpscaledSurfaceResolution(width, height, &newWidth, &newHeight);

#ifdef __ANDROID__
	if (coversBackBuffer)
		CoverBackBuffer(&newWidth, &newHeight);
#endif

	width = newWidth;
	height = newHeight;

	RenderTextureDesc desc;
	desc.dimension = RenderTextureDimension::TEXTURE_2D;
	desc.width = width;
	desc.height = height;
	desc.depth = 1;
	desc.mipLevels = 1;
	desc.arraySize = 1;
	desc.multisampling.sampleCount = RenderSampleCount::COUNT_1; //todo add as an option(?)
	desc.format = ogFormat;
	desc.flags = desc.format == DepthStencilFormat ? RenderTextureFlag::DEPTH_TARGET : RenderTextureFlag::RENDER_TARGET;

	auto surface = g_userHeap.AllocPhysical<GuestSurface>(desc.format == DepthStencilFormat ?
		ResourceType::DepthStencil : ResourceType::RenderTarget);

	surface->textureHolder = g_device->createTexture(desc);
	surface->texture = surface->textureHolder.get();
	surface->textureGeneration = NextGuestTextureGeneration();
	surface->width = width;
	surface->height = height;
	surface->g_width = ogWidth;
	surface->g_height = ogHeight;
	surface->format = desc.format;
	surface->guestFormat = format;
	surface->sampleCount = desc.multisampling.sampleCount;
	surface->coversBackBuffer = coversBackBuffer;

	RenderTextureViewDesc viewDesc;
	viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
	viewDesc.format = desc.format;
	viewDesc.mipLevels = 1;

	surface->textureView = surface->textureHolder->createTextureView(viewDesc);
	surface->descriptorIndex = g_textureDescriptorAllocator.allocate();
	g_textureDescriptorSet->setTexture(surface->descriptorIndex, surface->textureHolder.get(), RenderTextureLayout::SHADER_READ, surface->textureView.get());

	const bool isRT = ((desc.flags & RenderTextureFlag::RENDER_TARGET) != 0);

#ifdef _DEBUG 
	surface->texture->setName(fmt::format("{} {:X}", isRT ? "Render Target" : "Depth Stencil", g_memory.MapVirtual(surface)));
#endif

	DiscardInitRenderResource(surface->texture, isRT);

	surface->guestResource.Format = format;
	g_resizableSurfaces.insert(surface);
	return surface;
}

static std::vector<GuestSurface*> g_surfaceCache;
static GuestSurface* GetSurfaceLevel(GuestTexture* texture, int32_t level, X_D3DFORMAT Format)
{
	std::lock_guard lock(g_surfaceRTsMutex);
	uint32_t mipWidth = std::max(1u, texture->width >> level);
	uint32_t mipHeight = std::max(1u, texture->height >> level);

	GuestSurface* surface = g_userHeap.AllocPhysical<GuestSurface>(
		texture->format == DepthStencilFormat ?
		ResourceType::DepthStencil : ResourceType::RenderTarget
	);


	surface->guestFormat = Format;
	surface->sampleCount = RenderSampleCount::COUNT_1;
	surface->guestResource.Format = Format;

	surface->texture = texture->texture;
	surface->textureGeneration = texture->textureGeneration;
	surface->width = mipWidth;
	surface->height = mipHeight;
	surface->g_width = texture->g_width;
	surface->g_height = texture->g_height;
	surface->format = texture->format;
	surface->layout = texture->layout;

	surface->parentTexture = texture;
	texture->surfaceRTs.push_back(surface);

	return surface;
}

X86_FUNC_IMPL(__imp__sub_3DA010);
//CreateSurfaceLevel = "view" or "interface" to access ONE specific level/slice of a texture resource.
GuestSurface* CreateSurfaceOfTexture(X86Context& ctx, X_D3DFORMAT Format, uint32_t size, GuestBaseTexture* pParent, int32_t pvData)
{
	auto base = g_memory.base;
	__imp__sub_3DA010(ctx, base);
	ctx.esp.u32 -= 20;

	auto gSurface = (X_D3DSurface*)g_memory.Translate(ctx.eax.u32);
	auto newSurface = GetSurfaceLevel((GuestTexture*)pParent, 0, Format);
	memcpy(&newSurface->guestResource, gSurface, sizeof(X_D3DSurface));
	ctx.eax.u32 = g_memory.MapVirtual(newSurface);
	g_userHeap.Free(gSurface);
	return newSurface;
}

//CreateDepthStencilSurface
GuestSurface* CreateStandAloneSurface(int32_t Width, int32_t Height, int32_t a3, X_D3DFORMAT Format)
{
	return CreateSurface(Width, Height, Format);
}

// Helper to capture 512x384 draw in a 512x512 texture (aka bottom bloom box Xbox shit)
static void TrackRenderTargetLiveArea(GuestSurface* surface, const RenderViewport& viewport)
{
	const auto parent = surface->parentTexture;

	if (parent == nullptr || parent->texture != surface->texture)
		return;

	const uint32_t right = (uint32_t)std::clamp(viewport.x + viewport.width, 0.0f, (float)surface->g_width);
	const uint32_t bottom = (uint32_t)std::clamp(viewport.y + viewport.height, 0.0f, (float)surface->g_height);

	if (parent->liveAreaReset)
	{
		parent->liveWidth = right;
		parent->liveHeight = bottom;
		parent->liveAreaReset = false;
	}
	else
	{
		parent->liveWidth = std::max(parent->liveWidth, right);
		parent->liveHeight = std::max(parent->liveHeight, bottom);
	}
}

bool viewPort_wasFixed = false;
static void FlushViewport(const bool isFixed)
{
	auto& commandList = g_commandLists[g_frame];
	auto hostWidth = g_renderTarget->width;
	auto hostHeight = g_renderTarget->height;
	auto viewport = g_renderviewport.vp;
	auto backupVP = viewport;

	TrackRenderTargetLiveArea(g_renderTarget, viewport);

	if (isFixed != viewPort_wasFixed)
	{
		g_dirtyStates.viewport = true;
		g_dirtyStates.scissorRect = true;
	}

	viewPort_wasFixed = isFixed;

	float scaleX = (float)g_renderTarget->width / (float)g_renderTarget->g_width;
	float scaleY = (float)g_renderTarget->height / (float)g_renderTarget->g_height;
	float width = (float)std::min(viewport.width, (DWORD)g_renderTarget->g_width - viewport.x);
	float height = (float)std::min(viewport.height, (DWORD)g_renderTarget->g_height - viewport.y);


	width *= scaleX;
	height *= scaleY;
	viewport.x *= scaleX;
	viewport.y *= scaleY;

	// The Xbox half-pixel viewport offset reproduces the "vertex pos in a pixel centre" rule, so it has to be scaled.
	// However, scaling it cost coverage, some pixels of the target go unwritten because the guest's
	// fullscreen quads stop half a guest pixel short of the edge. This is handled by a separate fix in the Pixel Shader.
	// The backbuffer is the only exception since nothing sample it.

	const bool isBackBuffer = (g_renderTarget == g_backBuffer);
	const float halfPixelX = isBackBuffer ? 0.5f : 0.5f * scaleX;
	const float halfPixelY = isBackBuffer ? 0.5f : 0.5f * scaleY;

	viewport.x += halfPixelX;
	viewport.y += halfPixelY;

	if (isFixed == false)
	{
		width = (float)hostWidth;
		height = (float)hostHeight;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		viewport.x = halfPixelX;
		viewport.y = halfPixelY;
	}

	if (g_dirtyStates.viewport)
	{
		if (viewport.minDepth > viewport.maxDepth)
			std::swap(viewport.minDepth, viewport.maxDepth);

		viewport.width = width;
		viewport.height = height;

		commandList->setViewports(viewport);
		g_dirtyStates.viewport = false;
	}

	if (g_dirtyStates.scissorRect)
	{
		RenderRect scissorRect;
		int32_t left = (int32_t)(backupVP.x * scaleX);
		int32_t top = (int32_t)(backupVP.y * scaleY);
		int32_t right = left + (int32_t)(backupVP.width * scaleX);
		int32_t bottom = top + (int32_t)(backupVP.height * scaleY);

		if (isFixed)
		{
			right = (int32_t)hostWidth;
			bottom = (int32_t)hostHeight;
			left = 0;
			top = 0;
		}

		scissorRect = g_scissorTestEnable ? g_scissorRect : RenderRect(
			left,
			top,
			right,
			bottom);

		commandList->setScissors(scissorRect);
		g_dirtyStates.scissorRect = false;
	}
}

void SetResolution(int adapter, unsigned int a2, uint32_t a3)
{

	// Simulate format flags
	uint32_t formatFlag;
	switch (a2 & 3)
	{
	case 0: formatFlag = 30; break;
	case 1: formatFlag = 17; break;
	case 2: formatFlag = 28; break;
	case 3: formatFlag = 18; break;
	}

	Video::ComputeViewportDimensions();

	uint32_t width = uint32_t(round(Video::s_viewportWidth * Config::ResolutionScale));
	uint32_t height = uint32_t(round(Video::s_viewportHeight * Config::ResolutionScale));

	auto base = g_memory.base;

	// this does affect fixed function, such as text position on the screen
	X86_STORE_U32(a3, 640);
	X86_STORE_U32(a3 + (4 * 1), 480);
	X86_STORE_U32(a3 + (4 * 2), 60); //refresh rate in hz
	X86_STORE_U32(a3 + (4 * 3), 0x120);
	X86_STORE_U32(a3 + (4 * 4), formatFlag);
}


X86_FUNC_IMPL(__imp__sub_3DDAB0); //CreateDevice

int32_t Direct3D_CreateDevice_r(X86Context& ctx, int32_t Adapter, int32_t DeviceType, int32_t hFocusWindow, char Flags, X_D3DPRESENT_PARAMETERS* pPresentationParameters, uint32_t* device)
{
	LOG_UTILITY("CREATE GUEST D3D DEVICE!");


	InitAchievementTextures();

	auto base = g_memory.base;

	// Move backbuffer to guest memory.
	assert(!g_memory.IsInMemoryRange(g_backBuffer) && g_backBufferHolder != nullptr);
	auto* oldBackBuffer = g_backBufferHolder.get(); // installer -> game
	g_backBuffer = g_userHeap.AllocPhysical<GuestSurface>(std::move(*g_backBufferHolder));

	{
		std::lock_guard lock(g_surfaceRTsMutex);
		for (auto& [key, containers] : g_framebufferCacheUsers)
		{
			if (containers.erase(oldBackBuffer))
			{
				containers.insert(g_backBuffer);
			}
		}
	}

	// Check for stale reference. BeginCommandList() gets called before CreateDevice() which is where the assignment happens.
	if (g_renderTarget == g_backBufferHolder.get())
		g_renderTarget = g_backBuffer;

	if (g_depthStencil == g_backBufferHolder.get())
		g_depthStencil = g_backBuffer;

	// Free the host backbuffer.
	g_backBufferHolder = nullptr;


	//assign the guest array of texture render state to a pointer so we can get each texture state in real time
	g_texture_stage_states = (int32_t*)(base + g_DeferredTextureStateAddr);

	//assign the guest array of render state to a pointer so we can get each render state in real time, this reduces number of hooks needed and bypass the inlined functions.
	InitGuestRenderStatePointer((uint32_t*)(base + g_RenderStateAddr));
	BuildRenderStateMappingTable();

	__imp__sub_3DDAB0(ctx, base);
	ctx.esp.u32 -= 28; //readjust esp because of calling original function

	return 0; //success
}

static void ProcDestructResource(const RenderCommand& cmd)
{
	const auto& args = cmd.destructResource;
	g_tempResources[g_frame].push_back(args.resource);
}

bool DestructResourceMidAsmHook(X86Context& __restrict ctx, uint8_t* base)
{
	if (ctx.esi.u32 == 0)
		return false;


	auto resource = (GuestResource*)g_memory.Translate(ctx.esi.u32);

	switch (resource->type)
	{
	case ResourceType::Texture:
	case ResourceType::VolumeTexture:
	{
		const auto texture = reinterpret_cast<GuestTexture*>(resource);

		// cancel any queued resize, otherwise BeginCommandList recreates a texture that is
		// already about to get destroyed.
		{
			std::lock_guard lock(g_surfaceRTsMutex);
			g_resizableTextures.erase(texture);
		}

		texture->shouldFreeMemory = true;

	}
	break;
	case ResourceType::RenderTarget:
	case ResourceType::DepthStencil:
	{
		std::lock_guard lock(g_surfaceRTsMutex);
		g_resizableSurfaces.erase(static_cast<GuestSurface*>(resource));
		break;
	}
	default:

		return false; // not something the host allocated so leave it alone, the guest will free it.

	}

	AddDestructResource(resource);
	return true;
}


X_D3DFORMAT GetXboxPixelContainerFormat(const uint32_t XboxPixelContainer_Format)
{
	X_D3DFORMAT d3d_format = (X_D3DFORMAT)((XboxPixelContainer_Format & X_D3DFORMAT_FORMAT_MASK) >> X_D3DFORMAT_FORMAT_SHIFT);
	return d3d_format;
}

// this seems to be used by FMV

unsigned int GetPixelContainerMipMapLevels(X_D3DPixelContainer* pPixelContainer);
static void CopyDynamicTextureToGPU(GuestTexture* tex)
{
	auto base = g_memory.base;
	auto format = GetXboxPixelContainerFormat((X_D3DFORMAT)tex->guestResource.Format);
	bool isSwizzled = FormatIsSwizzled(format);
	bool bCompressed = XBFormatIsCompressed(format);
	bool bCubemap = tex->guestResource.Format & X_D3DFORMAT_CUBEMAP;

	struct Slice
	{
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t srcOffset;
		uint32_t dstOffset;
		uint32_t srcRowPitch;
		uint32_t dstRowPitch;
		uint32_t rowCount;
	};

	std::vector<Slice> slices;
	uint32_t curSrcOffset = 0;
	uint32_t curDstOffset = 0;
	uint32_t baseW = tex->width;
	uint32_t baseH = tex->height;
	uint32_t baseD = tex->depth == 0 ? 1 : tex->depth;

	UINT dwMipMapLevels = 1;
	D3DCUBEMAP_FACES last_face = (bCubemap) ? D3DCUBEMAP_FACE_NEGATIVE_Z : D3DCUBEMAP_FACE_POSITIVE_X;
	DWORD blockSize = 0;

	if (bCompressed)
		blockSize = format == X_D3DFMT_DXT1 ? 8 : 16;

#ifdef __ANDROID__
	// This path is the one of videos, which are not compressed. A compressed texture whose
	// host texture is a plain one (see CreateTexture) has no data here that would fit it.
	if (bCompressed && RenderFormatBlockWidth(tex->format) <= 1)
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			LOGN_WARNING("A block-compressed texture that the game writes to directly is not unpacked on this GPU and will not show.");
		}

		return;
	}
#endif

	// Build slice descriptors 
	for (int32_t face = D3DCUBEMAP_FACE_POSITIVE_X; face <= last_face; face++)
	{
		for (uint32_t mipmap_level = 0; mipmap_level < dwMipMapLevels; mipmap_level++)
		{
			auto& slice = slices.emplace_back();
			slice.width = std::max(1u, baseW >> mipmap_level);
			slice.height = std::max(1u, baseH >> mipmap_level);
			slice.depth = std::max(1u, baseD >> mipmap_level);

			if (bCompressed)
			{
				uint32_t blockCols = (slice.width + 3) / 4;
				slice.srcRowPitch = blockCols * blockSize;
				slice.rowCount = (slice.height + 3) / 4;
			}
			else
			{
				uint32_t bpp = XBFormatBitsPerPixel(format);
				uint32_t srcBytesPerRow = (slice.width * bpp) / 8;
				slice.srcRowPitch = srcBytesPerRow;
				slice.rowCount = slice.height;
			}

			uint32_t mip2dSize = slice.srcRowPitch * slice.rowCount;
			uint32_t mipSlicePitch = mip2dSize * slice.depth;
			slice.srcOffset = curSrcOffset;

			uint32_t dstBpp = XBFormatBitsPerPixel(format);
			uint32_t rawDstRowPitch = (slice.width * dstBpp) / 8;
			slice.dstRowPitch = (rawDstRowPitch + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
			slice.dstOffset = curDstOffset;

			curSrcOffset += mipSlicePitch;
			uint32_t alignedSliceSize = (slice.dstRowPitch * slice.rowCount * slice.depth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
			curDstOffset += alignedSliceSize;
		}
	}


	// this function should only be used for FMV and nothing else, but just in case, we can have
	// a small heuristic to ensure it's an FMV texture.
	tex->isFmv = isSwizzled && tex->depth == 1 && tex->width > 256 && tex->height > 256 && slices.size() == 1; // fmv tex should never use mipmap.


	auto allocation = g_uploadAllocators[g_frame].allocate(curDstOffset, PLACEMENT_ALIGNMENT);
	uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(allocation.memory);

	// Copy/unswizzle data into ring buffer
	for (auto& slice : slices)
	{

		uint8_t* srcData = reinterpret_cast<uint8_t*>(tex->mappedMemory);
		uint8_t* dstData = mappedMemory + slice.dstOffset;


		if (isSwizzled)
		{
			uint32_t dstSlicePitch = slice.dstRowPitch * slice.rowCount;
			uint32_t srcBpp = bCompressed ? blockSize : (XBFormatBitsPerPixel(format) / 8);

			UnswizzleBox(srcData, slice.width, slice.height, slice.depth, srcBpp, dstData, slice.dstRowPitch, dstSlicePitch);
		}
		else if (bCompressed)
		{
			memcpy(dstData, srcData, slice.srcRowPitch * slice.rowCount * slice.depth);
		}
		else
		{
			if (slice.srcRowPitch == slice.dstRowPitch)
			{
				memcpy(dstData, srcData, slice.srcRowPitch * slice.rowCount * slice.depth);
			}
			else
			{
				for (size_t i = 0; i < slice.rowCount * slice.depth; i++)
				{
					memcpy(dstData, srcData, slice.srcRowPitch);
					srcData += slice.srcRowPitch;
					dstData += slice.dstRowPitch;
				}
			}
		}
	}

	auto copyTextureRegion = [&](Slice& slice, uint32_t subresourceIndex)
		{
			uint32_t uploadRowPitchInTexels = slice.dstRowPitch /
				RenderFormatSize(tex->format) * RenderFormatBlockWidth(tex->format);

			g_commandLists[g_frame]->copyTextureRegion(
				RenderTextureCopyLocation::Subresource(tex->texture,
					subresourceIndex % dwMipMapLevels, subresourceIndex % dwMipMapLevels),
				RenderTextureCopyLocation::PlacedFootprint(allocation.buffer, tex->format,
					slice.width, slice.height, slice.depth, uploadRowPitchInTexels,
					allocation.offset + slice.dstOffset));
		};

	for (size_t i = 0; i < slices.size(); i++)
		copyTextureRegion(slices[i], i);

	if (bCubemap)
	{
		for (size_t i = 1; i < 6; i++)
		{
			for (size_t j = 0; j < slices.size(); j++)
				copyTextureRegion(slices[j], (slices.size() * i) + j);
		}
	}
}

static void ProcUnlockTextureRect(const RenderCommand& cmd)
{
	const auto& args = cmd.unlockTextureRect;

	AddBarrier(args.texture, RenderTextureLayout::COPY_DEST);
	FlushBarriers();

	CopyDynamicTextureToGPU(args.texture);
}

// The two CRI ADX service threads that tick on the vblank
static constexpr uint32_t ADXM_VSYNC_THREAD = 0x3291E0;
static constexpr uint32_t ADXM_FS_THREAD = 0x329260;

// This function is needed for ADX and FMV decoding, as they rely on the vblank
// timing to update their buffers. We fake a vblank signal; sleep-only pacing
// keeps the decoding threads off the CPU.
//
// ADX servers are the exception and must stay at 60 Hz: mwlRnaInsrtSlntDat (0x331300) is capped at 
// 1024 frames a call, so below ~43 FPS it cannot keep up with 44100 Hz. 
void CRI_BlockUntilVerticalBlank()
{
	// Per-thread pacer: each decoding thread holds its own vblank phase, and
	// the pacer's oversleep compensation keeps the period drift-free.
	thread_local FramePacer vblankPacer;

	const uint32_t entry = GuestThread::GetCurrentThreadEntry();
	const bool isAdxServiceThread = entry == ADXM_VSYNC_THREAD || entry == ADXM_FS_THREAD;

	const double vblankHz = std::max(GetTargetFPS(), 60.0); // account for both FMV at 30 fps and higher than 60

	vblankPacer.SetFpsLimit(isAdxServiceThread ? 60.0 : vblankHz);
	vblankPacer.EndFrame(FramePacer::WaitMode::SleepOnly);
}


static void ProcSetViewport(const RenderCommand& cmd)
{
	const auto& args = cmd.setViewport;

	RenderViewport* VP = &g_renderviewport.vp;
	RenderViewport_r* VPR = &g_renderviewport;
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->x, args.x);
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->y, args.y);
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->width, args.width);
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->height, args.height);
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->minDepth, args.minDepth);
	SetDirtyValue<float>(g_dirtyStates.viewport, VP->maxDepth, args.maxDepth);
	SetDirtyValue<bool>(g_dirtyStates.viewport, VPR->isFixed, args.isFixed);

	uint32_t specConstants = g_pipelineState.specConstants;

	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants, specConstants);

	g_dirtyStates.scissorRect |= g_dirtyStates.viewport;
}


float GetZScaleForPixelContainer(X_D3DPixelContainer* pSurface)
{
	// If no surface was present, fallback to 1
	if (pSurface == nullptr)
		return 1.0f;

	auto format = pSurface->Format;

	if (format > 50)
		format = GetXboxPixelContainerFormat(pSurface->Format);

	switch (format)
	{
	case X_D3DFMT_D16:
	case X_D3DFMT_LIN_D16:
		return 65535.0f;

	case X_D3DFMT_D24S8:
	case X_D3DFMT_LIN_D24S8:
		return 16777215.0f;

	case X_D3DFMT_F16:
	case X_D3DFMT_LIN_F16:
		return 511.9375f;

	case X_D3DFMT_F24S8:
	case X_D3DFMT_LIN_F24S8:
		// 24bit floating point is close to precision maximum, so a lower value is used
		// We can't use a double here since the vertex shader is only at float precision
		return 1.0e30f;
	}

	// Default to 1 if unknown depth format
	LOG_ERROR("Unknown Xbox Depth Format");
	return 1.0f;
}


void UpdateHostTextureScaling(GuestTexture* tex, const bool isFixedFunc)
{

	// Xbox works with "Linear" and "Swizzled" texture formats
	// Linear formats are not addressed with normalized coordinates
	// We want to use normalized coordinates in our shaders, so need to be able to scale the coordinates back
	// Note texcoords aren't only used for texture lookups
	// TODO store scaling per texture instead of per stage, and scale during lookup in the pixel shader

	// Each texture stage has one texture coordinate set associated with it
	// We'll store scale factors for each texture coordinate set
	std::array<std::array<float, 4>, X_D3DTS_STAGECOUNT> texcoordScales;
	texcoordScales.fill({ 1, 1, 1, 1 });

	for (int stage = 0; stage < X_D3DTS_STAGECOUNT; stage++)
	{
		auto pXboxBaseTexture = g_textures[stage];

		// No texture, no scaling to do
		if (pXboxBaseTexture == xbox::zeroptr)
		{
			continue;
		}

		// Texcoord index. Just the texture stage unless fixed function mode
		int texCoordIndex = stage;
		if (isFixedFunc)
		{
			// Get TEXCOORDINDEX for the current texture stage's state
			// Stores both the texture stage index and information for generating coordinates
			// See D3DTSS_TEXCOORDINDEX
			auto texCoordIndexState = g_sharedConstants.TEXCOORDINDEX[stage];

			// If coordinates are generated, we don't have to worry about the coordinates coming from the title
			bool isGenerated = texCoordIndexState >= X_D3DTSS_TCI_CAMERASPACENORMAL;
			if (isGenerated)
				continue;

			// Determine the texture coordinate addressing this texture stage
			texCoordIndex = (texCoordIndexState & 0x3); // 0 - 3
		}

		auto texCoordScale = &texcoordScales[texCoordIndex];

		// Check for active linear textures.
		X_D3DFORMAT XboxFormat = GetXboxPixelContainerFormat(pXboxBaseTexture->guestResource.Format);
		if (FormatIsLinear(XboxFormat))
		{
			float width, height;
			width = (float)pXboxBaseTexture->width;
			height = (float)pXboxBaseTexture->height;

			*texCoordScale = {
				width,
				height,
				1.0f, // TODO should this be mip levels for volume textures?
				1.0f
			};
		}

		// When a depth buffer is used as a texture
		// We do 'Native Shadow Mapping'
		// https://aras-p.info/texts/D3D9GPUHacks.html
		// The z texture coordinate component holds a depth value, which needs to be normalized
		// TODO implement handling for
		// - X_D3DRS_SHADOWFUNC
		// - X_D3DRS_POLYGONOFFSETZSLOPESCALE
		// - X_D3DRS_POLYGONOFFSETZOFFSET
		if (FormatIsDepthBuffer(XboxFormat))
		{
			(*texCoordScale)[2] = (float)GetZScaleForPixelContainer((X_D3DPixelContainer*)&pXboxBaseTexture->guestResource);
		}
	}

	// Pass above determined texture scaling factors to our HLSL shader.
	// Note : the current vertex shader applies texture scaling on
	// VS_OUTPUT registers oT0 to oT3. It may be needed to move the scaling
	// and apply it on input registers instead. In that case, we'd have to
	// figure out which registers are used to pass texture-coordinates into
	// the shader and allow scaling on any of the input registers (so we'd
	// need to allow scaling on all 16 attributes, instead of just the four
	// textures like we do right now).
	// 

	CopyVertexShaderConstant(HOST_D3DVS_TEXTURES_SCALE_BASE, (DWORD*)texcoordScales.data(), HOST_D3DVS_TEXTURES_SCALE_SIZE);
}

static void SetTextureInRenderThread(uint32_t index, GuestTexture* texture, const bool isFixed = false)
{
	AddBarrier(texture, RenderTextureLayout::SHADER_READ);

	auto viewDimension = texture != nullptr ? texture->viewDimension : RenderTextureViewDimension::UNKNOWN;

	uint32_t sampleType;
	if (texture)
	{
		sampleType = (uint32_t)(viewDimension)-1;
		UpdateHostTextureScaling(texture, isFixed);
	}
	else
	{
		sampleType = 0;
	}

	bool selectorDirty = false;
	SetDirtyValue(selectorDirty, g_sharedConstants.TextureSampleType[index], sampleType);
	MarkSharedPixelConstantsDirty(selectorDirty, index < FixedFunctionPixelShaderState::TextureStageCount);

	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture2DIndices[index],
		viewDimension == RenderTextureViewDimension::TEXTURE_2D ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D);

	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture3DIndices[index], texture != nullptr &&
		viewDimension == RenderTextureViewDimension::TEXTURE_3D ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D);

	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureCubeIndices[index], texture != nullptr &&
		viewDimension == RenderTextureViewDimension::TEXTURE_CUBE ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE);

	float texCoordMinX = 0.0f;
	float texCoordMinY = 0.0f;
	float texCoordMaxX = 0.0f;
	float texCoordMaxY = 0.0f;

	if (texture != nullptr)
	{
		// Render Targets carry an unwritten border from the half-pixel viewport offset
		// We can patch this by telling the pixel shader where the usable area start and skip the dead borders.

		if (texture->isRenderTarget)
		{
			// Helper function to get where the new area start.
			// The border is 0.5 * scale host texels wide.
			const auto getFirstLiveTexel = [](uint32_t hostSize, uint32_t guestSize) -> float
				{
					if (hostSize == 0 || guestSize == 0)
						return 0.0f;

					const float scale = float(hostSize) / float(guestSize);
					const float deadTexels = std::ceil(0.5f * scale - 0.5f);

					return (deadTexels + 0.5f) / float(hostSize);
				};

			texCoordMinX = getFirstLiveTexel(texture->width, texture->g_width);
			texCoordMinY = getFirstLiveTexel(texture->height, texture->g_height);

			// partly drawn case (bottom banding bs)
			const auto getLastLiveTexel = [](uint32_t hostSize, uint32_t guestSize, uint32_t liveSize) -> float
				{
					if (hostSize == 0 || liveSize == 0 || liveSize >= guestSize)
						return FLT_MAX;

					const float scale = float(hostSize) / float(guestSize);

					return (std::floor(float(liveSize) * scale) - 0.5f) / float(hostSize);
				};

			const float lastLiveX = getLastLiveTexel(texture->width, texture->g_width, texture->liveWidth);
			const float lastLiveY = getLastLiveTexel(texture->height, texture->g_height, texture->liveHeight);

			if (lastLiveX != FLT_MAX || lastLiveY != FLT_MAX)
			{
				texCoordMaxX = lastLiveX;
				texCoordMaxY = lastLiveY;
			}
		}
		else if (texture->isFmv)
		{
			// FMV surface is 1024x512, but the video region is 640x480, since we upload the whole surface,
			// we need to offset the texcoords by half a texel to avoid sampling garbage from the padding.
			// (basically keep the filter kernel inside the video region, so the wrap around can no longer be reached.)
			// Most FMVs don't have this issue, but I have seen it happen when the game play an event before an FMV, like the ark firing the laser.
			texCoordMinX = 0.5f / float(texture->width);
			texCoordMinY = 0.5f / float(texture->height);

			// The far edge has the same problem at high resolution (4k+), so clamp to the last texel center.
			const uint32_t frameWidth = g_fmvFrameWidth.load();
			const uint32_t frameHeight = g_fmvFrameHeight.load();

			if (frameWidth != 0 && frameHeight != 0 && frameWidth <= texture->width && frameHeight <= texture->height)
			{
				texCoordMaxX = (float(frameWidth) - 0.5f) / float(texture->width);
				texCoordMaxY = (float(frameHeight) - 0.5f) / float(texture->height);
			}
		}
	}

	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texCoordMin[index][0], texCoordMinX);
	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texCoordMin[index][1], texCoordMinY);
	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texCoordMax[index][0], texCoordMaxX);
	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texCoordMax[index][1], texCoordMaxY);
}


static void SetViewport(GuestViewport* viewport, GuestSurface* surface)
{
	if (surface != nullptr && viewport != nullptr)
	{
		const bool isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;

		// it's easier to let the game think we're still on the OG resolution, so use guest res.
		uint32_t rtWidth = surface->g_width;
		uint32_t rtHeight = surface->g_height;

		//for some reason, og xbox can send int_max for width and height, likely to trigger a reset(?)
		float width = (float)std::min(viewport->Width, (DWORD)rtWidth - viewport->X);
		float height = (float)std::min(viewport->Height, (DWORD)rtHeight - viewport->Y);


		RenderCommand cmd;
		cmd.type = RenderCommandType::SetViewport;
		cmd.setViewport.x = viewport->X;
		cmd.setViewport.y = viewport->Y;
		cmd.setViewport.width = width;
		cmd.setViewport.height = height;
		cmd.setViewport.minDepth = viewport->MinZ;
		cmd.setViewport.maxDepth = viewport->MaxZ;
		cmd.setViewport.isFixed = isFixed;
		g_gameViewport = RenderViewport(cmd.setViewport.x, cmd.setViewport.y, width, height, viewport->MinZ, viewport->MaxZ);
		g_pendingRenderCommands.enqueue() = cmd;
	}
}

// The game doesn't create the backbuffer and depth surface in CreateSurface / CreateTexture like the other surfaces
// Instead it does it in InitializeFramebuffers, since the function is complex we can't just blindly replace it.
// So we let the code run instead, then copy each header into a host surface which we will give to the guest device directly.
static constexpr uint32_t GuestDeviceBackBufferOffset = 0x1A14;
static constexpr uint32_t GuestDeviceAutoDepthOffset = 0x1A20;
static constexpr uint32_t GuestDeviceZScaleOffset = 0x948;

static void ReplaceDeviceSurface(uint32_t device, uint32_t offset, GuestSurface* newSurface)
{
	auto base = g_memory.base;
	X_D3DSurface* surface = (X_D3DSurface*)g_memory.Translate(X86_LOAD_U32(device + offset));
	newSurface->guestResource.Common = surface->Common;
	newSurface->guestResource.Data = surface->Data;
	newSurface->guestResource.Format = surface->Format;
	newSurface->guestResource.Lock = surface->Lock;
	newSurface->guestResource.Size = surface->Size;
	newSurface->padding = surface->Parent;

	X86_STORE_U32(device + offset, g_memory.MapVirtual(newSurface));
}

//D3D8::CDevice_InitializeFrameBuffers 
// (this function create backbuffer and stencil)
X86_FUNC_IMPL(__imp__sub_3D5140);
X86_OVERRIDE_FUNC(sub_3D5140)
{
	uint32_t device = ctx.ecx.u32;
	__imp__sub_3D5140(ctx, base);

	if ((int32_t)ctx.eax.u32 < 0)
		return;

	ReplaceDeviceSurface(device, GuestDeviceBackBufferOffset, g_backBuffer);

	if (X86_LOAD_U32(device + GuestDeviceAutoDepthOffset))
	{
#ifdef __ANDROID__
		// The depth buffer drawn with into the back buffer too: see CoverBackBuffer().
		g_defaultStencil = CreateSurface(512, 512, X_D3DFMT_LIN_D24S8, false, true);
#else
		g_defaultStencil = CreateSurface(512, 512, X_D3DFMT_LIN_D24S8);
#endif
		ReplaceDeviceSurface(device, GuestDeviceAutoDepthOffset, g_defaultStencil);
	}
}

// The main thread and the loading thread can both run SetRenderTarget at the same time, thus, they can read
// the same old surface and release it twice, so memory can get corrupted, let's mutex this as a fix.
static std::recursive_mutex g_guestRefCountMutex;

//Specify where it should draws (Either framebuffer surface or texture surface)
//on DX8 Xbox, SetDepthStencilSurface is merged with SetRenderTarget.
X86_FUNC_IMPL(__imp__sub_3D0040);
void D3DDevice_SetRenderTarget(X86Context& ctx, GuestSurface* surface, GuestSurface* pNewZStencil)
{
	std::lock_guard lock(g_guestRefCountMutex);

	auto base = g_memory.base;
	auto device = X86_LOAD_U32(GuestDevicePtrAddr);

	GuestSurface* newSurface = surface;
	GuestSurface* newDepthStencil = pNewZStencil;

	__imp__sub_3D0040(ctx, base);
	ctx.SubEsp(3);

	// The game calls this function with no depth buffer for render target composites.
	// The previous code here was making g_ZScale fallback to 1.0 in that case. After that, the shader was doing the math
	// z * 16777215 / 1 which made any value above 0 went way paste 1, resulting to no geometry visible in-game.
	// Rather than hardcode a cap on the shader like before, we can fall back to the old code only if g_ZScale is 0 or less.

	g_ZScale = X86_LOAD_F32(device + GuestDeviceZScaleOffset);
	if (g_ZScale <= 0.0f)
		g_ZScale = GetZScaleForPixelContainer(pNewZStencil != nullptr ? (X_D3DPixelContainer*)&pNewZStencil->guestResource : nullptr);

	RenderCommand cmd;
	cmd.type = RenderCommandType::SetRenderTarget;
	cmd.setRenderTarget.renderTarget = newSurface;
	g_pendingRenderCommands.enqueue() = cmd;

	static GuestViewport defaultViewport;
	defaultViewport.X = 0;
	defaultViewport.Y = 0;
	defaultViewport.Width = INT32_MAX;
	defaultViewport.Height = INT32_MAX;
	defaultViewport.MinZ = 0;
	defaultViewport.MaxZ = 1;

	SetViewport(&defaultViewport, newSurface);

	RenderCommand cmd2;
	cmd2.type = RenderCommandType::SetDepthStencilSurface;
	cmd2.setDepthStencilSurface.depthStencil = newDepthStencil;
	g_pendingRenderCommands.enqueue() = cmd2;

	g_next_RenderTarget = newSurface;
	MarkFixedFunctionVertexShaderStateDirty();
}

static void ProcSetRenderTarget(const RenderCommand& cmd)
{
	const auto& args = cmd.setRenderTarget;

	if (g_renderTarget != nullptr && g_renderTarget != args.renderTarget) //due to extensive ping pong with re use of previous RTs, we need to force shader_read to avoid race issues.
	{
		if (g_renderTarget->layout == RenderTextureLayout::COLOR_WRITE)
		{
			AddBarrier(g_renderTarget, RenderTextureLayout::SHADER_READ);
		}
	}

	SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_renderTarget, args.renderTarget);
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.renderTargetFormat, args.renderTarget != nullptr ? args.renderTarget->format : RenderFormat::UNKNOWN);
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.sampleCount, args.renderTarget != nullptr ? args.renderTarget->sampleCount : RenderSampleCount::COUNT_1);

	// Clear / Zero each RT once per frame before anything draws into it
	// This will wipe last frame of bloom leftover.
	// The game never really clear this surface itself, it relies on background covering the viewport
	// which is often incorrect (like when a stage load.)
	if (args.renderTarget != nullptr && args.renderTarget->texture != nullptr &&
		(CLEAR_BACKBUFFER || args.renderTarget != g_backBuffer) &&
		g_clearedRenderTargetsThisFrame.insert(args.renderTarget->texture).second)
	{
		AddBarrier(args.renderTarget, RenderTextureLayout::COLOR_WRITE);
		FlushBarriers();

		if (args.renderTarget->texture != nullptr)
		{
			SetFramebuffer(args.renderTarget, nullptr, true);
			g_commandLists[g_frame]->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
		}

		// re-measure in this case
		if (args.renderTarget->parentTexture != nullptr)
		{
			args.renderTarget->parentTexture->liveAreaReset = true;
		}
	}
}

static void ProcSetDepthStencilSurface(const RenderCommand& cmd)
{
	const auto& args = cmd.setDepthStencilSurface;

	SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_depthStencil, args.depthStencil);
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthStencilFormat, args.depthStencil != nullptr ? args.depthStencil->format : RenderFormat::UNKNOWN);
}

X86_FUNC_IMPL(__imp__sub_3D0680);
void D3DDevice_SetViewport(X86Context& ctx, GuestViewport* viewport)
{

	//call original func first
	auto base = g_memory.base;
	__imp__sub_3D0680(ctx, base);
	ctx.esp.u32 -= 8;

	if (viewport == nullptr) //they can call this with a null viewport for some reason
	{
		return;
	}

	SetViewport(viewport, g_next_RenderTarget);
}

static void SetFramebuffer(GuestSurface* renderTarget, GuestSurface* depthStencil, bool settingForClear)
{
	if (settingForClear || g_dirtyStates.renderTargetAndDepthStencil)
	{
		//printf("SetFramebuffer %x %x\n", renderTarget, depthStencil);
		GuestSurface* framebufferContainer = nullptr;
		RenderTexture* framebufferKey = nullptr;

		if (renderTarget != nullptr && depthStencil != nullptr)
		{
			framebufferContainer = depthStencil; // Backbuffer texture changes per frame so we can't use the depth stencil as the key.
			framebufferKey = renderTarget->texture;
		}
		else if (renderTarget != nullptr && depthStencil == nullptr)
		{
			framebufferContainer = renderTarget;
			framebufferKey = renderTarget->texture; // Backbuffer texture changes per frame so we can't assume nullptr for it.
		}
		else if (renderTarget == nullptr && depthStencil != nullptr)
		{
			framebufferContainer = depthStencil;
			framebufferKey = nullptr;
		}

		auto& commandList = g_commandLists[g_frame];

		if (framebufferContainer != nullptr)
		{
			auto& framebuffer = framebufferContainer->framebuffers[framebufferKey];

			if (framebufferKey != nullptr)
				g_framebufferCacheUsers[framebufferKey].insert(framebufferContainer);

			uint64_t textureGeneration = 0;
			if (renderTarget != nullptr)
			{
				textureGeneration = renderTarget->textureGeneration;
			}
			else if (depthStencil != nullptr)
			{
				textureGeneration = depthStencil->textureGeneration;
			}

			if (framebuffer.framebuffer == nullptr || framebuffer.generation != textureGeneration)
			{
				RenderFramebufferDesc desc;

				if (renderTarget != nullptr)
				{
					desc.colorAttachments = const_cast<const RenderTexture**>(&renderTarget->texture);
					desc.colorAttachmentsCount = 1;
				}

				if (depthStencil != nullptr)
					desc.depthAttachment = depthStencil->texture;

#ifdef __ANDROID__
				if (renderTarget != nullptr && depthStencil != nullptr)
					AndroidVideo::CheckFramebufferSizes(renderTarget->width, renderTarget->height, depthStencil->width, depthStencil->height);
#endif

				framebuffer = { g_device->createFramebuffer(desc), textureGeneration };
			}

			if (g_framebuffer != framebuffer.framebuffer.get())
			{
				commandList->setFramebuffer(framebuffer.framebuffer.get());
				g_framebuffer = framebuffer.framebuffer.get();
#ifdef __ANDROID__
				g_framebufferChangeCount++;
#endif
			}
		}
		else if (g_framebuffer != nullptr)
		{
			commandList->setFramebuffer(nullptr);
			g_framebuffer = nullptr;
		}


		g_dirtyStates.renderTargetAndDepthStencil = settingForClear;
	}
}

static void ProcClear(const RenderCommand& cmd)
{
	const auto& args = cmd.clear;

	AddBarrier(g_renderTarget, RenderTextureLayout::COLOR_WRITE);
	AddBarrier(g_depthStencil, RenderTextureLayout::DEPTH_WRITE);
	FlushBarriers();

	bool canClearInOnePass = (g_renderTarget == nullptr) || (g_depthStencil == nullptr) ||
		(g_renderTarget->width == g_depthStencil->width && g_renderTarget->height == g_depthStencil->height);

#ifdef __ANDROID__
	// The depth buffer of the game covers its colour targets without being of their size
	// (CoverBackBuffer()). All of it is cleared, as below, in a frame buffer of its own.
	// The colour target is then cleared in the frame buffer the two share, which is the one
	// the draws that follow a clear mostly use: one change of frame buffer less than
	// clearing each on its own, and every change makes the tile-based GPU of a phone
	// write its targets out and read the next ones in.
	if (!canClearInOnePass && g_depthStencil->width >= g_renderTarget->width && g_depthStencil->height >= g_renderTarget->height)
	{
		auto& clearCommandList = g_commandLists[g_frame];

		// Stencil clears are ignored: our D32_FLOAT textures only store depth.
		if ((args.flags & X_D3DCLEAR_ZBUFFER) != 0)
		{
			SetFramebuffer(nullptr, g_depthStencil, true);
			clearCommandList->clearDepthStencil(true, false, args.z, args.stencil);
		}

		if ((args.flags & X_D3DCLEAR_TARGET) != 0)
		{
			SetFramebuffer(g_renderTarget, g_depthStencil, true);
			clearCommandList->clearColor(0, RenderColor(args.color[0], args.color[1], args.color[2], args.color[3]));
		}

		return;
	}
#endif

	if (canClearInOnePass)
	{
		SetFramebuffer(g_renderTarget, g_depthStencil, true);
	}

	auto& commandList = g_commandLists[g_frame];

	if (g_renderTarget != nullptr && (args.flags & X_D3DCLEAR_TARGET) != 0)
	{
		if (!canClearInOnePass) {
			SetFramebuffer(g_renderTarget, nullptr, true);
		}

		commandList->clearColor(0, RenderColor(args.color[0], args.color[1], args.color[2], args.color[3]));
	}

	const bool clearDepth = (args.flags & X_D3DCLEAR_ZBUFFER) != 0;
	// Ignore stencil clears: our D32_FLOAT textures only store depth.
	if (g_depthStencil != nullptr && clearDepth)
	{
		if (!canClearInOnePass) {
			SetFramebuffer(nullptr, g_depthStencil, true);
		}

		commandList->clearDepthStencil(true, false, args.z, args.stencil);
	}
}


//Clears one or more surfaces such as a render target, multiple render targets, a stencil buffer, and a depth buffer.
void D3DDevice_Clear(uint32_t count, _D3DRECT* pRects, uint32_t flags, uint32_t Color, float Z, uint32_t Stencil)
{
	RenderCommand cmd;
	cmd.type = RenderCommandType::Clear;
	cmd.clear.flags = flags;
	cmd.clear.isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;

	//extract the RGBA components to convert the color to float
	float color[4] = { ((Color >> 16) & 0xFF) / 255.f,
				  ((Color >> 8) & 0xFF) / 255.f, (Color & 0xFF) / 255.f,
				  ((Color >> 24) & 0xFF) / 255.f };

	cmd.clear.color[0] = color[0];
	cmd.clear.color[1] = color[1];
	cmd.clear.color[2] = color[2];
	cmd.clear.color[3] = color[3];


	cmd.clear.z = float(Z);
	cmd.clear.stencil = Stencil;

	const bool clearDepth = (flags & X_D3DCLEAR_ZBUFFER) != 0;
	const bool clearStencil = (flags & X_D3DCLEAR_STENCIL) != 0;
	SendRenderCommand(cmd);
}

//D3DDevice_SetTransform
X86_FUNC_IMPL(__imp__sub_3CFCD0);
X86_OVERRIDE_FUNC(sub_3CFCD0)
{
	X_D3DTRANSFORMSTATETYPE State = (X_D3DTRANSFORMSTATETYPE)X86_LOAD_U32(ctx.esp.u32 + 4);
	auto matrixAddr = X86_LOAD_U32(ctx.esp.u32 + 8);
	D3DMATRIX* pMatrix = (D3DMATRIX*)g_memory.Translate(matrixAddr);

	//we let the original code run first (todo check if that's needed)
	__imp__sub_3CFCD0(ctx, base);

	if (memcmp(&g_Transforms[State], pMatrix, sizeof(*pMatrix)) == 0)
		return;

	g_Transforms[State] = *pMatrix;

	// World and texture matrix updates only feed the transform registers;
	// other kinds also affect lighting, fog or the shader variant.
	if ((X_D3DTS_TEXTURE0 <= State && State <= X_D3DTS_TEXTURE3) ||
		(X_D3DTS_WORLD <= State && State <= X_D3DTS_WORLD3))
		MarkFixedFunctionVertexShaderTransformsDirty();
	else
		MarkFixedFunctionVertexShaderStateDirty();

	if (State == X_D3DTS_VIEW)
	{
		bWorldViewDirty.fill(true);
		MarkFixedFunctionVertexShaderLightsDirty();
	}
	if ((X_D3DTS_WORLD <= State) && (State <= X_D3DTS_WORLD3))
	{
		bWorldViewDirty[State - X_D3DTS_WORLD] = true;
	}
	if (State == X_D3DTS_PROJECTION)
		MarkFixedFunctionVertexShaderVariantDirty();
}


int D3DDevice_Swap(X86Context& ctx, int32_t a1)
{
	Video::Present(&ctx);
	return 0;
}

static void SetSurface(uint32_t index, GuestSurface* surface)
{
	AddBarrier(surface, RenderTextureLayout::SHADER_READ);

	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture2DIndices[index], surface->descriptorIndex);
	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture3DIndices[index], uint32_t(TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D));
	SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureCubeIndices[index], uint32_t(TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE));
}

static void ProcSetTexture(const RenderCommand& cmd)
{
	const auto& args = cmd.setTexture;

	// If a pending copy operation is detected, set the source surface. The indices will be fixed later if flushing is necessary.
	bool shouldSetTexture = true;
	if (args.texture != nullptr)
	{
		if (args.texture->sourceSurface != nullptr)
		{
			//UpdateHostTextureScaling(args.texture, g_pipelineState.vertexShaderType == VertexShaderMode::FixedFunction);
			SetSurface(args.index, args.texture->sourceSurface);
			shouldSetTexture = false;
		}
	}

	if (shouldSetTexture)
		SetTextureInRenderThread(args.index, args.texture, args.isFixed);

	g_textures[args.index] = args.texture;
}

static void ProcAddPipeline(const RenderCommand& cmd)
{
	auto& pipeline = g_pipelines[cmd.addPipeline.hash];
	if (pipeline == nullptr)
	{
		pipeline.reset(cmd.addPipeline.pipeline);
	}
	else
	{
		delete cmd.addPipeline.pipeline;
	}
}

static RenderPipeline* CreateGraphicsPipelineInRenderThread(PipelineState pipelineState)
{
	RtScopeTimer _psoScope(g_rtPsoWorkAccum);
	SanitizePipelineState(pipelineState);

	// If the sanitized state matches the last draw's state, reuse the cached pipeline pointer.
	// Skips g_pipelines map lookup entirely.
	static PipelineState s_lastSanitizedState{};
	static RenderPipeline* s_lastPipeline = nullptr;

	if (s_lastPipeline != nullptr && std::memcmp(&pipelineState, &s_lastSanitizedState, sizeof(PipelineState)) == 0)
	{
		g_psoCacheHits++;
		return s_lastPipeline;
	}

	g_psoCacheMisses++;

	XXH64_hash_t hash = ComputePipelineStateHash(pipelineState);
	auto& pipeline = g_pipelines[hash];
	if (pipeline == nullptr)
	{
		auto pipeStart = std::chrono::steady_clock::now();
		pipeline = CreateGraphicsPipeline(pipelineState);
		auto pipeElapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipeStart).count();

		LOGFN("CreateGraphicsPipeline: PIPELINE MISS hash={:016X} -> {:.2f} ms",
			hash, pipeElapsed);

		RecordPipelineState(pipelineState);
	}

	s_lastSanitizedState = pipelineState;
	s_lastPipeline = pipeline.get();
	return pipeline.get();
}

struct TextureStageDirtyState
{
	bool any;
	bool fixedFunctionVertex;
};

static TextureStageDirtyState GetTextureStageDirtyState()
{
	if (memcmp(g_dirty_texture_stage_states, g_texture_stage_states, sizeof(g_dirty_texture_stage_states)) == 0)
		return {};

	for (uint32_t i = 0; i < 4; i++)
	{
		const auto offset = i * X_D3DTSS_MAX;
		if (g_dirty_texture_stage_states[offset + X_D3DTSS_TEXTURETRANSFORMFLAGS] !=
			g_texture_stage_states[offset + X_D3DTSS_TEXTURETRANSFORMFLAGS] ||
			g_dirty_texture_stage_states[offset + X_D3DTSS_TEXCOORDINDEX] !=
			g_texture_stage_states[offset + X_D3DTSS_TEXCOORDINDEX])
			return { true, true };
	}

	for (uint32_t i = 0; i < 4; i++)
	{
		for (uint32_t j = 0; j < X_D3DTSS_MAX; j++)
		{
			if (j == X_D3DTSS_TEXTURETRANSFORMFLAGS || j == X_D3DTSS_TEXCOORDINDEX)
				continue;

			auto lastState = g_dirty_texture_stage_states[i * X_D3DTSS_MAX + j];
			int curState = g_texture_stage_states[i * X_D3DTSS_MAX + j];

			if (lastState != curState)
				return { true, false };
		}
	}

	return {};
}

inline int GetXboxPixelContainerDimensionCount(const X_D3DPixelContainer* pXboxPixelContainer)
{
	// Don't pass in unassigned Xbox pixel container
	assert(pXboxPixelContainer != xbox::zeroptr);

	return (X_D3DFORMAT)((pXboxPixelContainer->Format & X_D3DFORMAT_DIMENSION_MASK) >> X_D3DFORMAT_DIMENSION_SHIFT);
}

X_D3DRESOURCETYPE GetXboxTextureType(const X_D3DResource* pXboxResource)
{
	DWORD Format = ((X_D3DPixelContainer*)pXboxResource)->Format;

	if (Format & X_D3DFORMAT_CUBEMAP)
		return X_D3DRTYPE_CUBETEXTURE;

	if (GetXboxPixelContainerDimensionCount((X_D3DPixelContainer*)pXboxResource) > 2)
		return X_D3DRTYPE_VOLUMETEXTURE;

	return X_D3DRTYPE_TEXTURE;
}

void UpdateUserConfigConstant(bool toggle)
{
	// upload user resolution to add HD resolution 
	float userConfig[4];

	userConfig[0] = toggle ? (float)Video::s_viewportWidth : 0.f;
	userConfig[1] = toggle ? (float)Video::s_viewportHeight : 0.f;
	userConfig[2] = toggle ? 1.f : 0.f;
	userConfig[3] = 0.f;
	CopyVertexShaderConstant(HOST_USER_CONFIG, (DWORD*)userConfig, 1);
}

extern uint32_t g_fixedFunctionVertexRegisterDefaultMask;
static void FlushRenderStateForMainThread(LocalRenderCommandQueue& queue)
{
	RtScopeTimer _stateFlushScope(g_gtStateFlushAccum);
	UpdateAllRenderState();
	const auto textureStageDirty = GetTextureStageDirtyState();
	if (textureStageDirty.fixedFunctionVertex)
		MarkFixedFunctionVertexShaderVariantDirty();

	if (g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction)
	{
		// Select the specialized shader for the current controls and forward
		// changes to the render thread.
		const auto variant = UpdateFixedFunctionVertexShaderState((size_t)g_memory.Translate(g_VertexShader_Handle),
			IsFixedFunctionPixelShader(), g_fixedFunctionVertexRegisterDefaultMask);
		if (variant != g_lastSentVertexShaderVariant)
		{
			auto& cmd = queue.enqueue();
			cmd.type = RenderCommandType::SetVertexShaderVariant;
			cmd.setVertexShaderVariant.key = variant;
			g_lastSentVertexShaderVariant = variant;
		}
	}
	else
	{
		UpdateHostViewPortOffsetAndScaleConstants();
	}

	if (textureStageDirty.any)
	{
		auto base = g_memory.base;
		for (uint32_t i = 0; i < 4; i++)
		{
			auto& cmd = queue.enqueue();
			cmd.type = RenderCommandType::SetTextureStageState;
			cmd.SetTextureStageState.index = i;
			cmd.SetTextureStageState.D3DTSS_ADDRESSU = g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ADDRESSU];
			cmd.SetTextureStageState.D3DTSS_ADDRESSV = g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ADDRESSV];

			cmd.SetTextureStageState.D3DTSS_ADDRESSW = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ADDRESSW];
			cmd.SetTextureStageState.D3DTSS_MAGFILTER = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_MAGFILTER];
			cmd.SetTextureStageState.D3DTSS_MINFILTER = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_MINFILTER];
			cmd.SetTextureStageState.D3DTSS_MIPFILTER = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_MIPFILTER];
			cmd.SetTextureStageState.D3DTSS_BORDERCOLOR = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_BORDERCOLOR];

			cmd.SetTextureStageState.D3DTSS_COLOROP = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_COLOROP];
			cmd.SetTextureStageState.D3DTSS_COLORARG0 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_COLORARG0];
			cmd.SetTextureStageState.D3DTSS_COLORARG1 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_COLORARG1];
			cmd.SetTextureStageState.D3DTSS_COLORARG2 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_COLORARG2];
			cmd.SetTextureStageState.D3DTSS_ALPHAOP = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ALPHAOP];
			cmd.SetTextureStageState.D3DTSS_ALPHAARG0 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ALPHAARG0];
			cmd.SetTextureStageState.D3DTSS_ALPHAARG1 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ALPHAARG1];
			cmd.SetTextureStageState.D3DTSS_ALPHAARG2 = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_ALPHAARG2];
			cmd.SetTextureStageState.D3DTSS_RESULTARG = (uint32_t)g_texture_stage_states[i * X_D3DTSS_MAX + X_D3DTSS_RESULTARG];
		}

		memcpy(g_dirty_texture_stage_states, (uint32_t*)g_texture_stage_states, sizeof(uint32_t) * 128);
	}

	HandleShadersConstant(queue);
	SetPixelShaderConstants(queue);

}

static bool isFixedForRenderThread = false;
static constexpr int32_t COMMON_DEPTH_BIAS_VALUE = int32_t((1 << 24) * 0.002f);
static constexpr float COMMON_SLOPE_SCALED_DEPTH_BIAS_VALUE = 1.0f;
static void FlushRenderStateForRenderThread()
{
	RtScopeTimer _flushScope(g_rtFlushWorkAccum);
	const bool isFixed = isFixedForRenderThread;
	static bool lastConstantBankFixed;
	static bool hasLastConstantBank;
	// Switching between the fixed-function and programmable constant banks
	// requires rebinding the other bank's constants.
	if (!hasLastConstantBank || lastConstantBankFixed != isFixed)
	{
		g_dirtyStates.vertexShaderConstants = true;
		lastConstantBankFixed = isFixed;
		hasLastConstantBank = true;
	}
	auto renderTarget = g_pipelineState.colorWriteEnable ? g_renderTarget : nullptr;
	auto depthStencil = g_pipelineState.zEnable || g_pipelineState.stencilEnable ? g_depthStencil : nullptr;

	if (g_pipelineState.isFixedFunctionPS && g_fixedFunctionPixelShaderStateDirty)
	{
		auto state = FixedFunctionPixelShaderVariants::Capture(g_sharedConstants);
		auto variant = FixedFunctionPixelShaderVariants::Select(state);
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.pixelShaderVariant, variant);
		g_fixedFunctionPixelShaderStateDirty = false;
	}

	AddBarrier(renderTarget, RenderTextureLayout::COLOR_WRITE);
	AddBarrier(depthStencil, RenderTextureLayout::DEPTH_WRITE);

	FlushBarriers();

	SetFramebuffer(renderTarget, depthStencil, false);
	FlushViewport(isFixed);


	auto& commandList = g_commandLists[g_frame];

	// D3D12 resets depth bias values to the pipeline values, even if they are dynamic.
	// We can reduce unnecessary calls by making common depth bias values part of the pipeline.
	if (g_capabilities.dynamicDepthBias && !g_vulkan)
	{
		bool useDepthBias = (g_depthBias != 0) || (g_slopeScaledDepthBias != 0.0f);

		int32_t depthBias = useDepthBias ? COMMON_DEPTH_BIAS_VALUE : 0;
		float slopeScaledDepthBias = useDepthBias ? COMMON_SLOPE_SCALED_DEPTH_BIAS_VALUE : 0.0f;

		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthBias, depthBias);
		SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.slopeScaledDepthBias, slopeScaledDepthBias);
	}


	if (g_dirtyStates.pipelineState)
	{
		RenderPipeline* pipeline = CreateGraphicsPipelineInRenderThread(g_pipelineState);

#ifdef __ANDROID__
		// A refused pipeline is not bound, and the draws that need it are left out.
		g_pipelineUsable = AndroidVideo::IsPipelineUsable(pipeline);
		if (g_pipelineUsable)
#endif
			commandList->setPipeline(pipeline);

		// D3D12 resets the depth bias values. Check if they need to be set again.
		if (g_capabilities.dynamicDepthBias && !g_vulkan)
		{
			g_dirtyStates.depthBias = (g_depthBias != g_pipelineState.depthBias) || (g_slopeScaledDepthBias != g_pipelineState.slopeScaledDepthBias);
		}
	}

	if (g_dirtyStates.depthBias && g_capabilities.dynamicDepthBias)
	{
		commandList->setDepthBias(g_depthBias, 0.0f, g_slopeScaledDepthBias);
	}

	if (g_dirtyStates.vertexShaderConstants)
	{
		if (isFixed)
		{
			auto vertexShaderConstants = g_uploadAllocators[g_frame].allocate(g_vertexShaderConstantsFixed, sizeof(g_vertexShaderConstantsFixed), 0x100);
			SetRootDescriptor(vertexShaderConstants, 0);
		}
		else
		{
			auto vertexShaderConstants = g_uploadAllocators[g_frame].allocate(g_vertexShaderConstants, sizeof(g_vertexShaderConstants), 0x100);
			SetRootDescriptor(vertexShaderConstants, 0);
		}
	}

	if (g_dirtyStates.pixelShaderConstants)
	{
		auto pixelShaderConstants = g_uploadAllocators[g_frame].allocate(g_pixelShaderConstants, sizeof(g_pixelShaderConstants), 0x100);
		SetRootDescriptor(pixelShaderConstants, 1);
	}

	if (g_dirtyStates.sharedConstants)
	{
		auto sharedConstants = g_uploadAllocators[g_frame].allocate(&g_sharedConstants, sizeof(g_sharedConstants), 0x100);
		SetRootDescriptor(sharedConstants, 2);

		if (g_vulkan)
		{
			const PixelPushConstants pushConstants = BuildPixelPushConstants(g_sharedConstants);
			commandList->setGraphicsPushConstants(0, &pushConstants, PIXEL_PUSH_CONSTANTS_OFFSET, sizeof(pushConstants));
		}
	}

	if (g_dirtyStates.vertexStreamFirst <= g_dirtyStates.vertexStreamLast)
	{
		commandList->setVertexBuffers(
			g_dirtyStates.vertexStreamFirst,
			g_vertexBufferViews + g_dirtyStates.vertexStreamFirst,
			g_dirtyStates.vertexStreamLast - g_dirtyStates.vertexStreamFirst + 1,
			g_inputSlots + g_dirtyStates.vertexStreamFirst);
	}

	if (g_dirtyStates.indices && (!g_vulkan || g_indexBufferView.buffer.ref != nullptr))
	{
		commandList->setIndexBuffer(&g_indexBufferView);
	}

	g_dirtyStates = DirtyStates(false);
}

static void SetPrimitiveType(uint32_t primitiveType)
{
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.primitiveTopology, ConvertPrimitiveType(primitiveType));
}

static void UnlockIndexBuffer(GuestBuffer* buffer);
static void* LockIndexBuffer(GuestBuffer* buffer, uint32_t flags);
static void ProcSetIndices(const RenderCommand& cmd)
{
	const auto& args = cmd.setIndices;

	SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, args.indexBuffer != nullptr ? args.indexBuffer->buffer->at(0) : RenderBufferReference{});
	SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format, args.indexBuffer != nullptr ? args.indexBuffer->format : RenderFormat::R16_UINT);
	SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, args.indexBuffer != nullptr ? args.indexBuffer->dataSize : 0u);
}

static void SetIndices(LocalRenderCommandQueue& queue, GuestBuffer* buffer)
{
	auto& cmd = queue.enqueue();
	cmd.type = RenderCommandType::SetIndices;
	cmd.setIndices.indexBuffer = buffer;
}

static void* LockBuffer(GuestBuffer* buffer, uint32_t flags)
{
	buffer->lockedReadOnly = (flags & 0x10) != 0;

	if (buffer->mappedMemory == nullptr)
		buffer->mappedMemory = g_userHeap.AllocPhysical(buffer->dataSize, 0x10);


	buffer->isDirty = true;

	if (buffer->isVertexBuffer)
	{
		buffer->guestResource.Data |= 0x80000000;
	}

	return buffer->mappedMemory;
}

//D3DVertexBuffer_Lock2 - DX8 on Xbox doesn't have an unlock function so we will unlock right before rendering
void* LockVertexBuffer(GuestBuffer* buffer, uint32_t flags2)
{
	return LockBuffer(buffer, flags2);
}


// Sends the CPU copy of a guest buffer to the GPU.
// Callers: game thread (useCopyQueue = true) or render thread (false).
//
// Why staging: the game thread is the bottleneck. Staged uploads cost it
// ~0.5us per buffer; a synchronous copy would block it for ~20us of
// driver calls.
//
// Paths:
// - Integrated GPU: shared memory, write the GPU buffer directly.
// - Discrete GPU, fits staging: stage bytes + queue a StagedCopy command.
// - Too big, or render-thread caller: one dedicated upload buffer.
static void UnlockBuffer(GuestBuffer* buffer, bool useCopyQueue)
{
	auto copyBuffer = [&](void* dest)
		{
			memcpy(dest, buffer->mappedMemory, buffer->dataSize);
		};

	if (useCopyQueue && g_capabilities.gpuUploadHeap)
	{
		copyBuffer(buffer->buffer->map());
		buffer->buffer->unmap();
	}
	else if (useCopyQueue && buffer->dataSize <= StagedBufferUploadAllocator::SIZE)
	{
		// Game thread: reserve staging bytes, memcpy, push one command.
		auto allocation = GameFrameStagedUploads().allocate(buffer->dataSize, 0x4, *g_device);
		memcpy(allocation.memory, buffer->mappedMemory, buffer->dataSize);

		RenderCommand cmd;
		cmd.type = RenderCommandType::StagedCopy;
		cmd.stagedCopy.dst = buffer;
		cmd.stagedCopy.srcBuffer = allocation.buffer;
		cmd.stagedCopy.srcOffset = allocation.offset;
		cmd.stagedCopy.size = buffer->dataSize;
		SendRenderCommand(cmd);
	}
	else
	{
		// Oversize uploads and render-thread callers: one dedicated upload
		// buffer for this copy.
		auto uploadBuffer = PlumeCreateBufferWrapper(buffer->dataSize);
		copyBuffer(uploadBuffer->map());
		uploadBuffer->unmap();

		if (useCopyQueue)
		{
			ExecuteCopyCommandList([&](RenderCommandList* commandList)
				{
					commandList->copyBufferRegion(buffer->buffer->at(0), uploadBuffer->at(0), buffer->dataSize);
				});

		}
		else
		{
			auto& commandList = g_commandLists[g_frame];

			commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::WRITE));
			commandList->copyBufferRegion(buffer->buffer->at(0), uploadBuffer->at(0), buffer->dataSize);
			commandList->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::READ));

			g_tempBuffers[g_frame].emplace_back(std::move(uploadBuffer));
		}
	}

	g_bufferUploadCount++;
}


static void UnlockBuffer(GuestBuffer* buffer)
{
	if (!buffer->lockedReadOnly)
	{
		UnlockBuffer(buffer, true);
	}
}

static void ProcUnlockBuffer16(const RenderCommand& cmd)
{
	UnlockBuffer(cmd.unlockBuffer.indexBuffer, false);
}

static void ProcUnlockBuffer32(const RenderCommand& cmd)
{
	UnlockBuffer(cmd.unlockBuffer.indexBuffer, false);
}

// Moves staged upload bytes into their final GPU buffers. Runs on the
// render thread.
//
// Steps for one group of consecutive staged copies:
// 1. Mark every destination buffer writable.
// 2. Copy all the staged bytes.
// 3. Mark every destination buffer readable again.
//
// Grouping keeps the GPU sync stops per frame low: a handful per group
// instead of two per copy.
static void ProcStagedCopyRange(RenderCommand* commands, size_t count)
{
	if (!g_commandListOpen)
	{
		// The frame's command list is closed, so it cannot record the
		// copies. Do a small standalone copy for each upload instead so the
		// data still arrives.
		for (size_t i = 0; i < count; i++)
		{
			auto& args = commands[i].stagedCopy;
			ExecuteCopyCommandList([&](RenderCommandList* commandList)
				{
					commandList->copyBufferRegion(args.dst->buffer->at(0), args.srcBuffer->at(args.srcOffset), args.size);
				});
		}
		return;
	}

	auto& commandList = g_commandLists[g_frame];

	// Barriers are submitted in groups of at most this many.
	constexpr size_t BARRIER_GROUP = 16;
	RenderBufferBarrier barriers[BARRIER_GROUP];

	auto submitBarriers = [&](RenderBarrierStages stage, RenderBufferAccessBits access)
		{
			size_t remaining = count;
			auto* cmds = commands;
			while (remaining > 0)
			{
				size_t group = remaining < BARRIER_GROUP ? remaining : BARRIER_GROUP;
				for (size_t i = 0; i < group; i++)
					barriers[i] = RenderBufferBarrier(cmds[i].stagedCopy.dst->buffer.get(), access);

				commandList->barriers(stage, barriers, uint32_t(group));
				cmds += group;
				remaining -= group;
			}
		};

	submitBarriers(RenderBarrierStage::COPY, RenderBufferAccess::WRITE);

	for (size_t i = 0; i < count; i++)
	{
		auto& args = commands[i].stagedCopy;
		commandList->copyBufferRegion(args.dst->buffer->at(0), args.srcBuffer->at(args.srcOffset), args.size);
	}

	submitBarriers(RenderBarrierStage::GRAPHICS, RenderBufferAccess::READ);
}

void UnlockVertexBuffer(GuestBuffer* buffer)
{
	UnlockBuffer(buffer);
}

static void* LockIndexBuffer(GuestBuffer* buffer, uint32_t flags)
{
	return LockBuffer(buffer, flags);
}

static void UnlockIndexBuffer(GuestBuffer* buffer)
{
	UnlockBuffer(buffer);
}

//D3D12 requires vertex buffer strides to be aligned to 4 bytes unlike Xbox, so we need to fix that when needed
static uint32_t AlignStride(uint32_t stride, uint32_t alignment = 4) {
	return (stride + alignment - 1) & ~(alignment - 1);
}
static inline FLOAT PackedIntToFloat(const int32_t value, const FLOAT PosFactor, const FLOAT NegFactor)
{
	if (value >= 0) {
		return ((FLOAT)value) / PosFactor;
	}
	else {
		return ((FLOAT)value) / NegFactor;
	}
}

//handle vertex format difference such as normalized short format
uint8_t* ApplyVertexPatch(uint8_t* vertexData, const uint8_t indexStream, const uint32_t VertexCount, const uint32_t originalStride, uint32_t alignedStride, bool useHostMemory)
{
	size_t host_handle = (size_t)g_memory.Translate(g_VertexShader_Handle);
	auto vertexElement = XSR_HostGetVertexDeclaration(host_handle, g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction);

	auto pVertexShaderStreamInfo = &(vertexElement->VertexStreams[indexStream]);
	UINT XboxStreamNumber = pVertexShaderStreamInfo->XboxStreamIndex;
	bool bNeedVertexPatching = (pVertexShaderStreamInfo != nullptr && pVertexShaderStreamInfo->NeedPatch);

	if (!bNeedVertexPatching)
		return nullptr;

	uint8_t* pHostVertexData = useHostMemory ? AllocateDrawData(VertexCount * alignedStride) : (uint8_t*)g_userHeap.AllocPhysical(VertexCount * alignedStride, 0x10);

	for (uint32_t uiVertex = 0; uiVertex < VertexCount; uiVertex++)
	{
		uint8_t* pXboxVertexAsByte = &vertexData[uiVertex * originalStride];
		uint8_t* pHostVertexAsByte = &pHostVertexData[uiVertex * alignedStride];

		for (UINT uiElement = 0; uiElement < pVertexShaderStreamInfo->NumberOfVertexElements; uiElement++)
		{
			FLOAT* pXboxVertexAsFloat = (FLOAT*)pXboxVertexAsByte;
			SHORT* pXboxVertexAsShort = (SHORT*)pXboxVertexAsByte;
			const int32_t XboxElementByteSize = pVertexShaderStreamInfo->VertexElements[uiElement].XboxByteSize;
			FLOAT* pHostVertexAsFloat = (FLOAT*)pHostVertexAsByte;
			SHORT* pHostVertexAsShort = (SHORT*)pHostVertexAsByte;
			// Dxbx note : The following code handles only the D3DVSDT enums that need conversion;
			// All other cases are catched by the memcpy in the default-block.

			switch (pVertexShaderStreamInfo->VertexElements[uiElement].XboxType)
			{
			case X_D3DVSDT_NORMSHORT1: { // 0x11:
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = 0;
				break;
			}
			case X_D3DVSDT_NORMSHORT2: { // 0x21:
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = pXboxVertexAsShort[1];
				break;
			}
			case X_D3DVSDT_NORMSHORT3: { // 0x31:
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = pXboxVertexAsShort[1];
				pHostVertexAsShort[2] = pXboxVertexAsShort[2];
				pHostVertexAsShort[3] = 32767; // TODO : verify
				break;
			}
			case X_D3DVSDT_NORMSHORT4: { // 0x41:
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = pXboxVertexAsShort[1];
				pHostVertexAsShort[2] = pXboxVertexAsShort[2];
				pHostVertexAsShort[3] = pXboxVertexAsShort[3];
				break;
			}
			case X_D3DVSDT_NORMPACKED3:
			{

				// Make it FLOAT3
				union {
					int32_t value;
					struct {
						int32_t x : 11;
						int32_t y : 11;
						int32_t z : 10;
					};
				} NormPacked3;

				NormPacked3.value = ((int32_t*)pXboxVertexAsByte)[0];

				pHostVertexAsFloat[0] = PackedIntToFloat(NormPacked3.x, 1023.0f, 1024.f);
				pHostVertexAsFloat[1] = PackedIntToFloat(NormPacked3.y, 1023.0f, 1024.f);
				pHostVertexAsFloat[2] = PackedIntToFloat(NormPacked3.z, 511.0f, 512.f);
				break;
			}
			case X_D3DVSDT_SHORT1: { // 0x15:
				// Make it SHORT2 and set the second short to 0
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = 0;
				break;
			}
			case X_D3DVSDT_SHORT3: { // 0x35:
				// Make it a SHORT4 and set the fourth short to 1
				pHostVertexAsShort[0] = pXboxVertexAsShort[0];
				pHostVertexAsShort[1] = pXboxVertexAsShort[1];
				pHostVertexAsShort[2] = pXboxVertexAsShort[2];
				pHostVertexAsShort[3] = 1; // Turok verified (character disappears when this is 32767)
				break;
			}
			case X_D3DVSDT_PBYTE1: { // 0x14:
				// Make it UBYTE4N
				pHostVertexAsByte[0] = pXboxVertexAsByte[0];
				pHostVertexAsByte[1] = 0;
				pHostVertexAsByte[2] = 0;
				pHostVertexAsByte[3] = 255; // TODO : Verify
				break;
			}
			case X_D3DVSDT_PBYTE2: { // 0x24:
				pHostVertexAsByte[0] = pXboxVertexAsByte[0];
				pHostVertexAsByte[1] = pXboxVertexAsByte[1];
				pHostVertexAsByte[2] = 0;
				pHostVertexAsByte[3] = 255; // TODO : Verify
				break;
			}
			case X_D3DVSDT_PBYTE3: { // 0x34:
				// Make it UBYTE4N
				pHostVertexAsByte[0] = pXboxVertexAsByte[0];
				pHostVertexAsByte[1] = pXboxVertexAsByte[1];
				pHostVertexAsByte[2] = pXboxVertexAsByte[2];
				pHostVertexAsByte[3] = 255; // TODO : Verify
				break;
			}
			case X_D3DVSDT_PBYTE4: { // 0x44:
				pHostVertexAsByte[0] = pXboxVertexAsByte[0];
				pHostVertexAsByte[1] = pXboxVertexAsByte[1];
				pHostVertexAsByte[2] = pXboxVertexAsByte[2];
				pHostVertexAsByte[3] = pXboxVertexAsByte[3];
				break;
			}
			case X_D3DVSDT_FLOAT2H: { // 0x72:
				// Make it FLOAT4 and set the third float to 0.0
				pHostVertexAsFloat[0] = pXboxVertexAsFloat[0];
				pHostVertexAsFloat[1] = pXboxVertexAsFloat[1];
				pHostVertexAsFloat[2] = 0.0f;
				pHostVertexAsFloat[3] = pXboxVertexAsFloat[2];
				break;
			}
			case X_D3DVSDT_NONE: { // 0x02:

				LOG_WARNING("X_D3DVSDT_NONE\n");
				// No host element data (but Xbox size can be above zero, when used for X_D3DVSD_MASK_SKIP*
				break;
			}
			default: {
				// Generic 'conversion' - just make a copy :
				memcpy(pHostVertexAsByte, pXboxVertexAsByte, XboxElementByteSize);
				break;
			}
			} // switch

			// Increment the Xbox pointer :
			pXboxVertexAsByte += XboxElementByteSize;
			// Increment the host pointer :
			pHostVertexAsByte += pVertexShaderStreamInfo->VertexElements[uiElement].HostByteSize;
		} // for NumberOfVertexElements
	} // for uiVertexCount

	return pHostVertexData;
}


// D3D_DrawVertices originally
// this seems to be used only for particles with a dynamic vertex buffer
// in that case we can redirect it to DrawPrimitiveUP to use our memory allocator
bool reUsedVertex = false;
void DrawPrimitive(X_D3DPRIMITIVETYPE PrimitiveType, uint32_t StartVertex, uint32_t VertexCount)
{
	RtScopeTimer _gtScope(g_gtDrawRecordingAccum);

	Guard_Rendering();
	std::shared_lock presentLock(g_presentMutex);

	LocalRenderCommandQueue& queue = g_pendingRenderCommands;

	FlushRenderStateForMainThread(queue);


	for (int i = 0; i < 16; i++)
	{
		auto xboxStreamSource = get_Xbox_SetStreamSource(i);

		if (!xboxStreamSource)
			continue;

		if (xboxStreamSource->VertexBuffer)
		{
			auto alignedStride = AlignStride(xboxStreamSource->Stride);
			size_t host_handle = (size_t)g_memory.Translate(g_VertexShader_Handle);
			auto vertexElement = XSR_HostGetVertexDeclaration(host_handle, g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction);

			auto pVertexShaderStreamInfo = &(vertexElement->VertexStreams[i]);
			bool bNeedVertexPatching = (pVertexShaderStreamInfo != nullptr && pVertexShaderStreamInfo->NeedPatch);

			//if patching is needed the stride may be bigger so update it from the Vertex Declaration that we got earlier
			alignedStride = pVertexShaderStreamInfo->HostVertexStride;

			unsigned long dataAddr = (unsigned long)GetDataFromXboxResource((X_D3DResource*)(xboxStreamSource->VertexBuffer));
			uint8_t* pXboxVertexData = reinterpret_cast<uint8_t*>(g_memory.Translate(dataAddr));

			uint8_t* vertexParticles = nullptr;

			// particles allocate memory every frame and we never pass our VB to the guest memory
			// thus, we can use our dynamic allocator with host memory for better use without worrying about guest physical memory cap.

			// so far testing shows that patching is always needed
			if (bNeedVertexPatching)
			{
				const uint32_t originalStride = xboxStreamSource->Stride;
				vertexParticles = ApplyVertexPatch(pXboxVertexData, i, VertexCount, originalStride, alignedStride, true);
			}
			else
			{
				const size_t originalSize = VertexCount * alignedStride;
				vertexParticles = (uint8_t*)AllocateDrawData(VertexCount * alignedStride);
				memcpy(vertexParticles, pXboxVertexData, originalSize);
			}


			if (vertexParticles)
			{
				auto& cmd = queue.enqueue();
				cmd.type = RenderCommandType::DrawPrimitiveUP;
				cmd.drawPrimitiveUP.primitiveType = PrimitiveType;
				cmd.drawPrimitiveUP.vertexStreamZeroData = vertexParticles;
				cmd.drawPrimitiveUP.vertexStreamZeroSize = VertexCount * alignedStride;
				cmd.drawPrimitiveUP.vertexStreamZeroStride = alignedStride;
				cmd.drawPrimitiveUP.primitiveCount = VertexCount;
				cmd.drawPrimitiveUP.isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;

				queue.submit();
			}

		}
	}
}

#ifdef __ANDROID__
// Twice in a run one frame is written to the log draw by draw (AndroidVideo::CountFrame):
// what is drawn, with which shaders and states, into what and from which texture. A report
// of a wrong picture on a GPU nobody here has says little without it.
static void LogFrameStart()
{
	LOGFN("[frame] the next frame, draw by draw: window {}x{}, picture {}x{}, back buffer {}x{}, Video.ResolutionScale {:.2f}, aspect ratio setting {}.",
		g_swapChain != nullptr ? g_swapChain->getWidth() : 0, g_swapChain != nullptr ? g_swapChain->getHeight() : 0,
		Video::s_viewportWidth, Video::s_viewportHeight, g_backBuffer != nullptr ? g_backBuffer->width : 0, g_backBuffer != nullptr ? g_backBuffer->height : 0,
		Config::ResolutionScale.Value, uint32_t(Config::AspectRatio.Value));
}

static void LogDrawForFrame(const char* kind, uint32_t count)
{
	if (!AndroidVideo::IsFrameLogged())
		return;

	const PipelineState& state = g_pipelineState;
	const char* vertexShader = state.vertexShaderType == VertexShaderMode::FixedFunction ? "fixed function" :
		state.vertexShaderType == VertexShaderMode::Passthrough ? "pass-through" : "of the game";

	std::string target = "no colour target";
	if (g_renderTarget != nullptr)
		target = fmt::format("{} {}x{}", g_renderTarget == g_backBuffer ? "back buffer" : "texture", g_renderTarget->width, g_renderTarget->height);

	std::string depth = "no depth buffer";
	if (g_depthStencil != nullptr && (state.zEnable || state.stencilEnable))
		depth = fmt::format("depth {}x{}", g_depthStencil->width, g_depthStencil->height);

	std::string texture = "no texture";
	if (const GuestTexture* bound = g_textures[0])
	{
		texture = fmt::format("texture 0 is #{} {}x{} format {}{}", bound->descriptorIndex, bound->width, bound->height, uint32_t(bound->format),
			bound->isRenderTarget ? " (a render target)" : "");
	}

	const RenderViewport& viewport = g_renderviewport.vp;

	AndroidVideo::LogDraw(fmt::format(
		"{} of {}, topology {}{}; vertex shader {}, pixel shader {} (variants {:#x}/{:#x}); "
		"depth test {} write {} function {}, blend {} {}/{}, cull {}, colour mask {:#x}, alpha test {} function {} reference {:.3f}; "
		"{}, {}; game viewport {:.0f},{:.0f} {:.0f}x{:.0f} depth {:.2f}..{:.2f}; {} (index pushed {})",
		kind, count, uint32_t(state.primitiveTopology), g_pipelineUsable ? "" : ", LEFT OUT: the driver refused its pipeline",
		vertexShader, state.isFixedFunctionPS ? "fixed function" : "of the game", uint64_t(state.vertexShaderVariant), uint64_t(state.pixelShaderVariant),
		state.zEnable ? "on" : "off", state.zWriteEnable ? "on" : "off", uint32_t(state.zFunc),
		state.alphaBlendEnable ? "on" : "off", uint32_t(state.srcBlend), uint32_t(state.destBlend), uint32_t(state.cullMode), uint32_t(state.colorWriteEnable),
		g_sharedConstants.alphaTestEnable != 0 ? "on" : "off", g_sharedConstants.alphaFunc, g_sharedConstants.alphaRef,
		target, depth, viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth,
		texture, g_sharedConstants.texture2DIndices[0]));
}
#endif

static void ProcDrawPrimitive(const RenderCommand& cmd)
{
	RtScopeTimer _handlerScope(g_rtDrawHandlerAccum);
	const auto& args = cmd.drawPrimitive;

	//convert primitive
	SetPrimitiveType(args.primitiveType);
	isFixedForRenderThread = args.isFixed;
	FlushRenderStateForRenderThread();

#ifdef __ANDROID__
	LogDrawForFrame("vertices", args.primitiveCount);

	if (!g_pipelineUsable)
	{
		g_skippedDrawCount++;
		return;
	}
#endif

	uint32_t primitiveCount = args.primitiveCount;
	auto& commandList = g_commandLists[g_frame];
	{
		RtScopeTimer _drawScope(g_rtDrawCallAccum);
		commandList->drawInstanced(primitiveCount, 1, args.startVertex, 0);
	}
}

//D3DDevice_DrawIndexedVertices (seems to be used to display message)
//On Xbox, VertexCount = IndexCount, both are the same
void DrawIndexedPrimitive(X_D3DPRIMITIVETYPE PrimitiveType, uint32_t VertexCount, int16_t* pIndexData)
{
	RtScopeTimer _gtScope(g_gtDrawRecordingAccum);
	LocalRenderCommandQueue& queue = g_pendingRenderCommands;

	Guard_Rendering();
	std::shared_lock presentLock(g_presentMutex);

	//shadow = 2192 vertex = stride 24 (32 with conversion)
	//object (shadow eyes) = 91 vertex =  stride 24 (32 with conversion)

	// rapidhash over XXH3: index lists average ~0.2 KB, where rapidhash's
	// small-input path wins; XXH3 is faster only on large buffers.
	uint64_t hash = rapidhash(pIndexData, VertexCount * sizeof(int16_t));
	auto& bufferIndexCache = g_indexBufferCache[hash];

	GuestBuffer* bufferIndex;
	if (bufferIndexCache == nullptr)
	{
		bufferIndex = CreateIndexBuffer(VertexCount, X_D3DFMT_INDEX16);

		bufferIndex->hash = hash;

		auto memory = LockIndexBuffer(bufferIndex, 0);

		memcpy(memory, pIndexData, sizeof(int16_t) * VertexCount);

		UnlockIndexBuffer(bufferIndex);

		// Publish only after the upload is queued, so another thread can
		// never draw with a buffer whose GPU copy is not on the queue yet.
		bufferIndexCache = bufferIndex;
	}
	else
	{
		bufferIndex = bufferIndexCache;
	}


	SetIndices(queue, bufferIndex);

	// The index hash already guarantees the cached buffer matches this index
	// data, so the min/max vertex range can be reused instead of rescanning
	// every index on cache hits.
	uint32_t minIdx = 0, maxIdx = 0;
	if (!bufferIndex->hasCachedIndexRange)
	{
		ComputeIndexRange(pIndexData, 0, VertexCount, minIdx, maxIdx);
		bufferIndex->hasCachedIndexRange = true;
		bufferIndex->cachedMinIndex = minIdx;
		bufferIndex->cachedMaxIndex = maxIdx;
	}
	else
	{
		minIdx = bufferIndex->cachedMinIndex;
		maxIdx = bufferIndex->cachedMaxIndex;
	}

	bool patched = false;

	for (int i = 0; i < 16; i++)
	{
		auto xboxStreamSource = get_Xbox_SetStreamSource(i);

		if (!xboxStreamSource || xboxStreamSource->VertexBuffer == nullptr)
			continue;

		auto alignedStride = AlignStride(xboxStreamSource->Stride);

		size_t host_handle = (size_t)g_memory.Translate(g_VertexShader_Handle);
		auto vertexElement = XSR_HostGetVertexDeclaration(host_handle, g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction);

		auto pVertexShaderStreamInfo = &(vertexElement->VertexStreams[i]);
		bool bNeedVertexPatching = (pVertexShaderStreamInfo != nullptr && pVertexShaderStreamInfo->NeedPatch);
		patched = bNeedVertexPatching;
		//if patching is needed the stride may be bigger so update it from the Vertex Declaration that we got earlier
		if (bNeedVertexPatching)
			alignedStride = pVertexShaderStreamInfo->HostVertexStride;

		// if the buffer is dynamic, we can't cache it since it will get rebuild every frame
		// so send it to our dynamic allocator instead
		if (IsDynamicVertexBuffer(xboxStreamSource->VertexBuffer))
		{
			const uint32_t originalStride = xboxStreamSource->Stride;
			const uint32_t usedVertices = maxIdx - minIdx + 1;

			unsigned long dataAddr = (unsigned long)GetDataFromXboxResource((X_D3DResource*)(xboxStreamSource->VertexBuffer));
			uint8_t* pXboxVertexData = reinterpret_cast<uint8_t*>(g_memory.Translate(dataAddr)) + (minIdx * originalStride);

			uint8_t* vertexData = ApplyVertexPatch(pXboxVertexData, i, usedVertices, originalStride, alignedStride, true);
			if (vertexData == nullptr)
			{
				vertexData = AllocateDrawData(usedVertices * alignedStride);
				memcpy(vertexData, pXboxVertexData, usedVertices * originalStride);
			}

			auto& cmd = queue.enqueue();
			cmd.type = RenderCommandType::SetStreamSourceUP;
			cmd.setStreamSourceUP.StreamNumber = i;
			cmd.setStreamSourceUP.vertexData = vertexData;
			cmd.setStreamSourceUP.size = usedVertices * alignedStride;
			cmd.setStreamSourceUP.stride = alignedStride;
			continue;
		}

		auto bufferCache = GetGuestVertexBufferIndexed(i, minIdx, maxIdx, alignedStride);

		if (!bufferCache)
			return;


		auto& cmd = queue.enqueue();
		cmd.type = RenderCommandType::SetStreamSource;
		cmd.setStreamSource.StreamNumber = i;
		cmd.setStreamSource.vertexBuffer = bufferCache;
		cmd.setStreamSource.stride = alignedStride;
	}

	FlushRenderStateForMainThread(queue);

	auto& cmd = queue.enqueue();
	cmd.type = RenderCommandType::DrawIndexedPrimitive;
	cmd.drawIndexedPrimitive.primitiveType = PrimitiveType;
	cmd.drawIndexedPrimitive.baseVertexIndex = -(int32_t)minIdx;
	cmd.drawIndexedPrimitive.startIndex = 0;
	cmd.drawIndexedPrimitive.primCount = VertexCount;
	cmd.drawIndexedPrimitive.isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;

	queue.submit();
}

static void ProcDrawIndexedPrimitive(const RenderCommand& cmd)
{
	RtScopeTimer _handlerScope(g_rtDrawHandlerAccum);
	const auto& args = cmd.drawIndexedPrimitive;
	SetPrimitiveType(args.primitiveType);
	isFixedForRenderThread = args.isFixed;
	FlushRenderStateForRenderThread();

#ifdef __ANDROID__
	LogDrawForFrame("indexed vertices", args.primCount);

	if (!g_pipelineUsable)
	{
		g_skippedDrawCount++;
		return;
	}
#endif

	{
		RtScopeTimer _drawScope(g_rtDrawCallAccum);
		g_commandLists[g_frame]->drawIndexedInstanced(args.primCount, 1, args.startIndex, args.baseVertexIndex, 0);
	}
}
extern bool captureNow;
extern bool drawCompositeRT;

void DrawPrimitiveUP(X_D3DPRIMITIVETYPE PrimitiveType, uint32_t VertexCount, void* pVertexStreamZeroData, uint32_t VertexStreamZeroStride)
{
	RtScopeTimer _gtScope(g_gtDrawRecordingAccum);

	DrawCallCaller callerDebugger = Others;

	if (drawCompositeRT)
		callerDebugger = CompositeRT;

	Guard_Rendering();
	std::shared_lock presentLock(g_presentMutex);

	LocalRenderCommandQueue& queue = g_pendingRenderCommands;
	FlushRenderStateForMainThread(queue);


	UpVertex* vertexData = (UpVertex*)(pVertexStreamZeroData);

	auto& cmd = queue.enqueue();
	cmd.type = RenderCommandType::DrawPrimitiveUP;
	cmd.drawPrimitiveUP.ID = g_drawPrimitiveUP_ID++;
	cmd.drawPrimitiveUP.primitiveType = PrimitiveType;
	cmd.drawPrimitiveUP.primitiveCount = VertexCount;
	cmd.drawPrimitiveUP.vertexStreamZeroData = AllocateDrawData(vertexData, VertexCount * VertexStreamZeroStride);
	cmd.drawPrimitiveUP.vertexStreamZeroSize = VertexCount * VertexStreamZeroStride;
	cmd.drawPrimitiveUP.vertexStreamZeroStride = VertexStreamZeroStride;
	cmd.drawPrimitiveUP.isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;
	cmd.drawPrimitiveUP.threadID = GuestThread::GetCurrentThreadId();
	cmd.drawPrimitiveUP.caller = callerDebugger;

	queue.submit();
	//	printf("Game called Draw Primitive UP - ID: %d, thread id %d - g_frame=%u\n", g_drawPrimitiveUP_ID, GetCurrentThreadId(), g_frame);

}

static void ProcDrawPrimitiveUP(const RenderCommand& cmd)
{
	RtScopeTimer _handlerScope(g_rtDrawHandlerAccum);
	if (g_pendingSurfaceResizeFlag)
	{
		return;
	}

	const auto& args = cmd.drawPrimitiveUP;

	//	printf("RenderThread is now treating DrawPrimitiveUP ID %d - g_frame=%u\n", args.ID, g_frame);
	X_D3DPRIMITIVETYPE XboxPrimitive = (X_D3DPRIMITIVETYPE)args.primitiveType;

	SetPrimitiveType(args.primitiveType);
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[0], uint8_t(args.vertexStreamZeroStride));

	auto allocation = g_uploadAllocators[g_frame].allocate(reinterpret_cast<const uint32_t*>(args.vertexStreamZeroData), args.vertexStreamZeroSize, 0x4);

	auto& vertexBufferView = g_vertexBufferViews[0];
	vertexBufferView.size = args.primitiveCount * args.vertexStreamZeroStride;
	vertexBufferView.buffer = allocation.buffer->at(allocation.offset);
	g_inputSlots[0].stride = args.vertexStreamZeroStride;
	g_dirtyStates.vertexStreamFirst = 0;

	uint32_t indexCount = 0;

	if (args.primitiveType == X_D3DPT_QUADLIST)
	{
		indexCount = g_quadIndexData.prepare(args.primitiveCount);
	}
	// TODO: Do we care about this?
	/**else if (!g_capabilities.triangleFan && args.primitiveType == X_D3DPT_TRIANGLEFAN)
		indexCount = g_triangleFanIndexData.prepare(args.primitiveCount);*/

	isFixedForRenderThread = args.isFixed;
	FlushRenderStateForRenderThread();

#ifdef __ANDROID__
	LogDrawForFrame("vertices handed over directly", args.primitiveCount);

	if (!g_pipelineUsable)
	{
		g_skippedDrawCount++;
		return;
	}
#endif

	uint32_t primitiveCount = args.primitiveCount;

	{
		RtScopeTimer _drawScope(g_rtDrawCallAccum);
		if (indexCount != 0)
			g_commandLists[g_frame]->drawIndexedInstanced(indexCount, 1, 0, 0, 0);
		else
			g_commandLists[g_frame]->drawInstanced(primitiveCount, 1, 0, 0);
	}
}


static void ProcSetTextureStageState(const RenderCommand& cmd)
{
	const auto& args = cmd.SetTextureStageState;

	const auto addressU = ConvertTextureAddressMode(cmd.SetTextureStageState.D3DTSS_ADDRESSU);
	const auto addressV = ConvertTextureAddressMode(cmd.SetTextureStageState.D3DTSS_ADDRESSV);
	const auto addressW = ConvertTextureAddressMode(cmd.SetTextureStageState.D3DTSS_ADDRESSW);
	const uint32_t rawMinFilter = cmd.SetTextureStageState.D3DTSS_MINFILTER;
	const uint32_t rawMipFilter = cmd.SetTextureStageState.D3DTSS_MIPFILTER;
	const auto magFilter = ConvertTextureFilter(cmd.SetTextureStageState.D3DTSS_MAGFILTER);
	const auto minFilter = ConvertTextureFilter(rawMinFilter);
	const auto mipFilter = ConvertTextureFilter(rawMipFilter);
	auto borderColor = ConvertBorderColor(cmd.SetTextureStageState.D3DTSS_BORDERCOLOR);

	// mip filter set to "none" on Xbox means only the base level is sampled
	const float maxLOD = rawMipFilter != X_D3DTEXF_NONE ? FLT_MAX : 0.0f;
	const bool isLinear = rawMinFilter == X_D3DTEXF_LINEAR || rawMinFilter == X_D3DTEXF_ANISOTROPIC;

	// the game has no mipmaps, but anisotropy still help so...
	const bool anisotropyEnabled = Config::AnisotropicFiltering > 1 && isLinear;

	auto& samplerDesc = g_samplerDescs[args.index];

	bool dirtySampler = false;

	SetDirtyValue(dirtySampler, samplerDesc.addressU, addressU);
	SetDirtyValue(dirtySampler, samplerDesc.addressV, addressV);
	SetDirtyValue(dirtySampler, samplerDesc.addressW, addressW);
	SetDirtyValue(dirtySampler, samplerDesc.minFilter, minFilter);
	SetDirtyValue(dirtySampler, samplerDesc.magFilter, magFilter);
	SetDirtyValue(dirtySampler, samplerDesc.mipmapMode, RenderMipmapMode(mipFilter));
	SetDirtyValue(dirtySampler, samplerDesc.maxAnisotropy, anisotropyEnabled ? Config::AnisotropicFiltering : 16u);
	SetDirtyValue(dirtySampler, samplerDesc.anisotropyEnabled, anisotropyEnabled);
	SetDirtyValue(dirtySampler, samplerDesc.maxLOD, maxLOD);
	SetDirtyValue(dirtySampler, samplerDesc.borderColor, borderColor);

	if (dirtySampler)
	{
		auto& [descriptorIndex, sampler] = g_samplerStates[XXH3_64bits(&samplerDesc, sizeof(RenderSamplerDesc))];
		if (descriptorIndex == NULL)
		{
			descriptorIndex = g_samplerStates.size();
			sampler = g_device->createSampler(samplerDesc);
			g_samplerDescriptorSet->setSampler(descriptorIndex - 1, sampler.get());
		}
		SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.samplerIndices[args.index], descriptorIndex - 1);
	}

	auto& sharedSamplerDesc = g_sharedSamplerDescs[args.index];
	bool dirtyCombiner = false;
	SetDirtyValue(dirtyCombiner, g_sharedConstants.color_op[args.index], cmd.SetTextureStageState.D3DTSS_COLOROP);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.color_arg0[args.index], cmd.SetTextureStageState.D3DTSS_COLORARG0);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.color_arg1[args.index], cmd.SetTextureStageState.D3DTSS_COLORARG1);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.color_arg2[args.index], cmd.SetTextureStageState.D3DTSS_COLORARG2);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.alpha_op[args.index], cmd.SetTextureStageState.D3DTSS_ALPHAOP);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.alpha_arg0[args.index], cmd.SetTextureStageState.D3DTSS_ALPHAARG0);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.alpha_arg1[args.index], cmd.SetTextureStageState.D3DTSS_ALPHAARG1);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.alpha_arg2[args.index], cmd.SetTextureStageState.D3DTSS_ALPHAARG2);
	SetDirtyValue(dirtyCombiner, g_sharedConstants.result_arg[args.index], cmd.SetTextureStageState.D3DTSS_RESULTARG);
	MarkSharedPixelConstantsDirty(dirtyCombiner, args.index < FixedFunctionPixelShaderState::TextureStageCount);

	bool dirtyTextureFlags = false;
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVMAT00[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVMAT00);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVMAT01[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVMAT01);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVMAT11[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVMAT11);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVMAT10[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVMAT10);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVLSCALE[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVLSCALE);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.BUMPENVLOFFSET[args.index], cmd.SetTextureStageState.D3DTSS_BUMPENVLOFFSET);
	SetDirtyValue(dirtyTextureFlags, g_sharedConstants.TEXCOORDINDEX[args.index], cmd.SetTextureStageState.D3DTSS_TEXCOORDINDEX);

	if (dirtyTextureFlags)
	{
		g_dirtyStates.sharedConstants = true;
	}
}

static void ProcSetVertexShaderConstants(const RenderCommand& cmd)
{
	auto& args = cmd.setVertexShaderConstants;

	assert((args.index + args.size) * 4 <= HOST_D3DVS_CONSTREG_FULL * 4);
	if (args.isFixedFunc)
		memcpy(&g_vertexShaderConstantsFixed[args.index * 4], args.memory, args.size * 4 * sizeof(float));
	else
		memcpy(&g_vertexShaderConstants[args.index * 4], args.memory, args.size * 4 * sizeof(float));
	g_dirtyStates.vertexShaderConstants = true;
}

static void ProcSetVertexShaderVariant(const RenderCommand& cmd)
{
	SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShaderVariant, cmd.setVertexShaderVariant.key);
}


static void ProcSetPixelShaderConstants(const RenderCommand& cmd)
{
	auto& args = cmd.setPixelShaderConstants;

	assert((args.index + args.size) * 4 <= PSH_XBOX_CONSTANT_MAX * 4);

	int sizeToCopy = args.size * 4 * sizeof(float);
	sizeToCopy = sizeToCopy;
	memcpy(&g_pixelShaderConstants[args.index * 4], args.memory, args.size * 4 * sizeof(float));
	g_dirtyStates.pixelShaderConstants = true;
}


//D3DResource_Release
X86_FUNC_IMPL(__imp__sub_3D5F10);
X86_OVERRIDE_FUNC(sub_3D5F10)
{
	auto resource = X86_LOAD_U32(ctx.esp.u32 + 4);
	if (resource)
	{
		std::lock_guard lock(g_guestRefCountMutex);
		__imp__sub_3D5F10(ctx, base);
	}
	else
	{
		ctx.esp.u32 += 0x8;  //Adjust esp for return value (0xC2 + 0x4)
	}
}

//Free Vertex Buffer
X86_FUNC_IMPL(__imp__sub_2E6140);
X86_OVERRIDE_FUNC(sub_2E6140)
{
	auto resource = X86_LOAD_U32(ctx.esp.u32 + 4);
	if (resource)
	{
		FreeVB(resource);

		std::vector<GuestResource*> unready;
		{
			std::lock_guard lock(g_tempUnreadyResourceMutex);
			unready.swap(g_tempUnreadyResource);
		}

		for (auto& temp : unready) //if previous VB wasn't ready to be deleted before, try again now
		{
			AddDestructResource(temp);
		}
	}

	__imp__sub_2E6140(ctx, base);
}

//CreateStandAloneSurface
X86_FUNC_IMPL(__imp__sub_3DA0A0);
X86_OVERRIDE_FUNC(sub_3DA0A0)
{

	uint32_t Width = X86_LOAD_U32(ctx.esp.u32 + 4);
	uint32_t Height = X86_LOAD_U32(ctx.esp.u32 + 8);
	X_D3DFORMAT Format = (X_D3DFORMAT)X86_LOAD_U32(ctx.esp.u32 + 16);
	__imp__sub_3DA0A0(ctx, base);

	ctx.esp.u32 -= 4;

	X_D3DSurface* surface = (X_D3DSurface*)(g_memory.Translate(ctx.eax.u32));

	if (surface)
	{
		auto newSurface = CreateSurface(Width, Height, Format);

		newSurface->guestResource.Common = surface->Common;
		newSurface->guestResource.Data = surface->Data;
		newSurface->guestResource.Format = surface->Format;
		newSurface->guestResource.Lock = surface->Lock;
		newSurface->guestResource.Size = surface->Size;
		newSurface->padding = surface->Parent;
		g_userHeap.Free(g_memory.Translate(ctx.eax.u32)); // free the original surface memory as it's not needed anymore
		ctx.eax.u32 = g_memory.MapVirtual(newSurface);
	}

	ctx.esp.u32 += 4;
}

// Stubbed DX8 functions

int D3D_GetAdapterModeCount(int a1)
{
	LOG_UTILITY("!!! STUB !!!");
	return 4 * 1;
}

DWORD* D3D_CommonSetDebugRegisters()
{
	return 0;
}

BOOL D3Device_IsBusy()
{
	return FALSE;
}

int D3D_SetTileNoWait(int a1, int* a2)
{
	return 0;
}

BOOL D3DDevice_IsFencePending(int a1)
{
	return FALSE;
}

void D3DResource_BlockUntilNotBusy(DWORD* a1)
{
	;
}

void D3D_BlockOnTime(int32_t a1, int32_t a2)
{
	;
}

void VideoConfigValueChangedCallback(IConfigDef* config)
{
	// Config options that require internal resolution resize
	g_needsResize |=
		config == &Config::AspectRatio ||
		config == &Config::ResolutionScale;

	if (g_needsResize)
	{
		g_pendingSurfaceResizeFlag = true;
	}

	if (g_needsResize)
	{
		Video::ComputeViewportDimensions();
	}
}

void Video::Shutdown()
{
	if (g_pipelineCacheShutdownDone.exchange(true))
		return;

	SavePipelineCache();

#ifdef __ANDROID__
	AndroidVideo::SavePipelineCache();
#endif

	if (!s_dumpPipelineCacheHeader)
		return;

	std::lock_guard lock(g_pipelineCacheMutex);
	if (g_pipelineCacheEntries.empty())
		return;

	auto path = GetCachePath() / "pipeline_cache_dump.h";
	std::filesystem::create_directories(path.parent_path());
	FILE* f = fopen(path.string().c_str(), "w");
	if (f == nullptr)
		return;

	std::vector<PipelineCacheEntry> entries;
	for (auto& [hash, entry] : g_pipelineCacheEntries)
		entries.push_back(entry);

	fmt::print(f, "#pragma once\n\n");
	fmt::print(f, "static const uint8_t g_staticPipelineCacheData[] = {{\n");

	auto bytes = reinterpret_cast<const uint8_t*>(entries.data());
	size_t totalSize = entries.size() * sizeof(PipelineCacheEntry);

	for (size_t i = 0; i < totalSize; i++)
	{
		fmt::print(f, "0x{:02X}{}", bytes[i], i < totalSize - 1 ? "," : "");
		if ((i + 1) % 16 == 0)
			fmt::print(f, "\n");
	}

	fmt::print(f, "\n}};\n");
	fmt::print(f, "static constexpr size_t g_staticPipelineCacheCount = {};\n", entries.size());

	fclose(f);
	LOGFN("PipelineCache: Generated {} with {} entries ({:.1f} KB)",
		path.filename().string(), entries.size(), totalSize / 1024.0);
}

class PipelineCacheSDLEventListener : public SDLEventListener
{
public:
	bool OnSDLEvent(SDL_Event* event) override
	{
		if (event->type == SDL_EVENT_QUIT)
		{
			Video::Shutdown();
		}
		return false;
	}
};
PipelineCacheSDLEventListener g_pipelineCacheSDLEventListener;

static void ProcessRenderCommand(const RenderCommand& cmd)
{
	if (cmd.type != RenderCommandType::AddPipeline && !g_commandListOpen)
	{
		static bool loggedDeferral = false;
		if (!loggedDeferral)
		{
			loggedDeferral = true;
			LOGFN_WARNING("RenderThread: Deferring stale command. Command list is closed (type={})", (int)cmd.type);
		}
		g_deferredRenderCommands.push_back(cmd);
		return;
	}

	switch (cmd.type)
	{
	case RenderCommandType::DestructResource: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcDestructResource(cmd); } break;
	case RenderCommandType::UnlockTextureRect: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcUnlockTextureRect(cmd); } break;
	case RenderCommandType::UnlockBuffer16: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcUnlockBuffer16(cmd); } break;
	case RenderCommandType::UnlockBuffer32: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcUnlockBuffer32(cmd); } break;
	case RenderCommandType::SetRenderState: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetRenderState(cmd); } break;
	case RenderCommandType::DrawImGui: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcDrawImGui(cmd); } break;
	case RenderCommandType::SetRenderTarget: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetRenderTarget(cmd); } break;
	case RenderCommandType::SetDepthStencilSurface: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetDepthStencilSurface(cmd); } break;
	case RenderCommandType::Clear: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcClear(cmd); } break;
	case RenderCommandType::SetViewport: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetViewport(cmd); } break;
	case RenderCommandType::SetTexture: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetTexture(cmd); } break;
	case RenderCommandType::SetTextureStageState: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetTextureStageState(cmd); } break;
	case RenderCommandType::SetVertexShaderConstants: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetVertexShaderConstants(cmd); } break;
	case RenderCommandType::SetVertexShaderVariant: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetVertexShaderVariant(cmd); } break;
	case RenderCommandType::SetPixelShaderConstants: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetPixelShaderConstants(cmd); } break;
	case RenderCommandType::DrawPrimitive: { g_drawCallCount++; ProcDrawPrimitive(cmd); } break;
	case RenderCommandType::DrawIndexedPrimitive: { g_drawCallCount++; ProcDrawIndexedPrimitive(cmd); } break;
	case RenderCommandType::DrawPrimitiveUP: { g_drawCallCount++; ProcDrawPrimitiveUP(cmd); } break;
	case RenderCommandType::SetVertexDeclaration: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetVertexDeclaration(cmd); } break;
	case RenderCommandType::SetVertexShader: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetVertexShader(cmd); } break;
	case RenderCommandType::SetStreamSource: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetStreamSource(cmd); } break;
	case RenderCommandType::SetStreamSourceUP: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetStreamSourceUP(cmd); } break;
	case RenderCommandType::SetIndices: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetIndices(cmd); } break;
	case RenderCommandType::SetPixelShader: { g_stateChangeCount++; RtScopeTimer _s(g_rtOtherCmdsAccum); ProcSetPixelShader(cmd); } break;
	case RenderCommandType::AddPipeline: { ProcAddPipeline(cmd); } break;
	default:
		assert(false && "Unrecognized render command type.");
		break;
	}
}

static std::thread g_renderThread([]
	{
#ifdef _WIN32
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		GuestThread::SetThreadName(GetCurrentThreadId(), "Render Thread");
#endif

		RenderCommand commands[30];

		while (true)
		{
			size_t count;
			{
				// Idle accounting: total queue wait, plus the slice inside the open
				// frame bracket (g_renderDirectorProfiler.Begin to End).
				const bool profEnabled = g_detailedProfilerEnabled.load(std::memory_order_relaxed);
				const auto waitStart = profEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
				count = g_renderQueue.wait_dequeue_bulk(commands, std::size(commands));

				if (profEnabled)
				{
					const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
					g_rtQueueWaitAccum.Add(waitMs);

					if (g_renderDirectorProfiler.active)
						g_rtQueueWaitInFrameAccum.Add(waitMs);
				}
			}
			for (size_t i = 0; i < count; i++)
			{
				auto& cmd = commands[i];
				g_totalCommandCount++;

				switch (cmd.type)
				{
				case RenderCommandType::StagedCopy:
				{
					RtScopeTimer _s(g_rtOtherCmdsAccum);

					// Handle every consecutive staged copy in one group.
					size_t stagedCount = 1;
					while (i + stagedCount < count && commands[i + stagedCount].type == RenderCommandType::StagedCopy)
						++stagedCount;

					g_stateChangeCount += uint32_t(stagedCount);
					g_totalCommandCount += uint32_t(stagedCount - 1);
					ProcStagedCopyRange(&commands[i], stagedCount);
					i += stagedCount - 1;
				} break;
				case RenderCommandType::EndFrame:
#ifdef __ANDROID__
					if (AndroidVideo::CountFrame(g_drawCallCount, std::exchange(g_skippedDrawCount, 0u), std::exchange(g_framebufferChangeCount, 0u)))
						LogFrameStart();
#endif
					g_drawCallCountSnapshot = std::exchange(g_drawCallCount, 0);
					g_stateChangeCountSnapshot = std::exchange(g_stateChangeCount, 0);
					g_totalCommandCountSnapshot = std::exchange(g_totalCommandCount, 0);
					g_psoCacheHitsSnapshot = std::exchange(g_psoCacheHits, 0);
					g_psoCacheMissesSnapshot = std::exchange(g_psoCacheMisses, 0);
					g_rtOtherCmdsAccum.SnapshotAndReset();
					g_rtBeginCmdListWorkAccum.SnapshotAndReset();
					g_rtPsoWorkAccum.SnapshotAndReset();
					g_rtFlushWorkAccum.SnapshotAndReset();
					g_rtDrawCallAccum.SnapshotAndReset();
					g_rtDrawHandlerAccum.SnapshotAndReset();
					g_rtQueueWaitAccum.SnapshotAndReset();
					g_rtQueueWaitInFrameAccum.SnapshotAndReset();
					g_renderDirectorProfiler.End();
					ProcEndFrame(cmd);
					g_renderDirectorProfiler.Begin();
					break;
				default:
					ProcessRenderCommand(cmd);
					break;
				}
			}
		}
	});


GUEST_FUNCTION_HOOK_STDCALL(sub_3DDAB0, Direct3D_CreateDevice_r);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DD950, SetResolution);

//Resource
GUEST_FUNCTION_HOOK_STDCALL(sub_3DA010, CreateSurfaceOfTexture);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D0040, D3DDevice_SetRenderTarget);

//Rendering
GUEST_FUNCTION_HOOK_STDCALL(sub_3D0680, D3DDevice_SetViewport);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D6BF0, D3DDevice_Clear);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DC1F0, DrawIndexedPrimitive);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DC150, DrawPrimitive);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DBFE0, DrawPrimitiveUP);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DD560, D3DDevice_Swap); //Present

// Misc

// adx and fmv decoding requires vblank signal
GUEST_FUNCTION_HOOK(sub_3CFE70, CRI_BlockUntilVerticalBlank);

//			==== STUB  ====
GUEST_FUNCTION_HOOK(sub_3D3240, D3D_CommonSetDebugRegisters);
GUEST_FUNCTION_HOOK(sub_3D0F10, D3Device_IsBusy);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D4720, D3DDevice_IsFencePending);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D5F60, D3DResource_BlockUntilNotBusy);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D4950, D3D_BlockOnTime);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DD8B0, D3D_GetAdapterModeCount);
GUEST_FUNCTION_HOOK_STDCALL(sub_3D0F40, D3D_SetTileNoWait);