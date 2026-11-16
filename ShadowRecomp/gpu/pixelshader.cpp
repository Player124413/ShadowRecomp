#include "video.h"
#include <shader/shader_cache.h>
#include <map>
#include "renderstate.h"
#include "render_commands.h"



// Original code (mostly) re-used from CXBX Reloaded

static GuestPixelShader* g_pXbox_PixelShader = nullptr;
static bool g_isFixedFunctionPixelShader = false;
static bool g_pixelShaderConstantsAnyDirty = true;
uint32_t psh_constants[PSH_XBOX_CONSTANT_MAX][4]; // (slot of 4 because one register = 4 bytes)
bool psh_constants_dirty[PSH_XBOX_CONSTANT_MAX];

typedef struct D3DXCOLOR
{
	FLOAT r, g, b, a;
} D3DXCOLOR, * LPD3DXCOLOR;

extern int32_t* g_texture_stage_states;

constexpr int PSH_XBOX_MAX_C_REGISTER_COUNT = 16;
// Mapping indices of Xbox register combiner constants to host pixel shader constants;
// The first 16 are identity-mapped (C0_1 .. C0_7 are C0 .. C7 on host, C1_0 .. C1_7 are C8 .. C15 on host) :
constexpr int PSH_XBOX_CONSTANT_C0 = 0; // = 0..15
// Then two final combiner constants :
constexpr int PSH_XBOX_CONSTANT_FC0 = PSH_XBOX_CONSTANT_C0 + PSH_XBOX_MAX_C_REGISTER_COUNT; // = 16
constexpr int PSH_XBOX_CONSTANT_FC1 = PSH_XBOX_CONSTANT_FC0 + 1; // = 17
// Fog requires a constant (as host PS1.4 doesn't support the FOG register)
constexpr int PSH_XBOX_CONSTANT_FOG = PSH_XBOX_CONSTANT_FC1 + 1; // = 18
// Bump Environment Material registers
constexpr int PSH_XBOX_CONSTANT_BEM = PSH_XBOX_CONSTANT_FOG + 1; // = 19..22
// Bump map Luminance registers
constexpr int PSH_XBOX_CONSTANT_LUM = PSH_XBOX_CONSTANT_BEM + 4; // = 23..26
// Which winding order to consider as the front face
constexpr int PSH_XBOX_CONSTANT_FRONTFACE_FACTOR = PSH_XBOX_CONSTANT_LUM + 4; // = 27
// Fog table information {Table mode, density, start and end}
constexpr int HOST_D3DPS_CONSTREG_FOGINFO = PSH_XBOX_CONSTANT_FRONTFACE_FACTOR + 1; // = 28
//Fog enable flag
constexpr int PSH_XBOX_CONSTANT_FOGENABLE = HOST_D3DPS_CONSTREG_FOGINFO + 1; // = 29

static std::map<uint64_t, GuestShader*> pixelCache;

GuestPixelShader* GetXboxPixelShader()
{
	return g_pXbox_PixelShader;
}

bool IsFixedFunctionPixelShader()
{
	return g_isFixedFunctionPixelShader;
}

uint64_t GetPixelShaderKey(const uint32_t pXboxPixelShaderDef)
{
	X_D3DPIXELSHADERDEF* pPSDef = (X_D3DPIXELSHADERDEF*)g_memory.Translate(pXboxPixelShaderDef);
	X_D3DPIXELSHADERDEF defForHash = *pPSDef;
	memset(defForHash.PSConstant0, 0, sizeof(defForHash.PSConstant0));
	memset(defForHash.PSConstant1, 0, sizeof(defForHash.PSConstant1));
	uint64_t PixelShaderKey = XXH3_64bits(&defForHash, sizeof(X_D3DPIXELSHADERDEF) - 3 * sizeof(DWORD));
	return PixelShaderKey;
}

void UpdatePixelShaderCacheEntryNew(GuestShader* shader, const uint64_t key)
{
	pixelCache[key] = shader;
}

GuestShader* FindPixelShaderCacheEntryNew(const uint32_t pXboxPixelShaderDef)
{

	auto PShaderKey = GetPixelShaderKey(pXboxPixelShaderDef);
	auto it = pixelCache.find(PShaderKey);
	if (it != pixelCache.end())
	{
		return it->second;
	}

	return nullptr; //findResult != end && findResult->hash == hash ? findResult : nullptr;
}

static void UnpackColor(DWORD color, D3DXCOLOR* fColor)
{
	// Xbox stores colors in 0xAARRGGBB format
	fColor->r = ((color >> 16) & 0xFF) / 255.0f;
	fColor->g = ((color >> 8) & 0xFF) / 255.0f;
	fColor->b = ((color >> 0) & 0xFF) / 255.0f;
	fColor->a = ((color >> 24) & 0xFF) / 255.0f;
}

bool UpdatePixelShaderConstant(uint32_t(*psh_constants)[4])
{
	D3DXCOLOR fColor[PSH_XBOX_CONSTANT_MAX];
	memset(fColor, 0, sizeof(fColor));
	// PSH_XBOX_CONSTANT_C0..C15 are stored as-is in (and should thus be read from) the Xbox render state pixel shader constant slots
	for (unsigned constant_nr = 0; constant_nr < 16; constant_nr++) 
	{
		UnpackColor(GetGuestRenderState(X_D3DRS_PSCONSTANT0_0 + constant_nr), &fColor[PSH_XBOX_CONSTANT_C0 + constant_nr]);
	}

	UnpackColor(GetGuestRenderState(X_D3DRS_PSFINALCOMBINERCONSTANT0), &fColor[PSH_XBOX_CONSTANT_FC0]);
	UnpackColor(GetGuestRenderState(X_D3DRS_PSFINALCOMBINERCONSTANT1), &fColor[PSH_XBOX_CONSTANT_FC1]);

	// Fog requires a constant (as host PS1.4 doesn't support the FOG register)
	// Note : FOG.RGB is correct like this, but FOG.a should be coming
	// from the vertex shader (oFog) - however, D3D8 does not forward this...
	UnpackColor(GetGuestRenderState(X_D3DRS_FOGCOLOR), &fColor[PSH_XBOX_CONSTANT_FOG]);

	for (int i = 0; i < PSH_XBOX_CONSTANT_MAX; i++) 
	{
		switch (i) 
		{
		case PSH_XBOX_CONSTANT_BEM + 0:
		case PSH_XBOX_CONSTANT_BEM + 1:
		case PSH_XBOX_CONSTANT_BEM + 2:
		case PSH_XBOX_CONSTANT_BEM + 3:
		{
			int stage_nr = i - (int)PSH_XBOX_CONSTANT_BEM;
			DWORD* value = (DWORD*)&fColor[i]; // Note : This overlays D3DXCOLOR's FLOAT r, g, b, a

			value[0] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVMAT00];  // Maps to BEM[stage].x
			value[1] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVMAT01]; // Maps to BEM[stage].y
			value[2] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVMAT10];// Maps to BEM[stage].z
			value[3] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVMAT11]; // Maps to BEM[stage].w

			// Note : The TSS values being read here, have been transfered from Xbox to host in XboxTextureStateConverter::Apply()
			break;
		}
		case PSH_XBOX_CONSTANT_LUM + 0:
		case PSH_XBOX_CONSTANT_LUM + 1:
		case PSH_XBOX_CONSTANT_LUM + 2:
		case PSH_XBOX_CONSTANT_LUM + 3:
		{
			int stage_nr = i - PSH_XBOX_CONSTANT_LUM;
			DWORD* value = (DWORD*)&fColor[i]; // Note : This overlays D3DXCOLOR's FLOAT r, g, b, a

			value[0] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVLSCALE];  // Maps to BEM[stage].x
			value[1] = (uint32_t)g_texture_stage_states[stage_nr * X_D3DTSS_MAX + X_D3DTSS_BUMPENVLOFFSET];
			value[2] = 0;
			value[3] = 0;
			break;
		}
		}
	}


	// Control whether to use front or back diffuse/specular colours
	// This factor should be multipled with VFACE
	// Test cases:
	// Amped (snowboard trails should use front colours, but use both CW and CCW winding)
	// TwoSidedLighting sample
	float frontfaceFactor = 0; // 0 == always use the front colours
	if (GetGuestRenderState(X_D3DRS_TWOSIDEDLIGHTING)) 
	{
	
		// VFACE is positive for clockwise faces
		// If Xbox designates counter-clockwise as front-facing, we invert VFACE
		auto cwFrontface = GetGuestRenderState(X_D3DRS_FRONTFACE) == 0x900; // clockwise; = NV097_SET_FRONT_FACE_V_CW = NV2A_FRONT_FACE_CW
		frontfaceFactor = cwFrontface ? 1.0 : -1.0;
	}

	fColor[PSH_XBOX_CONSTANT_FRONTFACE_FACTOR].r = frontfaceFactor;
	fColor[PSH_XBOX_CONSTANT_FRONTFACE_FACTOR].g = 0;
	fColor[PSH_XBOX_CONSTANT_FRONTFACE_FACTOR].b = 0;
	fColor[PSH_XBOX_CONSTANT_FRONTFACE_FACTOR].a = 0;
	float fogEnable = GetGuestRenderState(X_D3DRS_FOGENABLE) > 0;
	const float fogTableMode = GetGuestRenderState(X_D3DRENDERSTATETYPE::X_D3DRS_FOGTABLEMODE);
	const float fogDensity = GetGuestRenderStateAsFloat(X_D3DRENDERSTATETYPE::X_D3DRS_FOGDENSITY);
	const float fogStart = GetGuestRenderStateAsFloat(X_D3DRENDERSTATETYPE::X_D3DRS_FOGSTART);
	const float fogEnd = GetGuestRenderStateAsFloat(X_D3DRENDERSTATETYPE::X_D3DRS_FOGEND);

	fColor[HOST_D3DPS_CONSTREG_FOGINFO].r = fogTableMode;
	fColor[HOST_D3DPS_CONSTREG_FOGINFO].g = fogDensity;
	fColor[HOST_D3DPS_CONSTREG_FOGINFO].b = fogStart;
	fColor[HOST_D3DPS_CONSTREG_FOGINFO].a = fogEnd;
	fColor[PSH_XBOX_CONSTANT_FOGENABLE].r = fogEnable;
	fColor[PSH_XBOX_CONSTANT_FOGENABLE].g = 0;
	fColor[PSH_XBOX_CONSTANT_FOGENABLE].b = 0;
	fColor[PSH_XBOX_CONSTANT_FOGENABLE].a = 0;

	if (memcmp(&psh_constants[0][0], fColor, sizeof(fColor)) == 0)
		return false;

	memcpy(&psh_constants[0][0], fColor, sizeof(fColor));
	return true;

}

//SetPixelShaderConstants
void SetPixelShaderConstants(LocalRenderCommandQueue& queue)
{
	if (g_pixelShaderConstantsAnyDirty)
	{
		auto& cmd = queue.enqueue();
		cmd.type = RenderCommandType::SetPixelShaderConstants;
		cmd.setPixelShaderConstants.memory =
			AllocIntermediaryUploadMemoryWrapper(&psh_constants[0][0], PSH_XBOX_CONSTANT_MAX * sizeof(float) * 4);
		cmd.setPixelShaderConstants.index = 0;
		cmd.setPixelShaderConstants.size = PSH_XBOX_CONSTANT_MAX;
		g_pixelShaderConstantsAnyDirty = false;
	}
}


//SetPixelShaderConstant
X86_FUNC_IMPL(__imp__sub_3DC790);
X86_OVERRIDE_FUNC(sub_3DC790)
{
	__imp__sub_3DC790(ctx, base);

	if (UpdatePixelShaderConstant(psh_constants))
		g_pixelShaderConstantsAnyDirty = true;
}


// SetPixelShader
X86_FUNC_IMPL(__imp__sub_3DC590);
X86_OVERRIDE_FUNC(sub_3DC590)
{
	auto Handle = X86_LOAD_U32(ctx.esp.u32 + 4);

	__imp__sub_3DC590(ctx, base);

	RenderCommand cmd;
	cmd.type = RenderCommandType::SetPixelShader;
	g_pXbox_PixelShader = (GuestPixelShader*)g_memory.Translate(Handle);
	if (Handle)
	{
		// Copy the Pixel Shader data to our RenderState handler (this includes values for pixel shader constants)
		// This mirrors the fact that unpatched SetPixelShader does the same thing!

		auto pXboxPixelShaderDef = X86_LOAD_U32(Handle + 0x8);
		X_D3DPIXELSHADERDEF* pPSDef = (X_D3DPIXELSHADERDEF*)g_memory.Translate(pXboxPixelShaderDef);
		memcpy(GetPixelShaderRenderStatePointer(), pPSDef, sizeof(X_D3DPIXELSHADERDEF) - 3 * sizeof(DWORD));
		// Copy the PSDef.PSTextureModes field to it's dedicated slot, which lies outside the range of PixelShader render state slots
		// Note : This seems to be what XDK's do as well. Needs verification.
		SetGuestRenderState(X_D3DRS_PSTEXTUREMODES, pPSDef->PSTextureModes);

		auto shader = CreateShader(ResourceType::PixelShader);

		if (shader == nullptr)
		{
			return;
		}
		else
		{
			cmd.setPixelShader.shader = shader;
		}
	}
	else
	{
		cmd.setPixelShader.shader = nullptr;
	}

	cmd.setPixelShader.isFixed = Handle == 0;
	g_isFixedFunctionPixelShader = cmd.setPixelShader.isFixed;
	g_pendingRenderCommands.enqueue() = cmd;
}

const uint8_t* CreateRecompiledPixelShader(X_D3DPIXELSHADERDEF* PSDef, void* psGuestInf, size_t* outSize)
{
#ifdef SHADOW_RECOMP_NO_RUNTIME_SHADER_COMPILER
	// See CreateRecompiledShader in vertexshader.cpp.
	(void)PSDef;
	(void)psGuestInf;
	if (outSize != nullptr)
		*outSize = 0;

	return nullptr;
#else
	pixelShader_GuestInfo* psGuestInfo = static_cast<pixelShader_GuestInfo*>(psGuestInf);
	const bool isvulkan = Video::is_vulkan();
	bool exportHLSLFile = false;

#ifdef _DEBUG
	exportHLSLFile = true;
#endif

	auto pShaderOut = XSR_CreateRecompiledPixelShader(PSDef, outSize, psGuestInfo, isvulkan, exportHLSLFile);
	return pShaderOut;
#endif

}