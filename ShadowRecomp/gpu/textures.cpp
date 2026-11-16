#include "video.h"
#include "swizzle.h"
#include <user/config.h>
#include <input/input.h>
#include <bc_diff.h>
#include <kernel/function.h>
#include "guest_convert.h"
#include "render_commands.h"
#include "vertexshader.h"

#ifdef __ANDROID__
#include "android_video.h"
#endif

// This page handle textures process, such as palette conversion (originally used on Xbox),
// copy to the gpu, the custom texs GUI made for Recomp etc.


struct TextureDescriptorAllocator;
extern bool foundTex;
extern std::unique_ptr<uint8_t[]> g_buttonBcDiff_GC;
extern std::unique_ptr<uint8_t[]> g_buttonBcDiff_PS2;
const uint32_t RwD3D8RasterExtOffsetAddr = 0x57DD20;

uint64_t NextGuestTextureGeneration();
//we need to copy the palette data so we can convert properly when copy texture in the GPU
static uint32_t* g_currentPaletteData;

//rwD3D8RasterLockPalette
X86_FUNC_IMPL(__imp__sub_2DF9F0);
X86_OVERRIDE_FUNC(sub_2DF9F0)
{
	__imp__sub_2DF9F0(ctx, base);
	if (ctx.eax.u32 == 1)
	{
		auto paletteData = X86_LOAD_U32(ctx.ecx.u32);
		g_currentPaletteData = (uint32_t*)(base + paletteData);
	}
}


unsigned int GetPixelContainerMipMapLevels(X_D3DPixelContainer* pPixelContainer)
{
	if (pPixelContainer->Size == 0)
		return (pPixelContainer->Format & X_D3DFORMAT_MIPMAP_MASK) >> X_D3DFORMAT_MIPMAP_SHIFT;

	return 1;
}



static bool LoadTexture(GuestTexture& texture, const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping, bool forceCubeMap = false)
{
	ddspp::Descriptor ddsDesc;
	if (ddspp::decode_header((unsigned char*)(data), ddsDesc) != ddspp::Error)
	{
		forceCubeMap &= (ddsDesc.type == ddspp::Texture2D) && (ddsDesc.arraySize == 1);
		uint32_t arraySize = ddsDesc.type == ddspp::TextureType::Cubemap ? (ddsDesc.arraySize * 6) : ddsDesc.arraySize;

		RenderTextureDesc desc;
		desc.dimension = ConvertTextureDimension(ddsDesc.type);
		desc.width = ddsDesc.width;
		desc.height = ddsDesc.height;
		desc.depth = ddsDesc.depth;
		desc.mipLevels = ddsDesc.numMips;
		desc.arraySize = arraySize;
		desc.format = ConvertDXGIFormat(ddsDesc.format);
		desc.flags = ddsDesc.type == ddspp::TextureType::Cubemap ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE;

		// Bytes of one pixel the data is unpacked to on a GPU that cannot sample the
		// block-compressed format of the file; 0 where the data is uploaded as it is.
		uint32_t unpackedPixelSize = 0;
#ifdef __ANDROID__
		BcDecode::EFormat unpackFrom;
		desc.format = AndroidVideo::HostTextureFormat(desc.format, unpackFrom);
		unpackedPixelSize = BcDecode::PixelSize(unpackFrom);
#endif

		if (forceCubeMap)
		{
			desc.arraySize = 6;
			desc.flags = RenderTextureFlag::CUBE;
		}

		texture.textureHolder = PlumeCreateTextureWrapper(desc);
		texture.texture = texture.textureHolder.get();
		texture.textureGeneration = NextGuestTextureGeneration();
		texture.layout = RenderTextureLayout::COPY_DEST;

		RenderTextureViewDesc viewDesc;
		viewDesc.format = desc.format;
		viewDesc.dimension = ConvertTextureViewDimension(ddsDesc.type);
		viewDesc.mipLevels = ddsDesc.numMips;
		viewDesc.componentMapping = componentMapping;

		if (forceCubeMap)
			viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;

		texture.textureView = texture.texture->createTextureView(viewDesc);
		texture.descriptorIndex = AllocateTextureDescriptor();
		PlumeSetTextureWrapper(texture.descriptorIndex, texture.texture, RenderTextureLayout::SHADER_READ, texture.textureView.get());

		texture.width = ddsDesc.width;
		texture.height = ddsDesc.height;
		texture.viewDimension = viewDesc.dimension;

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

		for (uint32_t arraySlice = 0; arraySlice < arraySize; arraySlice++)
		{
			for (uint32_t mipSlice = 0; mipSlice < ddsDesc.numMips; mipSlice++)
			{
				auto& slice = slices.emplace_back();

				slice.width = std::max(1u, ddsDesc.width >> mipSlice);
				slice.height = std::max(1u, ddsDesc.height >> mipSlice);
				slice.depth = std::max(1u, ddsDesc.depth >> mipSlice);
				slice.srcOffset = curSrcOffset;
				slice.dstOffset = curDstOffset;
				uint32_t rowPitch = ((slice.width + ddsDesc.blockWidth - 1) / ddsDesc.blockWidth) * ddsDesc.bitsPerPixelOrBlock;
				slice.srcRowPitch = (rowPitch + 7) / 8;
				slice.dstRowPitch = (slice.srcRowPitch + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
				slice.rowCount = (slice.height + ddsDesc.blockHeight - 1) / ddsDesc.blockHeight;

				// Unpacked, a slice is rows of pixels rather than rows of blocks.
				uint32_t dstRowCount = slice.rowCount;
				if (unpackedPixelSize != 0)
				{
					slice.dstRowPitch = (slice.width * unpackedPixelSize + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
					dstRowCount = slice.height;
				}

				curSrcOffset += slice.srcRowPitch * slice.rowCount * slice.depth;
				curDstOffset += (slice.dstRowPitch * dstRowCount * slice.depth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
			}
		}

		auto uploadBuffer = PlumeCreateBufferWrapper(curDstOffset);
		uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

		for (auto& slice : slices)
		{
			const uint8_t* srcData = data + ddsDesc.headerSize + slice.srcOffset;
			uint8_t* dstData = mappedMemory + slice.dstOffset;

#ifdef __ANDROID__
			if (unpackedPixelSize != 0)
			{
				for (uint32_t z = 0; z < slice.depth; z++)
				{
					BcDecode::Decode(unpackFrom, srcData + size_t(z) * slice.srcRowPitch * slice.rowCount, slice.srcRowPitch,
						slice.width, slice.height, dstData + size_t(z) * slice.dstRowPitch * slice.height, slice.dstRowPitch);
				}

				continue;
			}
#endif

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

		uploadBuffer->unmap();

		ExecuteCopyCommandList([&](RenderCommandList* commandList)
			{
				commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

				auto copyTextureRegion = [&](Slice& slice, uint32_t subresourceIndex)
					{
						// Plume wants the width of a row in pixels.
						uint32_t rowWidth = (slice.dstRowPitch * 8) / ddsDesc.bitsPerPixelOrBlock * ddsDesc.blockWidth;
#ifdef __ANDROID__
						if (unpackedPixelSize != 0)
							rowWidth = slice.dstRowPitch / unpackedPixelSize;
#endif

						commandList->copyTextureRegion(
							RenderTextureCopyLocation::Subresource(texture.texture, subresourceIndex % ddsDesc.numMips, subresourceIndex / ddsDesc.numMips),
							RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), desc.format, slice.width, slice.height, slice.depth, rowWidth, slice.dstOffset));
					};

				for (size_t i = 0; i < slices.size(); i++)
					copyTextureRegion(slices[i], i);

				// Duplicate the first face across the remaining 6 faces.
				if (forceCubeMap)
				{
					for (size_t i = 1; i < 6; i++)
					{
						for (size_t j = 0; j < slices.size(); j++)
							copyTextureRegion(slices[j], (slices.size() * i) + j);
					}
				}
			});

		return true;
	}

	int width, height;
	void* stbImage = stbi_load_from_memory(data, dataSize, &width, &height, nullptr, 4);

	if (stbImage != nullptr)
	{
		texture.width = width;
		texture.height = height;
		texture.textureHolder = PlumeCreateTextureWrapper(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM));
		texture.texture = texture.textureHolder.get();
		texture.textureGeneration = NextGuestTextureGeneration();
		texture.viewDimension = RenderTextureViewDimension::TEXTURE_2D;
		texture.layout = RenderTextureLayout::COPY_DEST;

		texture.descriptorIndex = AllocateTextureDescriptor();
		PlumeSetTextureWrapper(texture.descriptorIndex, texture.texture, RenderTextureLayout::SHADER_READ, nullptr);

		uint32_t rowPitch = (width * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
		uint32_t slicePitch = rowPitch * height;

		auto uploadBuffer = PlumeCreateBufferWrapper(slicePitch);
		uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

		if (rowPitch == (width * 4))
		{
			memcpy(mappedMemory, stbImage, slicePitch);
		}
		else
		{
			auto data = reinterpret_cast<const uint8_t*>(stbImage);

			for (size_t i = 0; i < height; i++)
			{
				memcpy(mappedMemory, data, width * 4);
				data += width * 4;
				mappedMemory += rowPitch;
			}
		}

		uploadBuffer->unmap();

		stbi_image_free(stbImage);


		ExecuteCopyCommandList([&](RenderCommandList* commandList)
			{
				commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

				commandList->copyTextureRegion(
					RenderTextureCopyLocation::Subresource(texture.texture, 0),
					RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, rowPitch / 4, 0));
			});

		return true;
	}

	return false;
}


std::unique_ptr<GuestTexture> LoadCustomTexture(const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping)
{
	GuestTexture texture(ResourceType::Texture);

	if (LoadTexture(texture, data, dataSize, componentMapping))
		return std::make_unique<GuestTexture>(std::move(texture));

	return nullptr;
}



static bool DiffPatchControllerGUITexture(GuestTexture* texture, uint8_t* data, uint32_t dataSize, XXH64_hash_t hash)
{
	const bool isAuto = Config::ControllerIcons.Value == EControllerIcons::Auto;
	bool isXbox = Config::ControllerIcons.Value == EControllerIcons::Xbox || isAuto && input::g_inputDeviceController == input::EInputDevice::Xbox;
	bool isUnknown = isAuto && input::g_inputDeviceController == input::EInputDevice::Unknown;

	if (isXbox || isUnknown)
		return false;


	const bool isGC = Config::ControllerIcons.Value == EControllerIcons::Gamecube;
	auto ptr = g_buttonBcDiff_GC.get();
	if (Config::ControllerIcons == EControllerIcons::PlayStation2 || isAuto && input::g_inputDeviceController == input::EInputDevice::PlayStation)
	{
		ptr = g_buttonBcDiff_PS2.get();
	}

	auto header = reinterpret_cast<BlockCompressionDiffPatchHeader*>(ptr);
	auto entries = reinterpret_cast<BlockCompressionDiffEntry*>(ptr + header->entriesOffset);
	auto end = entries + header->entryCount;

	auto findResult = std::lower_bound(entries, end, hash, [](BlockCompressionDiffEntry& lhs, XXH64_hash_t rhs)
		{
			return lhs.hash < rhs;
		});

	if (findResult != end && findResult->hash == hash)
	{
		assert(findResult->dataSize <= dataSize);
		memcpy(data, ptr + findResult->dataOffset, findResult->dataSize);

		GuestTexture patchedTexture(ResourceType::Texture);
		if (LoadTexture(patchedTexture, data, dataSize, {}))
			texture->patchedTexture = std::make_unique<GuestTexture>(std::move(patchedTexture));

		return true;
	}

	return false;

}

void CopyTextureToGPU(GuestTexture* tex)
{
	auto base = g_memory.base;
	auto format = GetXboxPixelContainerFormat((X_D3DFORMAT)tex->guestResource.Format);
	bool isSwizzled = FormatIsSwizzled(format);
	bool bCompressed = XBFormatIsCompressed(format);
	bool bCubemap = tex->guestResource.Format & X_D3DFORMAT_CUBEMAP;
	bool bConvertARGB = XBFormatRequiresConversionToARGB(format); // convert to RGBA if needed

	struct Slice
	{
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t srcOffset; // offset in xbox memory (relative to VirtualAddr + header)
		uint32_t dstOffset;  // offset in upload buffer
		uint32_t srcRowPitch; // as stored on Xbox (bytes or block-row bytes)
		uint32_t dstRowPitch; // aligned row pitch used in upload buffer
		uint32_t rowCount;   // number of rows (or block rows for compressed)
	};

	std::vector<Slice> slices;
	uint32_t curSrcOffset = 0;
	uint32_t curDstOffset = 0;
	uint32_t baseW = tex->width;
	uint32_t baseH = tex->height;
	uint32_t baseD = tex->depth == 0 ? 1 : tex->depth;

	UINT dwMipMapLevels = GetPixelContainerMipMapLevels((X_D3DPixelContainer*)&tex->guestResource);

	D3DCUBEMAP_FACES last_face = (bCubemap) ? D3DCUBEMAP_FACE_NEGATIVE_Z : D3DCUBEMAP_FACE_POSITIVE_X;

	// Block size only applies to compressed DXT formats
	// DXT1 block size is 8 bytes
	// Other Xbox DXT formats are 16 bytes
	DWORD blockSize = 0;

	if (bCompressed)
	{
		blockSize = format == X_D3DFMT_DXT1 ? 8 : 16;
	}

	// Whether the blocks are unpacked into the plain texture a GPU without block-compressed
	// textures was given instead (see CreateTexture), and what they are unpacked from.
	bool bUnpack = false;
#ifdef __ANDROID__
	BcDecode::EFormat unpackFrom = BcDecode::EFormat::None;
	if (bCompressed)
	{
		AndroidVideo::HostTextureFormat(ConvertFormat(format), unpackFrom);
		bUnpack = unpackFrom != BcDecode::EFormat::None && RenderFormatBlockWidth(tex->format) <= 1;
	}
#endif

	for (int32_t face = D3DCUBEMAP_FACE_POSITIVE_X; face <= last_face; face++)
	{
		for (uint32_t mipmap_level = 0; mipmap_level < dwMipMapLevels; mipmap_level++)
		{
			auto& slice = slices.emplace_back();

			//ensures that we never go below 1 when computing sizes for each level.
			slice.width = std::max(1u, baseW >> mipmap_level);
			slice.height = std::max(1u, baseH >> mipmap_level);
			slice.depth = std::max(1u, baseD >> mipmap_level);


			// compute srcRowPitch (Xbox layout)
			if (bCompressed)
			{
				// rowPitch in bytes for block columns (each block covers 4 pixels horizontally)
				uint32_t blockCols = (slice.width + 3) / 4;
				slice.srcRowPitch = blockCols * blockSize;
				slice.rowCount = (slice.height + 3) / 4;
			}
			else
			{
				uint32_t bpp = XBFormatBitsPerPixel(format);
				uint32_t srcBytesPerRow = (slice.width * bpp) / 8;
				// Xbox often aligns rows to X_D3DTEXTURE_PITCH_ALIGNMENT (64) in "Size" path,
				// here we put the tight row (will be copied into a dst aligned pitch later)
				slice.srcRowPitch = srcBytesPerRow;
				slice.rowCount = slice.height;
			}

			uint32_t mip2dSize = slice.srcRowPitch * slice.rowCount;
			uint32_t mipSlicePitch = mip2dSize * slice.depth;

			slice.srcOffset = curSrcOffset;

			// destination: align each slice placement to PLACEMENT_ALIGN
			uint32_t dstBpp = bConvertARGB ? 32 : XBFormatBitsPerPixel(format); // 32 bits if converting to ARGB
			uint32_t rawDstRowPitch = (slice.width * dstBpp) / 8;

			if (bCompressed)
			{
				uint32_t blockCols = (slice.width + 3) / 4;
				rawDstRowPitch = blockCols * blockSize;
			}

			// Unpacked, a slice is rows of RGBA pixels rather than rows of blocks.
			uint32_t dstRowCount = slice.rowCount;
			if (bUnpack)
			{
				rawDstRowPitch = slice.width * 4;
				dstRowCount = slice.height;
			}

			slice.dstRowPitch = (rawDstRowPitch + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
			slice.dstOffset = curDstOffset;

			uint32_t alignedSliceSize = (slice.dstRowPitch * dstRowCount * slice.depth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);

			curSrcOffset += mipSlicePitch;
			curDstOffset += alignedSliceSize;
		}
	}

	// allocate upload buffer with curDstOffset
	auto uploadBuffer = PlumeCreateBufferWrapper(curDstOffset);
	uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

	// For each slice: copy/unswizzle/convert into mapped + dstOffset

	XXH64_hash_t hash;


	for (auto& slice : slices)
	{
		unsigned long VirtualAddr = (unsigned long)GetDataFromXboxResource((X_D3DResource*)(&tex->guestResource));
		uint8_t* srcData = reinterpret_cast<uint8_t*>(base + VirtualAddr + slice.srcOffset);
		uint8_t* dstData = mappedMemory + slice.dstOffset;

		// shadow tex don't have mipmap so it should be safe to do this here
		hash = XXH3_64bits(srcData, slice.srcRowPitch * slice.rowCount * slice.depth);

		if (DiffPatchControllerGUITexture(tex, mappedMemory, curDstOffset, hash))
		{
			break;
		}

		if (bConvertARGB)
		{
			ConvertD3DTextureToARGBBuffer(format, (uint8_t*)srcData, slice.width, slice.height, slice.srcRowPitch, slice.srcRowPitch * slice.rowCount,
				dstData, slice.dstRowPitch, slice.dstRowPitch * slice.rowCount, slice.depth, (void*)g_currentPaletteData);
		}
		else if (isSwizzled)
		{
			uint32_t dstSlicePitch = slice.dstRowPitch * slice.rowCount * slice.depth;
			uint32_t srcBpp = bCompressed ? (blockSize) : (XBFormatBitsPerPixel(format) / 8);

			UnswizzleBox(
				(PVOID)srcData, slice.width, slice.height, slice.depth,
				srcBpp,
				dstData, slice.dstRowPitch, dstSlicePitch);
		}
#ifdef __ANDROID__
		else if (bUnpack)
		{
			for (uint32_t z = 0; z < slice.depth; z++)
			{
				BcDecode::Decode(unpackFrom, srcData + size_t(z) * slice.srcRowPitch * slice.rowCount, slice.srcRowPitch,
					slice.width, slice.height, dstData + size_t(z) * slice.dstRowPitch * slice.height, slice.dstRowPitch);
			}
		}
#endif
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

	//finish
	uploadBuffer->unmap();

	ExecuteCopyCommandList([&](RenderCommandList* commandList)
		{
			commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(tex->texture, RenderTextureLayout::COPY_DEST));

			auto copyTextureRegion = [&](Slice& slice, uint32_t subresourceIndex)
				{
					uint32_t mipIndex = subresourceIndex % dwMipMapLevels;
					uint32_t arrayIndex = subresourceIndex / dwMipMapLevels;
					// Plume wants the row width in pixels, a compressed block covers 4 of them.
					uint32_t uploadRowPitchInTexels = slice.dstRowPitch / RenderFormatSize(tex->format) * RenderFormatBlockWidth(tex->format);

					commandList->copyTextureRegion(
						RenderTextureCopyLocation::Subresource(tex->texture, mipIndex, arrayIndex),
						RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), tex->format, slice.width, slice.height, slice.depth, uploadRowPitchInTexels, slice.dstOffset));
				};

			for (size_t i = 0; i < slices.size(); i++)
				copyTextureRegion(slices[i], i);

			// Duplicate the first face across the remaining 6 faces. (todo check if this is accurate)
			if (bCubemap)
			{
				for (size_t i = 1; i < 6; i++)
				{
					for (size_t j = 0; j < slices.size(); j++)
						copyTextureRegion(slices[j], (slices.size() * i) + j);
				}
			}
		});
}

// rwD3D8NativeTextureRead
X86_FUNC_IMPL(__imp__sub_2ED0D0);
X86_OVERRIDE_FUNC(sub_2ED0D0)
{
	// call original function first
	__imp__sub_2ED0D0(ctx, base);

	if (ctx.eax.u32 == 1) //did the function return 1 aka was the texture created
	{
		auto textureAddr = X86_LOAD_U32(ctx.ecx.u32);
		RwTexture* rwTex = (RwTexture*)(base + textureAddr);
		auto rasterAddr = X86_LOAD_U32(rwTex->raster);
		RwRaster* raster = (RwRaster*)(base + X86_LOAD_U32(rasterAddr));

		//renderware doesn't store texture data in the actual raster, instead it stores it in the raster extension offset
		auto RasterExtOffsetAddr = (uint32_t)(X86_LOAD_U32(rasterAddr) + X86_LOAD_U32(RwD3D8RasterExtOffsetAddr));
		auto TexAddr = X86_LOAD_U32(RasterExtOffsetAddr);
		GuestTexture* tex = (GuestTexture*)(base + TexAddr);

		//when native texture are used, we have to compute them so the memory alignment can match what DX12 expect
		//we also need to convert palette to regular format as DX12 does not support that directly
		//finally we need to unswizzle texture so they can match what DX12 expect.
		CopyTextureToGPU(tex);
		foundTex = false;
	}
}

static void UnlockTextureRect(GuestTexture* texture)
{
	if (texture->isRenderTarget == true)
	{
		return;
	}

	RenderCommand cmd;
	cmd.type = RenderCommandType::UnlockTextureRect;
	cmd.unlockTextureRect.texture = texture;

	g_frameReadsGuestMemory = true;
	SendRenderCommand(cmd);
}

GuestTexture* g_currentTextureRect = nullptr;
// rwD3D8RasterUnlock
X86_FUNC_IMPL(__imp__sub_2DF870);
X86_OVERRIDE_FUNC(sub_2DF870)
{
	// Texture data aren't stored in the raster, instead they store the image in another raster using the extension offset to reach it
	auto rasterAddr = X86_LOAD_U32(ctx.esp.u32 + 8); // get raster param addr 
	auto RasterExtOffsetAddr = (uint32_t)(X86_LOAD_U32(rasterAddr) + X86_LOAD_U32(RwD3D8RasterExtOffsetAddr));
	auto TexAddr = X86_LOAD_U32(RasterExtOffsetAddr);
	g_currentTextureRect = (GuestTexture*)(base + TexAddr);
	__imp__sub_2DF870(ctx, base);
}

// XGSwizzleRect - This is the final step where texture are ready for Xbox to use, we can upload safely here. (Used during FMV.)
X86_FUNC_IMPL(__imp__sub_40B363);
X86_OVERRIDE_FUNC(sub_40B363)
{
	auto textureData = GetHostParameter(ctx, 4, CallConvention::Stdcall, ParameterType::Pointer);
	auto bytesPerPixel = GetHostParameter(ctx, 8, CallConvention::Stdcall, ParameterType::Integer);

	__imp__sub_40B363(ctx, base);

	if (g_currentTextureRect)
	{
		auto tex = g_currentTextureRect;

		uint32_t size = tex->width * tex->height * bytesPerPixel;

		if (size > tex->sizeOfTextureRect) // this should never pass, but just in case.
			size = tex->sizeOfTextureRect;

		memcpy(tex->mappedMemory, (void*)textureData, size);
		UnlockTextureRect(g_currentTextureRect);
	}
}


// Always use guest size, a smaller host size overflow the buffer and corrupt memory.
static uint32_t ComputeTexturePitch(GuestTexture* texture)
{
	return (texture->g_width * RenderFormatSize(texture->format) + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
}

static void LockTextureRect(GuestTexture* texture, int32_t level, GuestLockedRect* lockedRect, RECT* pRect, uint32_t flags)
{
	uint32_t pitch = ComputeTexturePitch(texture);
	uint32_t slicePitch = pitch * texture->g_height;

	if (texture->mappedMemory == nullptr)
	{
		texture->mappedMemory = g_userHeap.AllocPhysical(slicePitch, 0x10);
	}

	texture->sizeOfTextureRect = slicePitch;
	lockedRect->pitch = pitch;
	lockedRect->bits = g_memory.MapVirtual(texture->mappedMemory);
}

static GuestTexture* CreateTexture_(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, X_D3DFORMAT format, X_D3DRESOURCETYPE type)
{
	return CreateTexture(width, height, depth, levels, usage, format, type);
}


// CreateTexture
X86_FUNC_IMPL(__imp__sub_2DFE20);
X86_OVERRIDE_FUNC(sub_2DFE20)
{
	RwRaster* info = (RwRaster*)g_memory.Translate(ctx.ecx.u32);
	uint8_t level = X86_LOAD_U8(ctx.eax.u32 + 19);
	const uint32_t usage = (info->cType & 0xFF) == rwRASTERTYPECAMERATEXTURE ? 1 : 0;


	X_D3DFORMAT format = (X_D3DFORMAT)X86_LOAD_U32(ctx.esp.u32 + 4);
	__imp__sub_2DFE20(ctx, base);

	if (ctx.eax.u32 != 0)
	{
		//RwRaster* rwM = (RwRaster*)g_memory.Translate(second);
		XD3DResource* texture = (XD3DResource*)(g_memory.Translate(ctx.eax.u32));

		X_D3DRESOURCETYPE textureType = GetXboxTextureType((X_D3DResource*)texture);

		auto newTex = CreateTexture(info->width, info->height, 1, level, usage, format, textureType);

		newTex->guestResource.Common = texture->Common;
		newTex->guestResource.Data = texture->Data;
		newTex->guestResource.Format = texture->Format;
		newTex->guestResource.Lock = texture->Lock;
		newTex->guestResource.Size = texture->Size;

		g_userHeap.Free(g_memory.Translate(ctx.eax.u32)); // free the original texture memory as it's not needed anymore 
		ctx.eax.u32 = g_memory.MapVirtual(newTex);
	}
}


extern GuestTexture* g_texturesPSDef[4];
void SetTexture(uint32_t stage, GuestTexture* texture)
{
	if (texture && texture->patchedTexture != nullptr)
		texture = texture->patchedTexture.get();

	RenderCommand cmd;
	cmd.type = RenderCommandType::SetTexture;
	cmd.setTexture.index = stage;
	cmd.setTexture.texture = texture;
	cmd.setTexture.isFixed = g_Xbox_VertexShaderMode == VertexShaderMode::FixedFunction;
	g_texturesPSDef[stage] = texture;
	g_pendingRenderCommands.enqueue() = cmd;
}

GUEST_FUNCTION_HOOK_STDCALL(sub_3D0CD0, SetTexture);
GUEST_FUNCTION_HOOK_STDCALL(sub_3DD710, CreateTexture_); //not used for 2D stuff at least.
GUEST_FUNCTION_HOOK_STDCALL(sub_3DD810, LockTextureRect); //todo cube version?
