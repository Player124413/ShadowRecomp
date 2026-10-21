#pragma once
// ETC2 block encoder.
//
// Why this exists: the GameCube's textures are small (CMPR = BC1, RGB565, paletted) but a phone's GPU almost never
// supports BC1, so Aurora decodes every texture to RGBA8 - 32 bits per pixel, eight times the console's CMPR traffic.
// ETC2 is mandatory in GLES3/Vulkan-class hardware, i.e. on every 64-bit phone, and gives the same 4 bits per pixel
// back (8 with the alpha channel), so the texture memory and the bandwidth a frame needs drop four- to eightfold.
//
// This is a small, dependency-free encoder: ETC1-style differential/individual colour with all eight modifier tables
// (the 8-bit "ETC2 RGB" format) and the EAC alpha channel for "ETC2 RGBA". The T/H/planar modes are not used: the
// sources here are already block-compressed or plain colours, and the simple modes keep the encoder fast enough to run
// while a texture loads.
//
// Layout of a block (the format's own, as the GPU reads it):
//   colour block, 8 bytes: bits 0..23 base colours, 24 flip, 25 differential, 26..28 and 29..31 the two modifier
//   tables; bits 32..47 and 48..63 the two bit-planes of the per-pixel selectors.
//   alpha block, 8 bytes: base alpha, then multiplier and table, then 16 three-bit indices.
#include <cstddef>
#include <cstdint>

namespace aurora::gfx::etc2 {

// One 4x4 block of tightly packed RGBA8 pixels (stride = bytes per row of the source, normally 16).
// Every pixel is treated as opaque.
void encode_rgb(const uint8_t* rgba, size_t stride, uint8_t out[8]);

// 4x4 block -> 8 bytes of EAC alpha, then 8 bytes of colour (the "ETC2 RGBA" format).
void encode_rgba(const uint8_t* rgba, size_t stride, uint8_t out[16]);

// True when every alpha in the block is 255 (then the alpha channel can be left out entirely).
bool block_is_opaque(const uint8_t* rgba, size_t stride);

// A whole mip chain at once (tightly packed, mip 0 first): the size the result needs, and the encoding itself.
// `out` must hold image_size() bytes - 8 per block for ETC2 RGB, 16 per block (EAC alpha + colour) for ETC2 RGBA.
size_t image_size(uint32_t width, uint32_t height, uint32_t mips);
void encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t mips, uint8_t* out);
size_t image_size_rgba(uint32_t width, uint32_t height, uint32_t mips);
void encode_image_rgba(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t mips, uint8_t* out);

}  // namespace aurora::gfx::etc2
