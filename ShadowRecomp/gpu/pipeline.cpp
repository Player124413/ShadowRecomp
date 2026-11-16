#include "pipeline.h"
#include <shader/shader_cache.h>
#include "ubershaders/fixed_function_vertex_shader_variants.h"
#include "ubershaders/fixed_function_pixel_shader_variants.h"
#include "vertexshader.h"
#include <os/logger.h>
#include "vertexdeclaration.h"
#include "render_commands.h"
#include "background_work_queue.h"

#ifdef __ANDROID__
#include "android_video.h"
#include <user/config.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#endif


static Mutex g_pendingCacheMutex;
static ankerl::unordered_dense::map<uint64_t, std::vector<PipelineCacheEntry>> g_pendingVS;
static ankerl::unordered_dense::map<uint64_t, std::vector<PipelineCacheEntry>> g_pendingPS;

extern std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
extern std::unique_ptr<RenderShader> FixedFuncVS;
extern std::unique_ptr<RenderShader> PassthroughVS; 
extern std::unique_ptr<RenderShader> FixedFuncPS ;

template <typename T>
static bool IsEnumInRange(T value, T last)
{
	return uint32_t(value) <= uint32_t(last);
}


bool IsPipelineCacheEntryInRange(const PipelineCacheEntry& entry)
{
	return entry.vertexElementCount <= MAX_VERTEX_ELEMENTS &&
		IsEnumInRange(entry.vertexShaderMode, VertexShaderMode::ShaderProgram) &&
		IsEnumInRange(entry.srcBlend, RenderBlend::INV_SRC1_ALPHA) &&
		IsEnumInRange(entry.destBlend, RenderBlend::INV_SRC1_ALPHA) &&
		IsEnumInRange(entry.srcBlendAlpha, RenderBlend::INV_SRC1_ALPHA) &&
		IsEnumInRange(entry.destBlendAlpha, RenderBlend::INV_SRC1_ALPHA) &&
		IsEnumInRange(entry.cullMode, RenderCullMode::BACK) &&
		IsEnumInRange(entry.zFunc, RenderComparisonFunction::ALWAYS) &&
		IsEnumInRange(entry.stencilFunc, RenderComparisonFunction::ALWAYS) &&
		IsEnumInRange(entry.stencilFuncCCW, RenderComparisonFunction::ALWAYS) &&
		IsEnumInRange(entry.stencilFail, RenderStencilOp::DECREMENT_AND_WRAP) &&
		IsEnumInRange(entry.stencilZFail, RenderStencilOp::DECREMENT_AND_WRAP) &&
		IsEnumInRange(entry.stencilPass, RenderStencilOp::DECREMENT_AND_WRAP) &&
		IsEnumInRange(entry.blendOp, RenderBlendOperation::MAX) &&
		IsEnumInRange(entry.blendOpAlpha, RenderBlendOperation::MAX) &&
		IsEnumInRange(entry.primitiveTopology, RenderPrimitiveTopology::TRIANGLE_FAN) &&
		IsEnumInRange(entry.renderTargetFormat, RenderFormat::MAX) &&
		IsEnumInRange(entry.depthStencilFormat, RenderFormat::MAX);
}


bool BuildPipelineCacheEntry(const PipelineState& pipelineState, PipelineCacheEntry& outEntry)
{
	outEntry = {};
	outEntry.vertexShaderMode = pipelineState.vertexShaderType;
	outEntry.isFixedFunctionPS = pipelineState.isFixedFunctionPS;

	if (pipelineState.vertexShaderType == VertexShaderMode::FixedFunction ||
		pipelineState.vertexShaderType == VertexShaderMode::Passthrough)
	{
		outEntry.vertexShaderHash = pipelineState.vertexShaderType == VertexShaderMode::FixedFunction
			? pipelineState.vertexShaderVariant
			: 0;
	}
	else if (pipelineState.vertexShader != nullptr)
	{
		if (pipelineState.vertexShader->shaderCacheEntry != nullptr)
			outEntry.vertexShaderHash = pipelineState.vertexShader->shaderCacheEntry->hash;
		else if (pipelineState.vertexShader->runtimeHash != 0)
			outEntry.vertexShaderHash = pipelineState.vertexShader->runtimeHash;
		else
			return false;
	}
	else
		return false;

	if (pipelineState.isFixedFunctionPS)
	{
		outEntry.pixelShaderHash = pipelineState.pixelShaderVariant;
	}
	else if (pipelineState.pixelShader != nullptr)
	{
		if (pipelineState.pixelShader->shaderCacheEntry != nullptr)
			outEntry.pixelShaderHash = pipelineState.pixelShader->shaderCacheEntry->hash;
		else if (pipelineState.pixelShader->runtimeHash != 0)
			outEntry.pixelShaderHash = pipelineState.pixelShader->runtimeHash;
		else
			return false;
	}
	else if (pipelineState.pixelShader == nullptr)
	{
		outEntry.pixelShaderHash = 0;
	}
	else
		return false;

	if (pipelineState.vertexDeclaration != nullptr)
	{
		outEntry.vertexDeclHash = pipelineState.vertexDeclaration->hash;
		outEntry.vertexElementCount = pipelineState.vertexDeclaration->vertexElementCount;
		if (outEntry.vertexElementCount > MAX_VERTEX_ELEMENTS)
			return false;

		memcpy(outEntry.vertexElements, pipelineState.vertexDeclaration->vertexElements.get(),
			outEntry.vertexElementCount * sizeof(D3DVERTEXELEMENT9));
	}
	else
		return false;

	outEntry.zEnable = pipelineState.zEnable;
	outEntry.zWriteEnable = pipelineState.zWriteEnable;
	outEntry.stencilEnable = pipelineState.stencilEnable;
	outEntry.srcBlend = pipelineState.srcBlend;
	outEntry.destBlend = pipelineState.destBlend;
	outEntry.cullMode = pipelineState.cullMode;
	outEntry.zFunc = pipelineState.zFunc;
	outEntry.stencilFunc = pipelineState.stencilFunc;
	outEntry.stencilFail = pipelineState.stencilFail;
	outEntry.stencilZFail = pipelineState.stencilZFail;
	outEntry.stencilPass = pipelineState.stencilPass;
	outEntry.stencilMask = pipelineState.stencilMask;
	outEntry.stencilWriteMask = pipelineState.stencilWriteMask;
	outEntry.stencilRef = pipelineState.stencilRef;
	outEntry.stencilFuncCCW = pipelineState.stencilFuncCCW;
	outEntry.alphaBlendEnable = pipelineState.alphaBlendEnable;
	outEntry.blendOp = pipelineState.blendOp;
	outEntry.slopeScaledDepthBias = pipelineState.slopeScaledDepthBias;
	outEntry.depthBias = pipelineState.depthBias;
	outEntry.srcBlendAlpha = pipelineState.srcBlendAlpha;
	outEntry.destBlendAlpha = pipelineState.destBlendAlpha;
	outEntry.blendOpAlpha = pipelineState.blendOpAlpha;
	outEntry.colorWriteEnable = pipelineState.colorWriteEnable;
	outEntry.primitiveTopology = pipelineState.primitiveTopology;
	memcpy(outEntry.vertexStrides, pipelineState.vertexStrides, sizeof(outEntry.vertexStrides));
	outEntry.renderTargetFormat = pipelineState.renderTargetFormat;
	outEntry.depthStencilFormat = pipelineState.depthStencilFormat;
	outEntry.sampleCount = pipelineState.sampleCount;
	outEntry.enableAlphaToCoverage = pipelineState.enableAlphaToCoverage;
	outEntry.specConstants = pipelineState.specConstants;

	return true;
}


void SanitizePipelineState(PipelineState& pipelineState)
{
	if (pipelineState.vertexShaderType == VertexShaderMode::FixedFunction ||
		pipelineState.vertexShaderType == VertexShaderMode::Passthrough)
		pipelineState.vertexShader = nullptr;
	if (pipelineState.vertexShaderType != VertexShaderMode::FixedFunction)
		pipelineState.vertexShaderVariant = GenericShaderVariant;

	if (pipelineState.isFixedFunctionPS)
		pipelineState.pixelShader = nullptr;
	else
		pipelineState.pixelShaderVariant = GenericShaderVariant;

	if (!pipelineState.zEnable && !pipelineState.stencilEnable)
	{
		pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;
	}

	if (!pipelineState.zEnable)
	{
		pipelineState.zWriteEnable = false;
		pipelineState.zFunc = RenderComparisonFunction::LESS;
		pipelineState.slopeScaledDepthBias = 0.0f;
		pipelineState.depthBias = 0;
		pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;
	}

	if (!pipelineState.stencilEnable)
	{
		pipelineState.stencilFail = RenderStencilOp::KEEP;
		pipelineState.stencilZFail = RenderStencilOp::KEEP;
		pipelineState.stencilPass = RenderStencilOp::KEEP;
		pipelineState.stencilMask = 0xFFFFFFFF;
		pipelineState.stencilWriteMask = 0xFFFFFFFF;
		pipelineState.stencilRef = 0;
	}

	if (pipelineState.slopeScaledDepthBias == 0.0f)
		pipelineState.slopeScaledDepthBias = 0.0f; // Remove sign.

	if (!pipelineState.colorWriteEnable)
	{
		pipelineState.alphaBlendEnable = false;
		pipelineState.renderTargetFormat = RenderFormat::UNKNOWN;
	}

	if (!pipelineState.alphaBlendEnable)
	{
		pipelineState.srcBlend = RenderBlend::ONE;
		pipelineState.destBlend = RenderBlend::ZERO;
		pipelineState.blendOp = RenderBlendOperation::ADD;
		pipelineState.srcBlendAlpha = RenderBlend::ONE;
		pipelineState.destBlendAlpha = RenderBlend::ZERO;
		pipelineState.blendOpAlpha = RenderBlendOperation::ADD;
	}

	for (size_t i = 0; i < 16; i++)
	{
		if (!pipelineState.vertexDeclaration->vertexStreams[i])
			pipelineState.vertexStrides[i] = 0;
	}

	uint32_t specConstantsMask = 0;
	/**if (pipelineState.vertexShader->shaderCacheEntry != nullptr)
		specConstantsMask |= pipelineState.vertexShader->shaderCacheEntry->specConstantsMask;*/

	if (pipelineState.pixelShader != nullptr && pipelineState.pixelShader->shaderCacheEntry != nullptr)
		specConstantsMask |= pipelineState.pixelShader->shaderCacheEntry->specConstantsMask;

	pipelineState.specConstants &= specConstantsMask;
}


void EnqueueDeferredPipeline(const PipelineCacheEntry& entry)
{
	if (entry.vertexShaderMode != VertexShaderMode::FixedFunction &&
		entry.vertexShaderMode != VertexShaderMode::Passthrough &&
		entry.vertexShaderHash != 0 && FindShaderCacheEntry(entry.vertexShaderHash) == nullptr)
	{
		std::lock_guard lock(g_pendingCacheMutex);
		g_pendingVS[entry.vertexShaderHash].push_back(entry);
		return;
	}

	if (entry.pixelShaderHash != 0 && !entry.isFixedFunctionPS && FindShaderCacheEntry(entry.pixelShaderHash) == nullptr)
	{
		std::lock_guard lock(g_pendingCacheMutex);
		g_pendingPS[entry.pixelShaderHash].push_back(entry);
		return;
	}
}

XXH64_hash_t ComputePipelineEntryHash(const PipelineCacheEntry& entry)
{
	constexpr size_t skip = sizeof(uint64_t); // skip pipelineHash
	return XXH3_64bits(reinterpret_cast<const uint8_t*>(&entry) + skip, sizeof(PipelineCacheEntry) - skip);
}


XXH64_hash_t ComputePipelineStateHash(const PipelineState& pipelineState)
{
	PipelineCacheEntry entry;
	if (BuildPipelineCacheEntry(pipelineState, entry))
		return ComputePipelineEntryHash(entry);

	return XXH3_64bits(&pipelineState, sizeof(pipelineState));
}


// Maps cache entry hashes to variant keys, falling back to the generic
// shader for hashes missing from the generated catalogs.
void ApplyCacheEntryVariants(const PipelineCacheEntry& entry, PipelineState& pipelineState)
{
	pipelineState.vertexShaderVariant = entry.vertexShaderMode == VertexShaderMode::FixedFunction &&
		FixedFunctionVertexShaderVariants::Contains(entry.vertexShaderHash)
		? entry.vertexShaderHash
		: GenericShaderVariant;
	pipelineState.pixelShaderVariant = entry.isFixedFunctionPS && FixedFunctionPixelShaderVariants::Contains(entry.pixelShaderHash)
		? entry.pixelShaderHash
		: GenericShaderVariant;
}


static void EnqueueAddPipeline(XXH64_hash_t hash, std::unique_ptr<RenderPipeline> pipeline)
{
	RenderCommand cmd;
	cmd.type = RenderCommandType::AddPipeline;
	cmd.addPipeline.hash = hash;
	cmd.addPipeline.pipeline = pipeline.release();
	SendRenderCommand(cmd);
}


// --- Background pipeline compilation ----------------------------------------------------
// See PrecompilesPipelinesInBackground() in pipeline.h for the why.

struct QueuedPipeline
{
	XXH64_hash_t hash;
	PipelineState pipelineState;
};

// States that were handed to the background threads, so that none is compiled twice.
static Mutex g_queuedPipelineMutex;
static ankerl::unordered_dense::set<XXH64_hash_t> g_queuedPipelineHashes;

bool PrecompilesPipelinesInBackground()
{
#ifdef __ANDROID__
	return Config::BackgroundPipelineCompilation;
#else
	return false;
#endif
}

static uint32_t GetPipelineCompilerThreadCount()
{
	// Half of the cores, four at most. The other half stays with the game, which is
	// starting up at the same time, and compiling does not get faster forever anyway.
	return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
}

static void OnPipelineCompilerThreadStarted(uint32_t threadIndex)
{
#ifdef __ANDROID__
	// Shown in the thread dump the watchdog writes to log.txt when frames stop.
	char name[16];
	snprintf(name, sizeof(name), "PSO Compiler %u", threadIndex);
	pthread_setname_np(pthread_self(), name);

	// Far below the game's threads (the nice value of Android's "background" priority):
	// where a core is free the compiler runs at full speed, where it has to share one with
	// the game it gets a tenth of it.
	setpriority(PRIO_PROCESS, id_t(gettid()), 10);
#else
	(void)threadIndex;
#endif
}

static void CompileQueuedPipeline(QueuedPipeline& queued)
{
	EnqueueAddPipeline(queued.hash, CreateGraphicsPipeline(queued.pipelineState));
}

static void OnQueuedPipelinesCompiled(uint32_t count, double milliseconds)
{
	LOGFN("PipelineCache: Background threads compiled {} pipelines in {:.2f} ms", count, milliseconds);

#ifdef __ANDROID__
	AndroidVideo::OnCachedPipelinesCreated();
#endif
}

static BackgroundWorkQueue<QueuedPipeline>& GetPipelineCompiler()
{
	// Never destroyed: its threads are there for as long as the process is.
	static auto* compiler = new BackgroundWorkQueue<QueuedPipeline>(GetPipelineCompilerThreadCount(),
		{ CompileQueuedPipeline, OnQueuedPipelinesCompiled, OnPipelineCompilerThreadStarted });

	return *compiler;
}

bool CompilePipelineInBackground(XXH64_hash_t hash, const PipelineState& pipelineState)
{
	{
		std::lock_guard lock(g_queuedPipelineMutex);
		if (!g_queuedPipelineHashes.insert(hash).second)
			return false;
	}

	GetPipelineCompiler().Add({ hash, pipelineState });
	return true;
}

BackgroundPipelineBatch::BackgroundPipelineBatch() : m_active(PrecompilesPipelinesInBackground())
{
	if (m_active)
		GetPipelineCompiler().BeginHold();
}

BackgroundPipelineBatch::~BackgroundPipelineBatch()
{
	if (m_active)
		GetPipelineCompiler().EndHold();
}


void CompilePendingPipelinesForShader(uint64_t runtimeHash, GuestShader* shader, bool isVertexShader)
{
	std::vector<PipelineCacheEntry> pending;
	{
		std::lock_guard lock(g_pendingCacheMutex);
		auto& map = isVertexShader ? g_pendingVS : g_pendingPS;
		auto it = map.find(runtimeHash);
		if (it != map.end())
		{
			pending = std::move(it->second);
			map.erase(it);
		}
	}

	if (pending.empty())
	{
		return;
	}

	// This runs on the game's thread, in the middle of creating a shader: where pipelines
	// are compiled in the background, these go there too rather than stall the game.
	const bool inBackground = PrecompilesPipelinesInBackground();
	BackgroundPipelineBatch batch;

	LOGFN("PipelineCache: {} {} deferred pipelines for {} runtime hash={:016X}",
		inBackground ? "Queueing" : "Compiling", pending.size(), isVertexShader ? "VS" : "PS", runtimeHash);

	uint32_t redeferred = 0;
	ankerl::unordered_dense::set<XXH64_hash_t> queued;
	for (auto& entry : pending)
	{
		GuestShader* vertexShader = nullptr;
		GuestShader* pixelShader = nullptr;

		if (entry.vertexShaderMode == VertexShaderMode::FixedFunction ||
			entry.vertexShaderMode == VertexShaderMode::Passthrough)
		{
			vertexShader = nullptr;
		}
		else if (isVertexShader && entry.vertexShaderHash == runtimeHash)
		{
			vertexShader = shader;
		}
		else if (entry.vertexShaderHash != 0)
		{
			auto findVS = FindShaderCacheEntry(entry.vertexShaderHash);
			if (findVS != nullptr && findVS->guestShader != nullptr)
			{
				vertexShader = findVS->guestShader;
			}
			else
			{
				std::lock_guard lock(g_pendingCacheMutex);
				g_pendingVS[entry.vertexShaderHash].push_back(entry);
				redeferred++;
				continue;
			}
		}
		else
		{
			continue;
		}

		if (entry.isFixedFunctionPS)
		{
			pixelShader = nullptr;
		}
		else if (!isVertexShader && entry.pixelShaderHash == runtimeHash)
		{
			pixelShader = shader;
		}
		else if (entry.pixelShaderHash != 0)
		{
			auto findPS = FindShaderCacheEntry(entry.pixelShaderHash);
			if (findPS != nullptr && findPS->guestShader != nullptr)
			{
				pixelShader = findPS->guestShader;
			}
			else
			{
				std::lock_guard lock(g_pendingCacheMutex);
				g_pendingPS[entry.pixelShaderHash].push_back(entry);
				redeferred++;
				continue;
			}
		}

		GuestVertexDeclaration* vertexDecl = FindVertexDeclarationByHash(entry.vertexDeclHash);
		if (vertexDecl == nullptr)
		{
			if (entry.vertexElementCount > 0 && entry.vertexElementCount <= MAX_VERTEX_ELEMENTS)
			{
				vertexDecl = CreateVertexDeclarationWithoutAddRef(entry.vertexElements, entry.vertexShaderMode);
			}
			if (vertexDecl == nullptr)
			{
				continue;
			}
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
		if (queued.insert(hash).second)
		{
			if (inBackground)
				CompilePipelineInBackground(hash, pipelineState);
			else
				EnqueueAddPipeline(hash, CreateGraphicsPipeline(pipelineState));
		}
	}

	if (redeferred != 0)
	{
		LOGFN("PipelineCache: Re-deferred {} pending pipelines (paired shader not yet created)", redeferred);
	}
}


std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(const PipelineState& pipelineState)
{
	RenderGraphicsPipelineDesc desc;
	desc.pipelineLayout = g_pipelineLayout.get();

	if (pipelineState.vertexShaderType == VertexShaderMode::Passthrough)
	{
		desc.vertexShader = PassthroughVS.get();
	}
	else if (pipelineState.vertexShaderType == VertexShaderMode::FixedFunction)
	{
		desc.vertexShader = FixedFunctionVertexShaderVariants::Resolve(pipelineState.vertexShaderVariant);
		if (desc.vertexShader == nullptr)
			desc.vertexShader = FixedFuncVS.get();
	}
	else
	{
		desc.vertexShader = GetOrLinkShader(pipelineState.vertexShader, pipelineState.specConstants);
	}


	if (pipelineState.isFixedFunctionPS)
	{
		desc.pixelShader = FixedFunctionPixelShaderVariants::Resolve(pipelineState.pixelShaderVariant);
		if (desc.pixelShader == nullptr)
			desc.pixelShader = FixedFuncPS.get();
	}
	else
	{
		desc.pixelShader = GetOrLinkShader(pipelineState.pixelShader, pipelineState.specConstants);
	}
	desc.depthFunction = pipelineState.zFunc;
	desc.depthEnabled = pipelineState.zEnable; //Xbox likes to enable ZEnable without a depth stencil
	desc.depthWriteEnabled = pipelineState.zWriteEnable;
	desc.depthBias = pipelineState.depthBias;
	desc.stencilEnabled = pipelineState.stencilEnable;
	desc.stencilReadMask = pipelineState.stencilMask;
	desc.stencilWriteMask = pipelineState.stencilWriteMask;
	desc.stencilReference = pipelineState.stencilRef;
	desc.stencilFrontFace.compareFunction = pipelineState.stencilFunc;
	desc.stencilFrontFace.failOp = pipelineState.stencilFail;
	desc.stencilFrontFace.depthFailOp = pipelineState.stencilZFail;
	desc.stencilFrontFace.passOp = pipelineState.stencilPass;
	desc.stencilBackFace = desc.stencilFrontFace;
	desc.slopeScaledDepthBias = pipelineState.slopeScaledDepthBias;
	desc.dynamicDepthBiasEnabled = Video::GetRenderDevice()->getCapabilities().dynamicDepthBias;
	desc.depthClipEnabled = true;
	desc.primitiveTopology = pipelineState.primitiveTopology;

	desc.cullMode = pipelineState.cullMode;
	desc.renderTargetFormat[0] = pipelineState.renderTargetFormat;
	desc.renderTargetBlend[0].blendEnabled = pipelineState.alphaBlendEnable;
	desc.renderTargetBlend[0].srcBlend = pipelineState.srcBlend;
	desc.renderTargetBlend[0].dstBlend = pipelineState.destBlend;
	desc.renderTargetBlend[0].blendOp = pipelineState.blendOp;
	desc.renderTargetBlend[0].srcBlendAlpha = pipelineState.srcBlendAlpha;
	desc.renderTargetBlend[0].dstBlendAlpha = pipelineState.destBlendAlpha;
	desc.renderTargetBlend[0].blendOpAlpha = pipelineState.blendOpAlpha;
	desc.renderTargetBlend[0].renderTargetWriteMask = pipelineState.colorWriteEnable;
	desc.renderTargetCount = pipelineState.renderTargetFormat != RenderFormat::UNKNOWN ? 1 : 0;
	desc.depthTargetFormat = pipelineState.depthStencilFormat;
	desc.multisampling.sampleCount = pipelineState.sampleCount;
	desc.alphaToCoverageEnabled = pipelineState.enableAlphaToCoverage;
	desc.inputElements = pipelineState.vertexDeclaration->inputElements.get();
	desc.inputElementsCount = pipelineState.vertexDeclaration->inputElementCount;


	RenderSpecConstant specConstant{};
	specConstant.value = pipelineState.specConstants;

	if (pipelineState.specConstants != 0)
	{
		desc.specConstants = &specConstant;
		desc.specConstantsCount = 1;
	}

	RenderInputSlot inputSlots[16]{};
	uint32_t inputSlotIndices[16]{};
	uint32_t inputSlotCount = 0;

	for (size_t i = 0; i < pipelineState.vertexDeclaration->inputElementCount; i++)
	{
		auto& inputElement = pipelineState.vertexDeclaration->inputElements[i];
		auto& inputSlotIndex = inputSlotIndices[inputElement.slotIndex];

		if (inputSlotIndex == NULL)
			inputSlotIndex = ++inputSlotCount;

		auto& inputSlot = inputSlots[inputSlotIndex - 1];
		inputSlot.index = inputElement.slotIndex;
		inputSlot.stride = pipelineState.vertexStrides[inputElement.slotIndex];
		inputSlot.classification = RenderInputSlotClassification::PER_VERTEX_DATA;
	}

	desc.inputSlots = inputSlots;
	desc.inputSlotsCount = inputSlotCount;

	auto pipeline = Video::GetRenderDevice()->createGraphicsPipeline(desc);

	return pipeline;
}