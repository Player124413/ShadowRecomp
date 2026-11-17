#pragma once

#include <cstddef>
#include <cstdint>

// Unpacks block-compressed textures (BC, known to the Xbox as DXT) into plain pixels.
//
// Desktop GPUs sample these formats as they are. Most phone GPUs cannot: Mali, PowerVR and
// Xclipse know ETC2 and ASTC instead, and a texture created in a format the GPU does not
// have shows nothing. On such a GPU the textures are unpacked when they are loaded, at
// four to eight times the memory. The game's own textures are not compressed this way;
// textures of mods often are.
//
// The unpacking itself is bcdec (thirdparty/bcdec). Nothing here is specific to a platform
// or to a graphics API; the tests run it on the build machine (tools/android/tests).
namespace BcDecode
{
    enum class EFormat
    {
        None, // Not a format that is unpacked.
        BC1,  // DXT1: colour, optionally with one bit of transparency. Unpacks to RGBA.
        BC2,  // DXT3: colour and four bits of transparency. Unpacks to RGBA.
        BC3,  // DXT5: colour and interpolated transparency. Unpacks to RGBA.
        BC4,  // One channel. Unpacks to one byte per pixel.
        BC5,  // Two channels. Unpacks to two bytes per pixel.
        BC7,  // Colour and transparency in high quality. Unpacks to RGBA.

        // The sRGB forms of the colour formats. The blocks unpack the same way; the
        // colours are then turned from sRGB into linear, because the plain textures they
        // are unpacked into are not sampled through the sRGB curve the way a GPU samples
        // a block-compressed sRGB texture. Without that the picture comes out too bright.
        // (There is no sRGB form of BC4 or BC5.) Transparency is not colour and is left
        // alone.
        BC1_SRGB,
        BC2_SRGB,
        BC3_SRGB,
        BC7_SRGB
    };

    // The size in bytes of one block of 4 x 4 pixels, and of one pixel once unpacked.
    // Both are 0 for EFormat::None.
    uint32_t BlockSize(EFormat format);
    uint32_t PixelSize(EFormat format);

    // Unpacks an image of `width` x `height` pixels.
    //
    // `source` holds the blocks row by row: (width + 3) / 4 blocks in each of
    // (height + 3) / 4 rows, a row starting `sourcePitch` bytes after the one before it.
    // `destination` receives `height` rows of `width` pixels, a row starting
    // `destinationPitch` bytes after the one before it. Blocks that reach beyond the image
    // at its right and lower edge are cut off: not a byte is written outside those rows
    // and pixels.
    void Decode(EFormat format, const uint8_t* source, size_t sourcePitch, uint32_t width, uint32_t height,
        uint8_t* destination, size_t destinationPitch);
}
