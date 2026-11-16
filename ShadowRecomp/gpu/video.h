#pragma once

#include <plume_render_interface.h>
#include <functional>

#define D3DCLEAR_TARGET  0x1
#define D3DCLEAR_ZBUFFER 0x10
typedef float RwReal;

#define LOAD_ZSTD_TEXTURE(name) LoadCustomTexture(decompressZstd(name, name##_uncompressed_size).get(), name##_uncompressed_size)

constexpr DWORD X_D3DTS_STAGECOUNT = 4; // Dxbx addition

static constexpr uint32_t PITCH_ALIGNMENT = 0x100;
static constexpr uint32_t PLACEMENT_ALIGNMENT = 0x200;
extern uint32_t g_VertexShader_Handle;
extern std::atomic<bool> g_pendingSurfaceResizeFlag;

extern std::atomic<uint32_t> g_fmvFrameWidth;
extern std::atomic<uint32_t> g_fmvFrameHeight;
extern std::atomic<bool> g_frameReadsGuestMemory;

// Xbox vertex shader counts
#define X_VSH_MAX_ATTRIBUTES          16
#define X_VSH_MAX_STREAMS             16
#define X_VSH_MAX_INSTRUCTION_COUNT   136  // The maximum Xbox shader instruction count
#define X_VSH_INSTRUCTION_SIZE        4
#define X_VSH_INSTRUCTION_SIZE_BYTES  (X_VSH_INSTRUCTION_SIZE * sizeof(DWORD))
#define TexSlotMax 16u

// pixel container "format" masks
#define X_D3DFORMAT_CUBEMAP               0x00000004      // Set if the texture if a cube map
#define X_D3DFORMAT_FORMAT_MASK           0x0000FF00    
#define X_D3DFORMAT_FORMAT_SHIFT          8
#define X_D3DFORMAT_MIPMAP_MASK           0x000F0000      // # mipmap levels (always 1 for surfaces)
#define X_D3DFORMAT_MIPMAP_SHIFT          16
#define X_D3DFORMAT_DIMENSION_MASK        0x000000F0      // # of dimensions, must be 2 or 3
#define X_D3DFORMAT_DIMENSION_SHIFT       4

#define PHYSICAL_MAP_BASE                   0x80000000

#define NV_PCRTC_INTR_0                                  0x00000100
#define NV_PCRTC_INTR_0_VBLANK                               (1 << 0)
#define NV_PMC_INTR_0_PCRTC                                (1 << 24)
#define NV_PCRTC_INTR_EN_0                               0x00000140


float GetRenderTargetHeight();
double GetTargetFPS();
int32_t GetMaxFPS();
int32_t GetUnlockedFPS();
bool IsFPSUnlocked();

inline bool g_needsResize;

using namespace plume;

struct Video
{
	static inline bool s_dumpPipelineCacheHeader = false;
	static inline uint32_t s_viewportWidth;
	static inline uint32_t s_viewportHeight;

	static RenderDevice* GetRenderDevice();
	static bool is_vulkan();
	static bool CreateHostDevice(const char* sdlVideoDriver, bool graphicsApiRetry);
	static void PrecompileShadersAndPipelines();
	static void PrecompileShadersAndPipelinesForInstall(const std::function<void(float)>& progress);
	static void Present(X86Context* ctx);
	static void WaitForGPU();
	static void ComputeViewportDimensions();
	static void Shutdown();

	// Writes what this run has learned about pipelines to storage, if anything: the pipeline
	// states the game used and, on Android, what the driver compiled for them. Meant for
	// the moments a process may not outlive, such as an Android app going to the background.
	static void SaveCaches();

	static void BeginGameThreadProfiler();
	static void EndGameThreadProfiler();
};



typedef enum _X_D3DPRIMITIVETYPE
{
	X_D3DPT_NONE = 0, // Dxbx addition

	X_D3DPT_POINTLIST = 1,
	X_D3DPT_LINELIST = 2,
	X_D3DPT_LINELOOP = 3,    // Xbox only
	X_D3DPT_LINESTRIP = 4,
	X_D3DPT_TRIANGLELIST = 5,
	X_D3DPT_TRIANGLESTRIP = 6,
	X_D3DPT_TRIANGLEFAN = 7,
	X_D3DPT_QUADLIST = 8,    // Xbox only
	X_D3DPT_QUADSTRIP = 9,    // Xbox only
	X_D3DPT_POLYGON = 10,   // Xbox only

	X_D3DPT_MAX = 11,
	X_D3DPT_INVALID = 0x7fffffff, /* force 32-bit size enum */
}
X_D3DPRIMITIVETYPE;

typedef enum _X_D3DFORMAT
{
	/*
		Xbox1 D3DFORMAT notes
		---------------------

		The Xbox1 D3DFORMAT type consists of 4 different format categories :
		1. Swizzled (improves data locality, incompatible with native Direct3D)
		2. Compressed (DXT compression, giving 4:1 reduction on 4x4 pixel blocks)
		3. Linear (compatible with native Direct3D)
		4. Depth (Fixed or Floating point, stored Linear or Swizzled)

		Requirements\Format      Swizzled  Compressed  Linear  Depth   Notes

		Power-of-two required ?  YES       YES         NO      NO
		Mipmap supported ?       YES       YES         NO      YES     Linear has MipmapLevels = 1
		CubeMaps supported ?     YES       YES         NO      NO      Cubemaps have 6 faces
		Supports volumes ?       YES       YES         NO      NO      Volumes have 3 dimensions, Textures have 2
		Can be a rendertarget ?  YES       YES         YES     LINEAR  Depth buffers can only be rendered to if stored Linear

		Implications :
		- CubeMaps must be square
		- Volumes cannot be cube mapped and vice versa

		Maximum dimensions :
		2D : 4096 x 4096 (12 mipmap levels)
		3D : 512 x 512 x 512 (9 mipmap levels)

	*/

	// Xbox D3DFORMAT types :
	// See https://wiki.beyondunreal.com/Legacy:Texture_Format

	// Swizzled Formats

	X_D3DFMT_L8 = 0x00,
	X_D3DFMT_AL8 = 0x01,
	X_D3DFMT_A1R5G5B5 = 0x02,
	X_D3DFMT_X1R5G5B5 = 0x03,
	X_D3DFMT_A4R4G4B4 = 0x04,
	X_D3DFMT_R5G6B5 = 0x05,
	X_D3DFMT_A8R8G8B8 = 0x06,
	X_D3DFMT_X8R8G8B8 = 0x07,
	X_D3DFMT_X8L8V8U8 = 0x07, // Alias

	X_D3DFMT_P8 = 0x0b, // 8-bit Palletized

	X_D3DFMT_A8 = 0x19,
	X_D3DFMT_A8L8 = 0x1a,
	X_D3DFMT_R6G5B5 = 0x27,
	X_D3DFMT_L6V5U5 = 0x27, // Alias

	X_D3DFMT_G8B8 = 0x28,
	X_D3DFMT_V8U8 = 0x28, // Alias

	X_D3DFMT_R8B8 = 0x29,
	X_D3DFMT_D24S8 = 0x2a,
	X_D3DFMT_F24S8 = 0x2b,
	X_D3DFMT_D16 = 0x2c,
	X_D3DFMT_D16_LOCKABLE = 0x2c, // Alias

	X_D3DFMT_F16 = 0x2d,
	X_D3DFMT_L16 = 0x32,
	X_D3DFMT_V16U16 = 0x33,
	X_D3DFMT_R5G5B5A1 = 0x38,
	X_D3DFMT_R4G4B4A4 = 0x39,
	X_D3DFMT_A8B8G8R8 = 0x3A,
	X_D3DFMT_Q8W8V8U8 = 0x3A, // Alias

	X_D3DFMT_B8G8R8A8 = 0x3B,
	X_D3DFMT_R8G8B8A8 = 0x3C,

	// YUV Formats

	X_D3DFMT_YUY2 = 0x24,
	X_D3DFMT_UYVY = 0x25,

	// Compressed Formats

	X_D3DFMT_DXT1 = 0x0C, // opaque/one-bit alpha
	X_D3DFMT_DXT2 = 0x0E, // Alias for X_D3DFMT_DXT3
	X_D3DFMT_DXT3 = 0x0E, // linear alpha
	X_D3DFMT_DXT4 = 0x0F, // Alias for X_D3DFMT_DXT5
	X_D3DFMT_DXT5 = 0x0F, // interpolated alpha

	// Linear Formats

	X_D3DFMT_LIN_A1R5G5B5 = 0x10,
	X_D3DFMT_LIN_R5G6B5 = 0x11,
	X_D3DFMT_LIN_A8R8G8B8 = 0x12,
	X_D3DFMT_LIN_L8 = 0x13,
	X_D3DFMT_LIN_R8B8 = 0x16,
	X_D3DFMT_LIN_G8B8 = 0x17,
	X_D3DFMT_LIN_V8U8 = 0x17, // Alias

	X_D3DFMT_LIN_AL8 = 0x1b,
	X_D3DFMT_LIN_X1R5G5B5 = 0x1c,
	X_D3DFMT_LIN_A4R4G4B4 = 0x1d,
	X_D3DFMT_LIN_X8R8G8B8 = 0x1e,
	X_D3DFMT_LIN_X8L8V8U8 = 0x1e, // Alias

	X_D3DFMT_LIN_A8 = 0x1f,
	X_D3DFMT_LIN_A8L8 = 0x20,
	X_D3DFMT_LIN_D24S8 = 0x2E,
	X_D3DFMT_LIN_F24S8 = 0x2f,
	X_D3DFMT_LIN_D16 = 0x30,
	X_D3DFMT_LIN_F16 = 0x31,
	X_D3DFMT_LIN_L16 = 0x35,
	X_D3DFMT_LIN_V16U16 = 0x36,
	X_D3DFMT_LIN_R6G5B5 = 0x37,
	X_D3DFMT_LIN_L6V5U5 = 0x37, // Alias

	X_D3DFMT_LIN_R5G5B5A1 = 0x3D,
	X_D3DFMT_LIN_R4G4B4A4 = 0x3e,
	X_D3DFMT_LIN_A8B8G8R8 = 0x3f,
	X_D3DFMT_LIN_B8G8R8A8 = 0x40,
	X_D3DFMT_LIN_R8G8B8A8 = 0x41,

	X_D3DFMT_VERTEXDATA = 0x64,

	X_D3DFMT_INDEX16 = 101/*=D3DFMT_INDEX16*/, // Dxbx addition : Not an Xbox format, used internally

	X_D3DFMT_UNKNOWN = 0xFFFFFFFF - 3,  // Unique declaration to make overloads possible
}
X_D3DFORMAT, * PX_D3DFORMAT;

typedef enum _X_D3DCULL
{
	X_D3DCULL_NONE = 0,
	X_D3DCULL_CW = 0x900,
	X_D3DCULL_CCW = 0x901,
	X_D3DCULL_FORCE_DWORD = 0x7fffffff
}
X_D3DCULL;

typedef enum _D3DCMPFUNC {
	D3DCMP_NEVER = 0x200,
	D3DCMP_LESS = 0x201,
	D3DCMP_EQUAL = 0x202,
	D3DCMP_LESSEQUAL = 0x203,
	D3DCMP_GREATER = 0x204,
	D3DCMP_NOTEQUAL = 0x205,
	D3DCMP_GREATEREQUAL = 0x206,
	D3DCMP_ALWAYS = 0x207,
	D3DCMP_FORCE_DWORD = 0x7fffffff, /* force 32-bit size enum */
} D3DCMPFUNC;

typedef enum _D3DSTENCILOP {
	D3DSTENCILOP_KEEP = 0x1e00,
	D3DSTENCILOP_ZERO = 0x0000,
	D3DSTENCILOP_REPLACE = 0x1e01,
	D3DSTENCILOP_INCRSAT = 0x1e02,
	D3DSTENCILOP_DECRSAT = 0x1e03,
	D3DSTENCILOP_INVERT = 0x150a,
	D3DSTENCILOP_INCR = 0x8507,
	D3DSTENCILOP_DECR = 0x8508,
	D3DSTENCILOP_FORCE_DWORD = 0x7fffffff, /* force 32-bit size enum */
} D3DSTENCILOP;

// Values for material source
typedef enum _D3DMATERIALCOLORSOURCE
{
	D3DMCS_MATERIAL = 0,            // Color from material is used
	D3DMCS_COLOR1 = 1,            // Diffuse vertex color is used
	D3DMCS_COLOR2 = 2,            // Specular vertex color is used
	D3DMCS_FORCE_DWORD = 0x7fffffff,   // force 32-bit size enum
} D3DMATERIALCOLORSOURCE;

// X_D3DTEXTUREFILTERTYPE Values
constexpr DWORD X_D3DTEXF_NONE = 0;
constexpr DWORD X_D3DTEXF_POINT = 1;
constexpr DWORD X_D3DTEXF_LINEAR = 2;
constexpr DWORD X_D3DTEXF_ANISOTROPIC = 3;
constexpr DWORD X_D3DTEXF_QUINCUNX = 4; // Xbox extension
constexpr DWORD X_D3DTEXF_GAUSSIANCUBIC = 5;

// X_D3DCLEAR values :
constexpr DWORD X_D3DCLEAR_ZBUFFER = 0x00000001;
constexpr DWORD X_D3DCLEAR_STENCIL = 0x00000002;
constexpr DWORD X_D3DCLEAR_TARGET_R = 0x00000010;  // Clear target surface R component (Xbox ext.)
constexpr DWORD X_D3DCLEAR_TARGET_G = 0x00000020;  // Clear target surface G component (Xbox ext.)
constexpr DWORD X_D3DCLEAR_TARGET_B = 0x00000040;  // Clear target surface B component (Xbox ext.)
constexpr DWORD X_D3DCLEAR_TARGET_A = 0x00000080;  // Clear target surface A component (Xbox ext.)
constexpr DWORD X_D3DCLEAR_TARGET = X_D3DCLEAR_TARGET_R | X_D3DCLEAR_TARGET_G | X_D3DCLEAR_TARGET_B | X_D3DCLEAR_TARGET_A;


enum class ResourceType
{
	Texture,
	VolumeTexture,
	VertexBuffer,
	IndexBuffer,
	RenderTarget,
	DepthStencil,
	VertexDeclaration,
	VertexShader,
	PixelShader,
	PassthroughVertexShader,
};

struct XD3DResource
{
	uint32_t Common;
	uint32_t Data;
	uint32_t Lock;
	uint32_t Format;
	uint32_t Size;
};

struct X_D3DResource
{
	DWORD Common;
	DWORD Data;
	DWORD Lock;
};


struct GuestResource
{
	XD3DResource guestResource;
	uint32_t padding;
	uint32_t refCount = 1;
	ResourceType type;

	GuestResource(ResourceType type) : type(type)
	{
	}

	void AddRef()
	{
		std::atomic_ref atomicRef(refCount);

		uint32_t originalValue, incrementedValue;
		do
		{
			originalValue = refCount;
			incrementedValue = ((originalValue)+1);
		} while (!atomicRef.compare_exchange_weak(originalValue, incrementedValue));
	}

	void Release()
	{
		std::atomic_ref atomicRef(refCount);

		uint32_t originalValue, decrementedValue;
		do
		{
			originalValue = refCount;
			decrementedValue = ((originalValue)-1);
		} while (!atomicRef.compare_exchange_weak(originalValue, decrementedValue));

		// Normally we are supposed to release here, so only use this
		// function when you know you won't be the one destructing it.
	}
};



struct GuestBaseTexture : GuestResource
{
	std::unique_ptr<RenderTexture> textureHolder;
	RenderTexture* texture = nullptr;
	uint64_t textureGeneration = 0; //0 implies host owned
	std::unique_ptr<RenderTextureView> textureView;
	uint32_t width = 0; //host width res
	uint32_t height = 0; //host height res
	uint32_t g_width = 0; //og width for resolution scaling
	uint32_t g_height = 0; //og height for resolution scaling
	RenderFormat format = RenderFormat::UNKNOWN;
	uint32_t descriptorIndex = 0;
	RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
	bool wasResized = false;

	GuestBaseTexture(ResourceType type) : GuestResource(type)
	{
	}
};

// Texture/VolumeTexture 
struct GuestTexture : GuestBaseTexture
{
	uint32_t depth = 0;
	RenderTextureViewDimension viewDimension = RenderTextureViewDimension::UNKNOWN;
	void* mappedMemory = nullptr;
	uint32_t sizeOfTextureRect = 0;
	std::unique_ptr<RenderFramebuffer> framebuffer;
	std::unique_ptr<GuestTexture> patchedTexture;
	struct GuestSurface* sourceSurface = nullptr;
	std::vector<struct GuestSurface*> surfaceRTs;
	bool isRenderTarget = false;
	bool shouldFreeMemory = false;
	bool isFmv = false;
	uint32_t liveWidth = 0; // 'live' being the portion drawn in the mismatch texture and draw size case
	uint32_t liveHeight = 0;
	bool liveAreaReset = false;
};

struct GuestLockedRect
{
	uint32_t pitch;
	uint32_t bits;
};


// VertexBuffer/IndexBuffer
struct GuestBuffer : GuestResource
{
	std::unique_ptr<RenderBuffer> buffer;
	XXH64_hash_t hash = 0;
	void* mappedMemory = nullptr;
	uint32_t dataSize = 0;
	RenderFormat format = RenderFormat::UNKNOWN;
	uint32_t guestFormat = 0;
	bool lockedReadOnly = false;
	bool isVertexBuffer = false;
	bool isDirty = false;
	bool wasUploaded = false;

	// Cached min/max of the index data for index buffers. Avoids rescanning
	// the indices on every draw that hits the index buffer cache.
	bool hasCachedIndexRange = false;
	uint32_t cachedMinIndex = 0;
	uint32_t cachedMaxIndex = 0;
};

struct GuestFramebufferCacheEntry
{
	std::unique_ptr<RenderFramebuffer> framebuffer;
	uint64_t generation = 0;
};

// RenderTarget/DepthStencil
struct GuestSurface : GuestBaseTexture
{
	uint32_t guestFormat = 0;
	ankerl::unordered_dense::map<const RenderTexture*, GuestFramebufferCacheEntry> framebuffers;
	RenderSampleCounts sampleCount = RenderSampleCount::COUNT_1;
	ankerl::unordered_dense::set<GuestTexture*> destinationTextures;
	GuestTexture* parentTexture = nullptr;
};

struct _D3DRECT
{
	int32_t x1;
	int32_t y1;
	int32_t x2;
	int32_t y2;
};

// This is used to initialize the last vertex element in a vertex declaration
// array
//
#define D3DDECL_END() {0xFF,0,D3DDECLTYPE_UNUSED,0,0,0}


struct GuestVertexDeclaration : GuestResource
{
	XXH64_hash_t hash = 0;
	std::unique_ptr<RenderInputElement[]> inputElements;
	std::unique_ptr<D3DVERTEXELEMENT9[]> vertexElements;
	uint32_t inputElementCount = 0;
	uint32_t vertexElementCount = 0;
	uint32_t swappedTexcoords = 0;
	bool hasR11G11B10Normal = false;
	bool vertexStreams[16]{};
	uint32_t indexVertexStream = 0;
#ifdef _DEBUG
	uint8_t uniqueID = 0;
#endif
};

// VertexShader/PixelShader
struct GuestShader : GuestResource
{
	Mutex mutex;
	std::unique_ptr<RenderShader> shader;
	struct ShaderCacheEntry* shaderCacheEntry = nullptr;
	uint64_t runtimeHash = 0;
	ankerl::unordered_dense::map<uint32_t, std::unique_ptr<RenderShader>> linkedShaders;
#ifdef SHADOW_RECOMP_D3D12
	std::vector<ComPtr<IDxcBlob>> shaderBlobs;
	ComPtr<IDxcBlobEncoding> libraryBlob;
#endif
};

typedef struct _X_D3DPIXELSHADERDEF
{
	DWORD    PSAlphaInputs[8];          // X_D3DRS_PSALPHAINPUTS0..X_D3DRS_PSALPHAINPUTS7 : Alpha inputs for each stage
	DWORD    PSFinalCombinerInputsABCD; // X_D3DRS_PSFINALCOMBINERINPUTSABCD : Final combiner inputs
	DWORD    PSFinalCombinerInputsEFG;  // X_D3DRS_PSFINALCOMBINERINPUTSEFG : Final combiner inputs (continued)
	DWORD    PSConstant0[8];            // X_D3DRS_PSCONSTANT0_0..X_D3DRS_PSCONSTANT0_7 : C0 for each stage
	DWORD    PSConstant1[8];            // X_D3DRS_PSCONSTANT1_0..X_D3DRS_PSCONSTANT1_7 : C1 for each stage
	DWORD    PSAlphaOutputs[8];         // X_D3DRS_PSALPHAOUTPUTS0..X_D3DRS_PSALPHAOUTPUTS7 : Alpha output for each stage
	DWORD    PSRGBInputs[8];            // X_D3DRS_PSRGBINPUTS0..X_D3DRS_PSRGBINPUTS7 : RGB inputs for each stage
	DWORD    PSCompareMode;             // X_D3DRS_PSCOMPAREMODE : Compare modes for clipplane texture mode
	DWORD    PSFinalCombinerConstant0;  // X_D3DRS_PSFINALCOMBINERCONSTANT0 : C0 in final combiner
	DWORD    PSFinalCombinerConstant1;  // X_D3DRS_PSFINALCOMBINERCONSTANT1 : C1 in final combiner
	DWORD    PSRGBOutputs[8];           // X_D3DRS_PSRGBOUTPUTS0..X_D3DRS_PSRGBOUTPUTS7 : Stage 0 RGB outputs
	DWORD    PSCombinerCount;           // X_D3DRS_PSCOMBINERCOUNT : Active combiner count (Stages 0-7)
	DWORD    PSTextureModes;            // X_D3DRS_PS_RESERVED (copied from out-of-range X_D3DRS_PSTEXTUREMODES) : Texture addressing modes
	DWORD    PSDotMapping;              // X_D3DRS_PSDOTMAPPING : Input mapping for dot product modes
	DWORD    PSInputTexture;            // X_D3DRS_PSINPUTTEXTURE : Texture source for some texture modes

	// These last three DWORDs are used to define how Direct3D8 pixel shader constants map to the constant
	// registers in each combiner stage. They are used by the Direct3D run-time software but not by the hardware.
	DWORD    PSC0Mapping;               // Mapping of c0 regs to D3D constants
	DWORD    PSC1Mapping;               // Mapping of c1 regs to D3D constants
	DWORD    PSFinalCombinerConstants;  // Final combiner constant mapping
}
X_D3DPIXELSHADERDEF;

typedef void X_D3DDevice;

typedef struct GuestPixelShader
{
	DWORD RefCount;
	DWORD D3DOwned;
	uint32_t pPSDef; //pointer to _X_D3DPIXELSHADERDEF
} X_PixelShader;


//_XD3DVIEWPORT8
struct GuestViewport
{
	DWORD X;
	DWORD Y;
	DWORD Width;
	DWORD Height;
	float MinZ;
	float MaxZ;
};

struct _XMETAL_PushBuffer
{
	uint32_t m_pPut;
	uint32_t m_pThreshold;
};


struct GuestDevice
{
	_XMETAL_PushBuffer m_Pusher;
	uint32_t m_StateFlags;
	uint8_t padding2[1911];
	uint32_t m_pPixelShader;
	uint8_t padding3[11];
	uint32_t m_pVertexShader;
	uint32_t m_VertexShaderHandle;
	uint8_t padding4[412];
	uint32_t m_cRefs;
	uint8_t padding5[1444];
	GuestViewport viewport;
	uint8_t padding6[2608];
	uint32_t m_ConstantMode;
	uint8_t padding7[216];
	uint32_t m_pRenderTarget;
	uint8_t padding8[12];
	uint8_t m_FrameBufferSurfaces[0x48];
	uint8_t padding9[2625];
};


static_assert(sizeof(GuestDevice) == 0x24A0);



struct X_D3DPixelContainer : public X_D3DResource
{
	DWORD		Format; // Format information about the texture.
	DWORD       Size; // Size of a non power-of-2 texture, must be zero otherwise
};

struct X_D3DBaseTexture : X_D3DPixelContainer
{
};


struct X_D3DSurface : X_D3DPixelContainer
{
	uint32_t Parent;
};


typedef enum _X_D3DRESOURCETYPE
{
	X_D3DRTYPE_NONE = 0,
	X_D3DRTYPE_SURFACE = 1, // = D3DRESOURCETYPE.D3DRTYPE_SURFACE
	X_D3DRTYPE_VOLUME = 2, // = D3DRESOURCETYPE.D3DRTYPE_VOLUME
	X_D3DRTYPE_TEXTURE = 3, // = D3DRESOURCETYPE.D3DRTYPE_TEXTURE
	X_D3DRTYPE_VOLUMETEXTURE = 4, // = D3DRESOURCETYPE.D3DRTYPE_VOLUMETEXTURE
	X_D3DRTYPE_CUBETEXTURE = 5, // = D3DRESOURCETYPE.D3DRTYPE_CUBETEXTURE
	X_D3DRTYPE_VERTEXBUFFER = 6, // = D3DRESOURCETYPE.D3DRTYPE_VERTEXBUFFER
	X_D3DRTYPE_INDEXBUFFER = 7, // = D3DRESOURCETYPE.D3DRTYPE_INDEXBUFFER
	X_D3DRTYPE_PUSHBUFFER = 8,
	X_D3DRTYPE_PALETTE = 9,
	X_D3DRTYPE_FIXUP = 10,

	X_D3DRTYPE_FORCE_DWORD = 0x7fffffff
}
X_D3DRESOURCETYPE;

// Values, used with D3DTSS_TEXCOORDINDEX, to specify that the vertex data (position
// and normal in the camera space) should be taken as texture coordinates.
// Low 16 bits are used to specify texture coordinate index, to take the WRAP mode from.
#define X_D3DTSS_TCI_PASSTHRU                             0x00000000
#define X_D3DTSS_TCI_CAMERASPACENORMAL                    0x00010000
#define X_D3DTSS_TCI_CAMERASPACEPOSITION                  0x00020000
#define X_D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR          0x00030000
#define X_D3DTSS_TCI_OBJECT                               0x00040000 
#define X_D3DTSS_TCI_SPHEREMAP                            0x00050000

enum X_D3DTRANSFORMSTATETYPE {
	X_D3DTS_VIEW = 0,
	X_D3DTS_PROJECTION = 1,
	X_D3DTS_TEXTURE0 = 2,
	X_D3DTS_TEXTURE1 = 3,
	X_D3DTS_TEXTURE2 = 4,
	X_D3DTS_TEXTURE3 = 5,
	X_D3DTS_WORLD = 6,
	X_D3DTS_WORLD1 = 7,
	X_D3DTS_WORLD2 = 8,
	X_D3DTS_WORLD3 = 9,

	X_D3DTS_MAX = 10,
	X_D3DTS_FORCE_DWORD = 0x7FFFFFFF,
};

enum _D3DSWAPEFFECT : int32_t
{
	D3DSWAPEFFECT_DISCARD = 0x1,
	D3DSWAPEFFECT_FLIP = 0x2,
	D3DSWAPEFFECT_COPY = 0x3,
	D3DSWAPEFFECT_COPY_VSYNC = 0x4,
	D3DSWAPEFFECT_FORCE_DWORD = 0x7FFFFFFF,
};



struct X_D3DPRESENT_PARAMETERS
{
	uint32_t BackBufferWidth;
	uint32_t BackBufferHeight;
	X_D3DFORMAT BackBufferFormat;
	uint32_t BackBufferCount;
	uint32_t MultiSampleType;
	_D3DSWAPEFFECT SwapEffect;
	uint32_t hDeviceWindow;
	int32_t Windowed;
	int32_t EnableAutoDepthStencil;
	X_D3DFORMAT AutoDepthStencilFormat;
	uint32_t Flags;
	uint32_t FullScreen_RefreshRateInHz;
	uint32_t FullScreen_PresentationInterval;
};



#ifndef D3DMATRIX_DEFINED
typedef struct _D3DMATRIX {
	union {
		struct {
			float        _11, _12, _13, _14;
			float        _21, _22, _23, _24;
			float        _31, _32, _33, _34;
			float        _41, _42, _43, _44;

		};
		float m[4][4];
	};
} D3DMATRIX;
typedef D3DMATRIX D3DXMATRIX;
#define D3DMATRIX_DEFINED
#endif

typedef enum _D3DLIGHTTYPE {
	D3DLIGHT_POINT = 1,
	D3DLIGHT_SPOT = 2,
	D3DLIGHT_DIRECTIONAL = 3,
	D3DLIGHT_FORCE_DWORD = 0x7fffffff, /* force 32-bit size enum */
} D3DLIGHTTYPE;

#ifndef D3DCOLORVALUE_DEFINED
typedef struct _D3DCOLORVALUE {
	float r;
	float g;
	float b;
	float a;
} D3DCOLORVALUE;
#define D3DCOLORVALUE_DEFINED
#endif

typedef enum _D3DFOGMODE {
	D3DFOG_NONE = 0,
	D3DFOG_EXP = 1,
	D3DFOG_EXP2 = 2,
	D3DFOG_LINEAR = 3,
	D3DFOG_FORCE_DWORD = 0x7fffffff, /* force 32-bit size enum */
} D3DFOGMODE;

typedef struct _D3DVECTOR {
	float x;
	float y;
	float z;
} D3DVECTOR;

typedef struct _D3DVECTOR4 {
	float x;
	float y;
	float z;
	float w;
} D3DVECTOR4;

typedef struct _X_D3DLIGHT8 {
	D3DLIGHTTYPE    Type;            /* Type of light source */
	D3DCOLORVALUE   Diffuse;         /* Diffuse color of light */
	D3DCOLORVALUE   Specular;        /* Specular color of light */
	D3DCOLORVALUE   Ambient;         /* Ambient color of light */
	D3DVECTOR       Position;         /* Position in world space */
	D3DVECTOR       Direction;        /* Direction in world space */
	float           Range;            /* Cutoff range */
	float           Falloff;          /* Falloff */
	float           Attenuation0;     /* Constant attenuation */
	float           Attenuation1;     /* Linear attenuation */
	float           Attenuation2;     /* Quadratic attenuation */
	float           Theta;            /* Inner angle of spotlight cone */
	float           Phi;              /* Outer angle of spotlight cone */
} X_D3DLIGHT8;

struct D3D8LightState {
	std::array<X_D3DLIGHT8, 4096> Lights;

	// The indices of last 8 enabled lights
	// From least recently to most recently enabled
	// -1 represents empty light slots
	// which always appear after enabled lights
	std::array<int, 8> EnabledLights;

	// The number of enabled lights
	uint32_t EnabledLightCount = 0;

	D3D8LightState();

	// Enable a light. Returns true when the enabled light set or order changed.
	bool EnableLight(uint32_t index, bool enable);
};

void LightEnable(uint32_t Index, int32_t bEnable);

typedef enum _D3DVERTEXBLENDFLAGS
{
	X_D3DVBF_DISABLE = 0,              // 1 matrix,   0 weights => final weight effectively 1 (Disable vertex blending)
	X_D3DVBF_1WEIGHTS = 1,             // 2 matrices, 1 weights => final weight calculated
	X_D3DVBF_2WEIGHTS = 3,             // 3 matrices, 2 weights => final weight calculated
	X_D3DVBF_3WEIGHTS = 5,             // 4 matrices, 3 weights => final weight calculated
	X_D3DVBF_2WEIGHTS2MATRICES = 2,    // 2 matrices, 2 weights (Xbox ext.)
	X_D3DVBF_3WEIGHTS3MATRICES = 4,    // 3 matrices, 3 weights (Xbox ext.)
	X_D3DVBF_4WEIGHTS4MATRICES = 6,    // 4 matrices, 4 weights (Xbox ext.)
	X_D3DVBF_FORCE_DWORD = 0x7fffffff
} X_D3DVERTEXBLENDFLAGS;

// FVF Definitions
#define X_D3DFVF_RESERVED0        0x001
#define X_D3DFVF_XYZ              0x002
#define X_D3DFVF_XYZRHW           0x004
#define X_D3DFVF_XYZB1            0x006
#define X_D3DFVF_XYZB2            0x008
#define X_D3DFVF_XYZB3            0x00a
#define X_D3DFVF_XYZB4            0x00c
#define X_D3DFVF_POSITION_MASK    0x00E
#define X_D3DFVF_NORMAL           0x010
#define X_D3DFVF_RESERVED1        0x020
#define X_D3DFVF_DIFFUSE          0x040
#define X_D3DFVF_SPECULAR         0x080
#define X_D3DFVF_TEXCOUNT_MASK    0xf00
#define X_D3DFVF_TEXCOUNT_SHIFT   8
#define X_D3DFVF_TEX0             0x000
#define X_D3DFVF_TEX1             0x100
#define X_D3DFVF_TEX2             0x200
#define X_D3DFVF_TEX3             0x300
#define X_D3DFVF_TEX4             0x400
#define X_D3DFVF_TEX5             0x500
#define X_D3DFVF_TEX6             0x600
#define X_D3DFVF_TEX7             0x700
#define X_D3DFVF_TEX8             0x800
#define X_D3DFVF_TEXTUREFORMAT1   0x003
#define X_D3DFVF_TEXTUREFORMAT2   0x000
#define X_D3DFVF_TEXTUREFORMAT3   0x001
#define X_D3DFVF_TEXTUREFORMAT4   0x002

enum _D3DBLEND : int32_t
{
	D3DBLEND_ZERO = 0x0,
	D3DBLEND_ONE = 0x1,
	D3DBLEND_SRCCOLOR = 0x300,
	D3DBLEND_INVSRCCOLOR = 0x301,
	D3DBLEND_SRCALPHA = 0x302,
	D3DBLEND_INVSRCALPHA = 0x303,
	D3DBLEND_DESTALPHA = 0x304,
	D3DBLEND_INVDESTALPHA = 0x305,
	D3DBLEND_DESTCOLOR = 0x306,
	D3DBLEND_INVDESTCOLOR = 0x307,
	D3DBLEND_SRCALPHASAT = 0x308,
	D3DBLEND_CONSTANTCOLOR = 0x8001,
	D3DBLEND_INVCONSTANTCOLOR = 0x8002,
	D3DBLEND_CONSTANTALPHA = 0x8003,
	D3DBLEND_INVCONSTANTALPHA = 0x8004,
	D3DBLEND_FORCE_DWORD = 0x7FFFFFFF,
};

enum _D3DBLENDOP : int32_t
{
	D3DBLENDOP_ADD = 0x8006,
	D3DBLENDOP_SUBTRACT = 0x800A,
	D3DBLENDOP_REVSUBTRACT = 0x800B,
	D3DBLENDOP_MIN = 0x8007,
	D3DBLENDOP_MAX = 0x8008,
	D3DBLENDOP_ADDSIGNED = 0xF006,
	D3DBLENDOP_REVSUBTRACTSIGNED = 0xF005,
	D3DBLENDOP_FORCE_DWORD = 0x7FFFFFFF,
};

//this seems to vary per game, likely because of the Xbox SDK version.
enum X_D3DTEXTURESTAGESTATETYPE : int32_t
{
	X_D3DTSS_ADDRESSU = 0,
	X_D3DTSS_ADDRESSV = 1,
	X_D3DTSS_ADDRESSW = 2,
	X_D3DTSS_MAGFILTER = 3,
	X_D3DTSS_MINFILTER = 4,
	X_D3DTSS_MIPFILTER = 5,
	X_D3DTSS_MIPMAPLODBIAS = 6,
	X_D3DTSS_MAXMIPLEVEL = 7,
	X_D3DTSS_MAXANISOTROPY = 8,
	X_D3DTSS_COLORKEYOP = 9, // Xbox ext.
	X_D3DTSS_COLORSIGN = 10, // Xbox ext.
	X_D3DTSS_ALPHAKILL = 11, // Xbox ext.
	X_D3DTSS_COLOROP = 12,
	X_D3DTSS_COLORARG0 = 13,
	X_D3DTSS_COLORARG1 = 14,
	X_D3DTSS_COLORARG2 = 15,
	X_D3DTSS_ALPHAOP = 16,
	X_D3DTSS_ALPHAARG0 = 17,
	X_D3DTSS_ALPHAARG1 = 18,
	X_D3DTSS_ALPHAARG2 = 19,
	X_D3DTSS_RESULTARG = 20,
	X_D3DTSS_TEXTURETRANSFORMFLAGS = 21,
	// End of "deferred" texture states, continuing with the rest :
	X_D3DTSS_BUMPENVMAT00 = 22,
	X_D3DTSS_BUMPENVMAT01 = 23,
	X_D3DTSS_BUMPENVMAT11 = 24,
	X_D3DTSS_BUMPENVMAT10 = 25,
	X_D3DTSS_BUMPENVLSCALE = 26,
	X_D3DTSS_BUMPENVLOFFSET = 27,
	X_D3DTSS_TEXCOORDINDEX = 28,
	X_D3DTSS_BORDERCOLOR = 29,
	X_D3DTSS_COLORKEYCOLOR = 30, // Xbox ext.
	X_D3DTSS_UNSUPPORTED = 31, // Note : Somehow, this one comes through D3DDevice_SetTextureStageStateNotInline sometimes

	X_D3DTSS_MAX = 0x20,
	X_D3DTSS_FORCE_DWORD = 0x7FFFFFFF,
};

enum _D3DTEXTUREADDRESS : int32_t
{
	D3DTADDRESS_WRAP = 0x1,
	D3DTADDRESS_MIRROR = 0x2,
	D3DTADDRESS_CLAMP = 0x3,
	D3DTADDRESS_BORDER = 0x4,
	D3DTADDRESS_CLAMPTOEDGE = 0x5,
	D3DTADDRESS_MAX = 0x6,
	D3DTADDRESS_FORCE_DWORD = 0x7FFFFFFF,
};

// vertex shader input registers for fixed function vertex shader



//typedef X_D3DVSDE = X_D3DVSDE_POSITION..High(DWORD)-2; // Unique declaration to make overloads possible;


//
// DeclTypes
//
#define D3DDTCAPS_UBYTE4     0x00000001L
#define D3DDTCAPS_UBYTE4N    0x00000002L
#define D3DDTCAPS_SHORT2N    0x00000004L
#define D3DDTCAPS_SHORT4N    0x00000008L
#define D3DDTCAPS_USHORT2N   0x00000010L
#define D3DDTCAPS_USHORT4N   0x00000020L
#define D3DDTCAPS_UDEC3      0x00000040L
#define D3DDTCAPS_DEC3N      0x00000080L
#define D3DDTCAPS_FLOAT16_2  0x00000100L
#define D3DDTCAPS_FLOAT16_4  0x00000200L




// X_D3DVertexShader.Flags values :
#define X_VERTEXSHADER_FLAG_WRITE           (1 <<  0) // = 0x0001 // Set for Xbox ShaderType != X_VST_NORMAL 
#define X_VERTEXSHADER_FLAG_PASSTHROUGH     (1 <<  1) // = 0x0002
#define X_VERTEXSHADER_FLAG_STATE           (1 <<  3) // = 0x0008 // Set for Xbox ShaderType == X_VST_STATE
#define X_VERTEXSHADER_FLAG_PROGRAM         (1 <<  4) // = 0x0010 // Set when X_D3DVertexShader was created with assigned function data; introduced after XDK 3948; don't use directly, use g_X_VERTEXSHADER_FLAG_PROGRAM instead
#define X_VERTEXSHADER_FLAG_HASDIFFUSE      (1 << 10) // = 0x0400 Corresponds to X_D3DUSAGE_PERSISTENTDIFFUSE
#define X_VERTEXSHADER_FLAG_HASSPECULAR     (1 << 11) // = 0x0800 Corresponds to X_D3DUSAGE_PERSISTENTSPECULAR
#define X_VERTEXSHADER_FLAG_HASBACKDIFFUSE  (1 << 12) // = 0x1000 Corresponds to X_D3DUSAGE_PERSISTENTBACKDIFFUSE
#define X_VERTEXSHADER_FLAG_HASBACKSPECULAR (1 << 13) // = 0x2000 Corresponds to X_D3DUSAGE_PERSISTENTBACKSPECULAR

static uint32_t  g_X_VERTEXSHADER_FLAG_PROGRAM = X_VERTEXSHADER_FLAG_PROGRAM;

#define X_D3DFVF_TEXCOORDSIZE_SHIFT(Index) ((Index) * 2 + 16)


/* Flags to construct D3DRS_COLORWRITEENABLE */
#define D3DCOLORWRITEENABLE_RED     (1L<<0)
#define D3DCOLORWRITEENABLE_GREEN   (1L<<1)
#define D3DCOLORWRITEENABLE_BLUE    (1L<<2)
#define D3DCOLORWRITEENABLE_ALPHA   (1L<<3)

/*
 * Values for COLORARG0,1,2, ALPHAARG0,1,2, and RESULTARG texture blending
 * operations set in texture processing stage controls in D3DRENDERSTATE.
 */
#define D3DTA_SELECTMASK        0x0000000f  // mask for arg selector
#define D3DTA_DIFFUSE           0x00000000  // select diffuse color (read only)
#define D3DTA_CURRENT           0x00000001  // select stage destination register (read/write)
#define D3DTA_TEXTURE           0x00000002  // select texture color (read only)
#define D3DTA_TFACTOR           0x00000003  // select D3DRS_TEXTUREFACTOR (read only)
#define D3DTA_SPECULAR          0x00000004  // select specular color (read only)
#define D3DTA_TEMP              0x00000005  // select temporary register color (read/write)
#define D3DTA_COMPLEMENT        0x00000010  // take 1.0 - x (read modifier)
#define D3DTA_ALPHAREPLICATE    0x00000020  // replicate alpha to color components (read modifier)

#define X_D3D_RENDER_MEMORY_ALIGNMENT     64
#define X_D3DSURFACE_ALIGNMENT            X_D3D_RENDER_MEMORY_ALIGNMENT
#define X_D3DTEXTURE_ALIGNMENT            (2 * X_D3D_RENDER_MEMORY_ALIGNMENT)
#define X_D3DTEXTURE_CUBEFACE_ALIGNMENT   (2 * X_D3D_RENDER_MEMORY_ALIGNMENT)
#define X_D3DTEXTURE_PITCH_ALIGNMENT      X_D3D_RENDER_MEMORY_ALIGNMENT
#define X_D3DTEXTURE_PITCH_MIN            X_D3DTEXTURE_PITCH_ALIGNMENT

 // pixel container "size" masks
 // The layout of the size field, used for non swizzled or compressed textures.
 //
 // The Size field of a container will be zero if the texture is swizzled or compressed.
 // It is guarenteed to be non-zero otherwise because either the height/width will be
 // greater than one or the pitch adjust will be nonzero because the minimum texture
 // pitch is 8 bytes.
#define X_D3DSIZE_WIDTH_MASK              0x00000FFF   // Width of the texture - 1, in texels
//#define X_D3DSIZE_WIDTH_SHIFT           0
#define X_D3DSIZE_HEIGHT_MASK             0x00FFF000   // Height of the texture - 1, in texels
#define X_D3DSIZE_HEIGHT_SHIFT            12
#define X_D3DSIZE_PITCH_MASK              0xFF000000   // Pitch / 64 - 1
#define X_D3DSIZE_PITCH_SHIFT             24


typedef enum _D3DCUBEMAP_FACES
{
	D3DCUBEMAP_FACE_POSITIVE_X = 0,
	D3DCUBEMAP_FACE_NEGATIVE_X = 1,
	D3DCUBEMAP_FACE_POSITIVE_Y = 2,
	D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
	D3DCUBEMAP_FACE_POSITIVE_Z = 4,
	D3DCUBEMAP_FACE_NEGATIVE_Z = 5,

	D3DCUBEMAP_FACE_FORCE_DWORD = 0x7fffffff
} D3DCUBEMAP_FACES;

typedef enum _D3DTEXTURETRANSFORMFLAGS {
	D3DTTFF_DISABLE = 0,    // texture coordinates are passed directly
	D3DTTFF_COUNT1 = 1,    // rasterizer should expect 1-D texture coords
	D3DTTFF_COUNT2 = 2,    // rasterizer should expect 2-D texture coords
	D3DTTFF_COUNT3 = 3,    // rasterizer should expect 3-D texture coords
	D3DTTFF_COUNT4 = 4,    // rasterizer should expect 4-D texture coords
	D3DTTFF_PROJECTED = 256,  // texcoords to be divided by COUNTth element
	D3DTTFF_FORCE_DWORD = 0x7fffffff,
} D3DTEXTURETRANSFORMFLAGS;


struct RwD3D8Vertex
{
	RwReal x;
	RwReal y;
	RwReal z;
	RwReal rhw;
	uint32_t emissiveColor;
	RwReal u;
	RwReal v;
};


struct IndexedVertex
{
	float x;
	float y;
	float z;
	float R;
	float G;
	float B;
	float u;
	float v;
};

struct UpVertex
{
	float x, y, z, w;
	uint8_t R, G, B, A;
	float u, v;
};


enum class VertexShaderMode {
	FixedFunction,
	// When titles use Xbox fixed function with pre-transformed vertices
	// it actually uses a special "passthrough" shader program
	Passthrough,
	ShaderProgram
};

enum RwRasterType : uint8_t
{
	rwRASTERTYPENORMAL = 0x0,
	rwRASTERTYPEZBUFFER = 0x1,
	rwRASTERTYPECAMERA = 0x2,
	rwRASTERTYPETEXTURE = 0x4,
	rwRASTERTYPECAMERATEXTURE = 0x5,
	rwRASTERTYPEMASK = 0x7,
	rwRASTERDONTALLOCATE = 0x80,
};

struct RwRaster
{
	uint32_t parent; //RwRaster*
	uint32_t cpPixels; //uint8_t* 
	uint32_t palette; //uint8_t*
	int32_t width;
	int32_t height;
	int32_t depth;
	int32_t stride;
	int16_t nOffsetX;
	int16_t nOffsetY;
	RwRasterType cType;
	uint32_t cFlags;
	uint8_t privateFlags;
	uint8_t cFormat;
	uint32_t originalPixels; //uint8_t*
	int32_t originalWidth;
	int32_t originalHeight;
	int32_t originalStride;
};

struct RwTexture
{
	uint32_t raster;   //RwRaster*
	uint32_t dict; //RwTexDictionary ptr
	uint32_t lInDictionary[2];
	char name[32];
	char mask[32];
	uint32_t filterAddressing;
	int32_t refCount;
};

enum DrawCallCaller
{
	Others,
	LoadingLogo,
	CompositeRT,

};

enum class PlumeBufferType : uint8_t
{
	Upload,
	Index,

};

std::unique_ptr<RenderTexture> PlumeCreateTextureWrapper(const RenderTextureDesc& desc);
std::unique_ptr<RenderBuffer> PlumeCreateBufferWrapper(const uint32_t size);
void PlumeSetTextureWrapper(uint32_t descriptorIndex, const RenderTexture* texture, const RenderTextureLayout textureLayout, const RenderTextureView* textureView);
uint32_t AllocateTextureDescriptor();
void FreeTextureDescriptor(const uint32_t descriptorIndex);
void ExecuteCopyCommandList(const std::function<void(RenderCommandList*)>& function);
GuestTexture* CreateTexture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, X_D3DFORMAT format, X_D3DRESOURCETYPE type);
X_D3DRESOURCETYPE GetXboxTextureType(const X_D3DResource* pXboxResource);

// Xbox vertex shader constant modes
#define X_D3DSCM_96CONSTANTS                  0x00 // Enables constants 0..95
#define X_D3DSCM_192CONSTANTS                 0x01 // Enables constants -96..-1 on top of 0..95
#define X_D3DSCM_192CONSTANTSANDFIXEDPIPELINE 0x02 // Unsupported?
#define X_D3DSCM_NORESERVEDCONSTANTS          0x10  // Do not reserve constant -38 and -37

#define X_D3DSCM_RESERVED_CONSTANT_SCALE -38 // Becomes 58 after correction, contains Scale v
#define X_D3DSCM_RESERVED_CONSTANT_OFFSET -37 // Becomes 59 after correction, contains Offset

#define X_D3DSCM_CORRECTION                 96 // Add 96 to arrive at the range 0..191 (instead of -96..95)
#define X_D3DVS_CONSTREG_COUNT              192

// Special Registers, used to pass additional information to the shaders
const uint16_t HOST_D3DVS_CONSTREG_VREGDEFAULTS_BASE = X_D3DVS_CONSTREG_COUNT;
const uint16_t HOST_D3DVS_CONSTREG_VREGDEFAULTS_SIZE = 16u;
#define HOST_D3DVS_CONSTREG_VREGDEFAULTS_FLAG_BASE (HOST_D3DVS_CONSTREG_VREGDEFAULTS_BASE + HOST_D3DVS_CONSTREG_VREGDEFAULTS_SIZE)
#define HOST_D3DVS_CONSTREG_VREGDEFAULTS_FLAG_SIZE 4
#define HOST_D3DVS_SCREENSPACE_SCALE_BASE     (HOST_D3DVS_CONSTREG_VREGDEFAULTS_FLAG_BASE + HOST_D3DVS_CONSTREG_VREGDEFAULTS_FLAG_SIZE)
#define HOST_D3DVS_NORMALIZE_SCALE_SIZE     1
#define HOST_D3DVS_CONSTREG_FULL  (X_D3DVS_CONSTREG_COUNT + 26 + 4 + 4) //+ 1 extra constant to get user resolution

#define HOST_D3DVS_SCREENSPACE_OFFSET_BASE     (HOST_D3DVS_SCREENSPACE_SCALE_BASE + HOST_D3DVS_NORMALIZE_SCALE_SIZE)

#define HOST_D3DVS_NORMALIZE_OFFSET_SIZE     1
#define HOST_D3DVS_TEXTURES_SCALE_BASE             (HOST_D3DVS_SCREENSPACE_OFFSET_BASE + HOST_D3DVS_NORMALIZE_OFFSET_SIZE)
#define HOST_D3DVS_TEXTURES_SCALE_SIZE             4
#define HOST_D3DVS_CONSTREG_FOGINFO                (HOST_D3DVS_TEXTURES_SCALE_BASE + HOST_D3DVS_TEXTURES_SCALE_SIZE)

#define X_D3DSCM_RESERVED_CONSTANT_SCALE_CORRECTED (X_D3DSCM_RESERVED_CONSTANT_SCALE + X_D3DSCM_CORRECTION)
#define X_D3DSCM_RESERVED_CONSTANT_OFFSET_CORRECTED (X_D3DSCM_RESERVED_CONSTANT_OFFSET + X_D3DSCM_CORRECTION)

#define HOST_USER_CONFIG HOST_D3DVS_CONSTREG_FOGINFO + 1

constexpr int PSH_XBOX_CONSTANT_MAX = 30;

void DrawPrimitiveUP(X_D3DPRIMITIVETYPE PrimitiveType, uint32_t VertexCount, void* pVertexStreamZeroData, uint32_t VertexStreamZeroStride);
void DrawIndexedPrimitive(X_D3DPRIMITIVETYPE PrimitiveType, uint32_t VertexCount, int16_t* pIndexData);
void UpdateVertexShader(uint32_t Handle);


extern std::unique_ptr<GuestTexture> LoadCustomTexture(const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping = RenderComponentMapping());

extern void VideoConfigValueChangedCallback(class IConfigDef* config);
GuestBuffer* CreateVertexBuffer(uint32_t length);


uint8_t* ApplyVertexPatch(uint8_t* vertexData, const uint8_t indexStream, const uint32_t VertexCount, const uint32_t originalStride, uint32_t alignedStride, bool useHostMemory = false);
void UnlockVertexBuffer(GuestBuffer* buffer);

void AddDestructResource(GuestResource* ressource);
void* GetDataFromXboxResource(X_D3DResource* pXboxResource);
X_D3DFORMAT GetXboxPixelContainerFormat(const uint32_t XboxPixelContainer_Format);
uint8_t* AllocIntermediaryUploadMemoryWrapper(const void* memory, uint32_t size);
GuestShader* CreateShader(ResourceType resourceType);