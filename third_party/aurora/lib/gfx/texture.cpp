#include "resources.hpp"
#include "recording.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "aurora/aurora.h"
#include "texture.hpp"
#include "texture_convert.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <fmt/format.h>
#include <tracy/Tracy.hpp>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};

wgpu::Extent3D physical_size(wgpu::Extent3D size, TextureFormatInfo info) {
  const uint32_t width = ((size.width + info.blockWidth - 1) / info.blockWidth) * info.blockWidth;
  const uint32_t height = ((size.height + info.blockHeight - 1) / info.blockHeight) * info.blockHeight;
  return {.width = width, .height = height, .depthOrArrayLayers = size.depthOrArrayLayers};
}

bool setup_swizzle(wgpu::TextureComponentSwizzleDescriptor& swizzle, u32 format) noexcept {
  if (!webgpu::g_textureComponentSwizzleSupported) {
    return false;
  }

  switch (format) {
  case GX_TF_R8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::R;
    return true;
  case GX_TF_RG8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::G;
    return true;
  default:
    return false;
  }
}

wgpu::AddressMode wgpu_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return wgpu::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  }
}

std::pair<wgpu::FilterMode, wgpu::MipmapFilterMode> wgpu_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Undefined};
  case GX_LINEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Undefined};
  case GX_NEAR_MIP_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  }
}

u16 wgpu_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
  case GX_MAX_ANISOTROPY:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}
} // namespace

namespace {
// The one place a sampled texture is created: format first, everything else follows from it.
TextureHandle new_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                             wgpu::TextureFormat wgpuFormat, const char* label) noexcept {
  ZoneScopedS(3);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = mips,
  };
  wgpu::TextureComponentSwizzleDescriptor swizzle;
  if (setup_swizzle(swizzle, gxFormat)) {
    textureViewDescriptor.nextInChain = &swizzle;
  }
  auto textureView = texture.CreateView(&textureViewDescriptor);
  return std::make_shared<TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size, wgpuFormat,
                                      mips, gxFormat);
}
} // namespace

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,
                                    bool tlut, const char* label) noexcept {
  ZoneScoped;

  // Convert first, create the texture second. The conversion can change the *GPU* format (ETC2 replaces RGBA8 on a
  // phone), and a texture has to be created with the format its bytes are in: uploading 4-bit-per-pixel ETC2 blocks into
  // an RGBA8 texture is what made every CMPR texture on Android - that is, most of the world - upload as garbage and
  // show up black, while the UI (paletted / RGB5A3 art, which is not transcoded) looked fine.
  ConvertedTexture converted;
  if (format != InvalidTextureFormat) {
    if (tlut) {
      CHECK(height == 1, "new_static_texture_2d[{}]: expected tlut height 1, got {}", label, height);
      CHECK(mips == 1, "new_static_texture_2d[{}]: expected tlut mipCount 1, got {}", label, mips);
      converted = convert_tlut(format, width, data);
    } else {
      converted = convert_texture(format, width, height, mips, data);
    }
  }

  // Empty converted data means "upload the caller's bytes as they are": then, and only then, the formula format is the
  // one to use.
  const wgpu::TextureFormat uploadFormat = converted.data.empty() ? to_wgpu(format) : converted.format;
  auto handle = new_texture_2d(width, height, mips, format, uploadFormat, label);
  auto& ref = *handle;
  if (!converted.data.empty()) {
    data = converted.data;
    ref.hasArbitraryMips = converted.hasArbitraryMips;
    if (ref.format != uploadFormat) {
      Log.warn("new_static_texture_2d[{}]: upload format {} does not match the texture's {}", label,
               static_cast<uint32_t>(uploadFormat), static_cast<uint32_t>(ref.format));
    }
  }

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "new_static_texture_2d[{}]: expected at least {} bytes, got {}", label,
          offset + dataSize, data.size());
    if (offset + dataSize > data.size()) {
      // A size mismatch is a bug above, never a reason to read past the buffer (release builds do not check): stop and
      // keep the levels that were uploaded.
      Log.error("new_static_texture_2d[{}]: level {} needs {} bytes, only {} left - upload truncated", label, mip,
                dataSize, data.size() - offset);
      break;
    }
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, bytesPerRow, heightBlocks, std::move(dstView), physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("new_static_texture_2d[{}]: texture used {} bytes, but given {} bytes", label, offset, data.size());
  }
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  return new_texture_2d(width, height, mips, gxFormat, to_wgpu(gxFormat), label);
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label,
                                 bool mipped) noexcept {
  ZoneScoped;

  const auto wgpuFormat = webgpu::g_graphicsConfig.colorFormat;
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  uint32_t mips = 1;
  if (mipped) {
    for (uint32_t d = std::max(width, height); d > 1 && mips < 12; d >>= 1) ++mips;
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments (level 0: what copies write)
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
      .baseMipLevel = 0,
      .mipLevelCount = 1,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  std::vector<wgpu::TextureView> mipViews;
  if (mips > 1) {
    const wgpu::TextureViewDescriptor all{
        .label = viewLabel.c_str(),
        .format = wgpuFormat,
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseMipLevel = 0,
        .mipLevelCount = mips,
    };
    sampleTextureView = texture.CreateView(&all);
    for (uint32_t i = 0; i < mips; ++i) {
      const wgpu::TextureViewDescriptor one{
          .label = viewLabel.c_str(),
          .format = wgpuFormat,
          .dimension = wgpu::TextureViewDimension::e2D,
          .baseMipLevel = i,
          .mipLevelCount = 1,
      };
      mipViews.push_back(texture.CreateView(&one));
    }
  }
  auto ref = std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                          std::move(attachmentTextureView), size, wgpuFormat, mips, gxFormat);
  ref->isCopyMipped = mips > 1;
  ref->mipViews = std::move(mipViews);
  return ref;
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto wgpuFormat = to_wgpu(gxFormat);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                      std::move(attachmentTextureView), size, wgpuFormat, 1, gxFormat);
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    // The texture already exists with a fixed format, so the bytes written into it have to be in *that* format. A
    // conversion that lands somewhere else (the ETC2 encoder deciding this texture is a per-frame upload, so it stayed
    // RGBA8) leaves two cases: the caller's own bytes already are what the texture holds, or the write cannot be done
    // at all. Writing CMPR blocks into a decoded RGBA8 texture - or ETC2 into either - is what turns the world black,
    // and a release build cannot recover from it (CHECK() is compiled out), so refuse instead.
    if (!converted.data.empty() && converted.format == ref.format) {
      data = converted.data;
      ref.hasArbitraryMips = converted.hasArbitraryMips;
    } else if (uses_direct_texture_upload(ref.gxFormat) && to_wgpu(ref.gxFormat) == ref.format) {
      // nothing to convert: the caller's bytes are already in the texture's format
    } else {
      static bool reported = false;
      if (!reported) {
        reported = true;
        Log.error("write_texture: cannot write a {}x{} texture as format {} (GX format {}); skipping the upload",
                  ref.size.width, ref.size.height, static_cast<uint32_t>(ref.format),
                  static_cast<uint32_t>(ref.gxFormat));
      }
      return;
    }
  }

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "write_texture: expected at least {} bytes, got {}", offset + dataSize,
          data.size());
    if (offset + dataSize > data.size()) {
      Log.error("write_texture: level {} needs {} bytes, only {} left - upload truncated", mip, dataSize,
                data.size() - offset);
      break;
    }
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, bytesPerRow, heightBlocks, std::move(dstView), physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("write_texture: texture used {} bytes, but given {} bytes", offset, data.size());
  }
}

wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept {
  auto [minFilter, mipFilter] = wgpu_filter_mode(texObj.min_filter());
  auto [magFilter, _] = wgpu_filter_mode(texObj.mag_filter());
  const bool mipsEnabled = mipFilter != wgpu::MipmapFilterMode::Undefined;
  float minLod = texObj.min_lod();
  float maxLod = texObj.max_lod();
  u16 maxAnisotropy = wgpu_aniso(texObj.max_aniso());
  if (ref && ref->isCopyMipped) {
    minLod = 0.f;
    maxLod = static_cast<float>(ref->mipCount);
    mipFilter = wgpu::MipmapFilterMode::Linear;
  } else if (ref && ref->isReplacement) {
    minLod = 0.f;
    maxLod = 1000.f;
    if (!mipsEnabled) {
      mipFilter = wgpu::MipmapFilterMode::Nearest;
    }
  } else if (mipFilter == wgpu::MipmapFilterMode::Undefined) {
    minLod = 0.f;
    maxLod = 0.f;
  }
  if ((ref && ref->hasArbitraryMips) || !mipsEnabled || (ref && ref->isCopyMipped)) {
    maxAnisotropy = 1;
  } else if (maxAnisotropy > 1) {
    magFilter = wgpu::FilterMode::Linear;
    minFilter = wgpu::FilterMode::Linear;
    mipFilter = wgpu::MipmapFilterMode::Linear;
  }
  return {
      .label = "Generated Filtering Sampler",
      .addressModeU = wgpu_address_mode(texObj.wrap_s()),
      .addressModeV = wgpu_address_mode(texObj.wrap_t()),
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .lodMinClamp = minLod,
      .lodMaxClamp = maxLod,
      .maxAnisotropy = maxAnisotropy,
  };
}
} // namespace aurora::gfx
