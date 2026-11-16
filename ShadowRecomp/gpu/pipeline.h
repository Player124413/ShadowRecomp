#pragma once

#include "video.h"
#include  <gpu/ubershaders/shader_variant.h>

#pragma pack(push, 1)
struct PipelineState
{
	GuestShader* vertexShader = nullptr;
	GuestShader* pixelShader = nullptr;
	//bool isFixedFunctionVS = false;
	bool isFixedFunctionPS = false;
	ShaderVariantKey vertexShaderVariant = GenericShaderVariant;
	ShaderVariantKey pixelShaderVariant = GenericShaderVariant;
	VertexShaderMode vertexShaderType;
	GuestVertexDeclaration* vertexDeclaration = nullptr;
	bool zEnable = false;
	bool zWriteEnable = true;
	bool stencilEnable = false;
	RenderBlend srcBlend = RenderBlend::ONE;
	RenderBlend destBlend = RenderBlend::ZERO;
	RenderCullMode cullMode = RenderCullMode::NONE;
	RenderComparisonFunction zFunc = RenderComparisonFunction::LESS;
	RenderComparisonFunction stencilFunc = RenderComparisonFunction::ALWAYS;
	RenderStencilOp stencilFail = RenderStencilOp::KEEP;
	RenderStencilOp stencilZFail = RenderStencilOp::KEEP;
	RenderStencilOp stencilPass = RenderStencilOp::KEEP;
	uint32_t stencilMask = 0xFFFFFFFF;
	uint32_t stencilWriteMask = 0xFFFFFFFF;
	uint32_t stencilRef = 0;
	RenderComparisonFunction stencilFuncCCW = RenderComparisonFunction::ALWAYS;
	bool alphaBlendEnable = false;
	RenderBlendOperation blendOp = RenderBlendOperation::ADD;
	float slopeScaledDepthBias = 0.0f;
	int32_t depthBias = 0;
	RenderBlend srcBlendAlpha = RenderBlend::ONE;
	RenderBlend destBlendAlpha = RenderBlend::ZERO;
	RenderBlendOperation blendOpAlpha = RenderBlendOperation::ADD;
	uint32_t colorWriteEnable = uint32_t(RenderColorWriteEnable::ALL);
	RenderPrimitiveTopology primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
	uint8_t vertexStrides[16]{};
	RenderFormat renderTargetFormat{};
	RenderFormat depthStencilFormat{};
	RenderSampleCounts sampleCount = RenderSampleCount::COUNT_1;
	bool enableAlphaToCoverage = false;
	uint32_t specConstants = 0;
};
#pragma pack(pop)

static constexpr uint32_t PIPELINE_CACHE_MAGIC = 0x31435053;
static constexpr uint32_t PIPELINE_CACHE_VERSION = 1;
static constexpr size_t MAX_VERTEX_ELEMENTS = 32;

#pragma pack(push, 1)
struct PipelineCacheEntry
{
	uint64_t pipelineHash;
	uint64_t vertexShaderHash;
	uint64_t pixelShaderHash;
	uint64_t vertexDeclHash;
	uint32_t vertexElementCount;
	VertexShaderMode vertexShaderMode;
	D3DVERTEXELEMENT9 vertexElements[MAX_VERTEX_ELEMENTS];
	bool isFixedFunctionPS;
	bool zEnable;
	bool zWriteEnable;
	bool stencilEnable;
	RenderBlend srcBlend;
	RenderBlend destBlend;
	RenderCullMode cullMode;
	RenderComparisonFunction zFunc;
	RenderComparisonFunction stencilFunc;
	RenderStencilOp stencilFail;
	RenderStencilOp stencilZFail;
	RenderStencilOp stencilPass;
	uint32_t stencilMask;
	uint32_t stencilWriteMask;
	uint32_t stencilRef;
	RenderComparisonFunction stencilFuncCCW;
	bool alphaBlendEnable;
	RenderBlendOperation blendOp;
	float slopeScaledDepthBias;
	int32_t depthBias;
	RenderBlend srcBlendAlpha;
	RenderBlend destBlendAlpha;
	RenderBlendOperation blendOpAlpha;
	uint32_t colorWriteEnable;
	RenderPrimitiveTopology primitiveTopology;
	uint8_t vertexStrides[16];
	RenderFormat renderTargetFormat;
	RenderFormat depthStencilFormat;
	RenderSampleCounts sampleCount;
	bool enableAlphaToCoverage;
	uint32_t specConstants;
};
#pragma pack(pop)

static_assert(sizeof(PipelineCacheEntry) == 414,
	"PipelineCacheEntry layout changed - bump PIPELINE_CACHE_VERSION and regenerate pipeline_cache.h");

bool IsPipelineCacheEntryInRange(const PipelineCacheEntry& entry);
bool BuildPipelineCacheEntry(const PipelineState& pipelineState, PipelineCacheEntry& outEntry);
std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(const PipelineState& pipelineState);
void SanitizePipelineState(PipelineState& pipelineState);

XXH64_hash_t ComputePipelineEntryHash(const PipelineCacheEntry& entry);
XXH64_hash_t ComputePipelineStateHash(const PipelineState& pipelineState);
void CompilePendingPipelinesForShader(uint64_t runtimeHash, GuestShader* shader, bool isVertexShader);
void ApplyCacheEntryVariants(const PipelineCacheEntry& entry, PipelineState& pipelineState);
void EnqueueDeferredPipeline(const PipelineCacheEntry& entry);

// Whether the pipelines listed in the pipeline caches are created by background threads
// while the game is already running (true), or all of them before it starts (false).
//
// Creating them up front is quick where the graphics driver keeps compiled pipelines on
// disk, which is every desktop driver: there the cost is paid once. No Android driver does
// (gpu/vulkan_pipeline_cache.h does it for them), and their compilers are slow, so on
// Android the game would sit on a black screen for the full compilation of about a
// thousand pipelines, on every launch. There they are created in the background instead: a
// pipeline that a draw needs before its turn is created by the render thread on the spot,
// as any pipeline missing from the caches is.
bool PrecompilesPipelinesInBackground();

// Has a background thread create the pipeline of `pipelineState`, which must be sanitized
// and have `hash` as its ComputePipelineStateHash(), and hand it to the render thread. The
// render thread keeps it unless it had to create one for that state itself in the
// meantime. Returns false if the pipeline had been queued before.
bool CompilePipelineInBackground(XXH64_hash_t hash, const PipelineState& pipelineState);

// Brackets a run of CompilePipelineInBackground() calls. The background threads start on
// none of the pipelines until the last such object is gone, and treat what was queued as
// one batch: when they are done with it, the driver's pipeline cache is written to disk.
class BackgroundPipelineBatch
{
public:
    BackgroundPipelineBatch();
    ~BackgroundPipelineBatch();

    BackgroundPipelineBatch(const BackgroundPipelineBatch&) = delete;
    BackgroundPipelineBatch& operator=(const BackgroundPipelineBatch&) = delete;

private:
    const bool m_active;
};