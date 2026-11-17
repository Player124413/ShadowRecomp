#include "bc_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

namespace BcDecode
{
    // The sRGB transfer curve, as a table over the 256 values a byte holds. Turning every
    // colour channel of every texel through it by hand is what a GPU does on its own when
    // it samples an sRGB texture; the curve is not linear, so it cannot be folded into the
    // block decoding. Made once, on first use.
    static const uint8_t* SrgbToLinearTable()
    {
        static const uint8_t* table = []()
        {
            uint8_t* made = new uint8_t[256];
            for (uint32_t i = 0; i < 256; i++)
            {
                const double srgb = double(i) / 255.0;
                const double linear = srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
                made[i] = uint8_t(std::lround(linear * 255.0));
            }

            return made;
        }();

        return table;
    }

    // `pixels` is `count` pixels of 4 bytes (R, G, B, A). The transparency is not colour
    // and keeps its value.
    static void LinearizeSrgb(uint8_t* pixels, uint32_t count)
    {
        const uint8_t* table = SrgbToLinearTable();
        for (uint32_t i = 0; i < count; i++, pixels += 4)
        {
            pixels[0] = table[pixels[0]];
            pixels[1] = table[pixels[1]];
            pixels[2] = table[pixels[2]];
        }
    }

    uint32_t BlockSize(EFormat format)
    {
        switch (format)
        {
        case EFormat::BC1:
        case EFormat::BC1_SRGB:
        case EFormat::BC4:
            return 8;
        case EFormat::BC2:
        case EFormat::BC2_SRGB:
        case EFormat::BC3:
        case EFormat::BC3_SRGB:
        case EFormat::BC5:
        case EFormat::BC7:
        case EFormat::BC7_SRGB:
            return 16;
        default:
            return 0;
        }
    }

    uint32_t PixelSize(EFormat format)
    {
        switch (format)
        {
        case EFormat::BC1:
        case EFormat::BC1_SRGB:
        case EFormat::BC2:
        case EFormat::BC2_SRGB:
        case EFormat::BC3:
        case EFormat::BC3_SRGB:
        case EFormat::BC7:
        case EFormat::BC7_SRGB:
            return 4;
        case EFormat::BC4:
            return 1;
        case EFormat::BC5:
            return 2;
        default:
            return 0;
        }
    }

    void Decode(EFormat format, const uint8_t* source, size_t sourcePitch, uint32_t width, uint32_t height,
        uint8_t* destination, size_t destinationPitch)
    {
        const uint32_t blockSize = BlockSize(format);
        const uint32_t pixelSize = PixelSize(format);
        if (blockSize == 0 || source == nullptr || destination == nullptr)
            return;

        // A block is unpacked here first: at the edges of an image whose size is not a
        // multiple of four only part of it belongs to the image.
        uint8_t block[4 * 4 * 4];
        const int blockPitch = int(4 * pixelSize);

        // And copied here before that: bcdec reads the block as 16-bit and wider numbers,
        // and the data of a texture may start at any address.
        alignas(8) uint8_t packed[16];

        for (uint32_t y = 0; y < height; y += 4)
        {
            const uint8_t* sourceRow = source + size_t(y / 4) * sourcePitch;
            const uint32_t rows = std::min(4u, height - y);

            for (uint32_t x = 0; x < width; x += 4)
            {
                memcpy(packed, sourceRow + size_t(x / 4) * blockSize, blockSize);

                bool linearizeSrgb = false;
                switch (format)
                {
                case EFormat::BC1:
                case EFormat::BC1_SRGB:
                    bcdec_bc1(packed, block, blockPitch);
                    linearizeSrgb = format == EFormat::BC1_SRGB;
                    break;
                case EFormat::BC2:
                case EFormat::BC2_SRGB:
                    bcdec_bc2(packed, block, blockPitch);
                    linearizeSrgb = format == EFormat::BC2_SRGB;
                    break;
                case EFormat::BC3:
                case EFormat::BC3_SRGB:
                    bcdec_bc3(packed, block, blockPitch);
                    linearizeSrgb = format == EFormat::BC3_SRGB;
                    break;
                case EFormat::BC4:
                    bcdec_bc4(packed, block, blockPitch);
                    break;
                case EFormat::BC5:
                    bcdec_bc5(packed, block, blockPitch);
                    break;
                case EFormat::BC7:
                case EFormat::BC7_SRGB:
                    bcdec_bc7(packed, block, blockPitch);
                    linearizeSrgb = format == EFormat::BC7_SRGB;
                    break;
                default:
                    return;
                }

                // Done before the edge is cut off, so only one place knows the curve.
                if (linearizeSrgb)
                    LinearizeSrgb(block, 16);

                const size_t columnBytes = size_t(std::min(4u, width - x)) * pixelSize;
                for (uint32_t row = 0; row < rows; row++)
                    memcpy(destination + size_t(y + row) * destinationPitch + size_t(x) * pixelSize, block + size_t(row) * blockPitch, columnBytes);
            }
        }
    }
}
