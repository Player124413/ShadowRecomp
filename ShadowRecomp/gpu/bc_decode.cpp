#include "bc_decode.h"

#include <algorithm>
#include <cstring>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

namespace BcDecode
{
    uint32_t BlockSize(EFormat format)
    {
        switch (format)
        {
        case EFormat::BC1:
        case EFormat::BC4:
            return 8;
        case EFormat::BC2:
        case EFormat::BC3:
        case EFormat::BC5:
        case EFormat::BC7:
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
        case EFormat::BC2:
        case EFormat::BC3:
        case EFormat::BC7:
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

                switch (format)
                {
                case EFormat::BC1:
                    bcdec_bc1(packed, block, blockPitch);
                    break;
                case EFormat::BC2:
                    bcdec_bc2(packed, block, blockPitch);
                    break;
                case EFormat::BC3:
                    bcdec_bc3(packed, block, blockPitch);
                    break;
                case EFormat::BC4:
                    bcdec_bc4(packed, block, blockPitch);
                    break;
                case EFormat::BC5:
                    bcdec_bc5(packed, block, blockPitch);
                    break;
                case EFormat::BC7:
                    bcdec_bc7(packed, block, blockPitch);
                    break;
                default:
                    return;
                }

                const size_t columnBytes = size_t(std::min(4u, width - x)) * pixelSize;
                for (uint32_t row = 0; row < rows; row++)
                    memcpy(destination + size_t(y + row) * destinationPitch + size_t(x) * pixelSize, block + size_t(row) * blockPitch, columnBytes);
            }
        }
    }
}
