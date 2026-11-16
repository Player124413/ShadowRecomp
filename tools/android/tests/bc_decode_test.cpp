// Host test of gpu/bc_decode.cpp: block-compressed textures unpacked for GPUs that cannot
// sample them. The blocks are put together here by the rules of the formats, so the test
// knows every pixel that has to come out; what it is after is that they come out in the
// right place, for images of any size and rows of any length.
#include <gpu/bc_decode.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace BcDecode;

static int g_checks = 0;
static int g_failures = 0;

static void Check(bool ok, const char* what, const std::string& got = {})
{
    g_checks++;
    if (!ok)
    {
        g_failures++;
        printf("FAIL %s%s%s\n", what, got.empty() ? "" : ": ", got.c_str());
    }
}

struct Rgba
{
    uint8_t r, g, b, a;
    bool operator==(const Rgba&) const = default;
};

static std::string Text(Rgba c)
{
    char text[40];
    snprintf(text, sizeof(text), "(%u, %u, %u, %u)", c.r, c.g, c.b, c.a);
    return text;
}

// The colour part of a block, as BC1, BC2 and BC3 have it: two colours of 5, 6 and 5 bits
// and sixteen 2-bit numbers saying which of the four colours made of them each pixel has.
static void ColourBlock(uint8_t* block, uint16_t colour0, uint16_t colour1, const uint8_t indices[16])
{
    block[0] = uint8_t(colour0);
    block[1] = uint8_t(colour0 >> 8);
    block[2] = uint8_t(colour1);
    block[3] = uint8_t(colour1 >> 8);

    uint32_t bits = 0;
    for (int i = 0; i < 16; i++)
        bits |= uint32_t(indices[i] & 3) << (2 * i);

    memcpy(block + 4, &bits, 4);
}

static constexpr uint16_t White = 0xFFFF;
static constexpr uint16_t Black = 0x0000;
static constexpr uint16_t Red = 0xF800;
static constexpr uint16_t Blue = 0x001F;

// Sixteen pixels that are 0, 1, 2, 3 from left to right in every row.
static const uint8_t AcrossIndices[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };

// Sixteen pixels that are 0, 1, 2, 3 from top to bottom in every column.
static const uint8_t DownIndices[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };

static Rgba PixelAt(const std::vector<uint8_t>& image, size_t pitch, uint32_t x, uint32_t y)
{
    const uint8_t* p = image.data() + y * pitch + x * 4;
    return { p[0], p[1], p[2], p[3] };
}

int main()
{
    Check(BlockSize(EFormat::BC1) == 8 && BlockSize(EFormat::BC2) == 16 && BlockSize(EFormat::BC3) == 16 &&
        BlockSize(EFormat::BC4) == 8 && BlockSize(EFormat::BC5) == 16 && BlockSize(EFormat::BC7) == 16 && BlockSize(EFormat::None) == 0,
        "blocks have the sizes of their formats");
    Check(PixelSize(EFormat::BC1) == 4 && PixelSize(EFormat::BC2) == 4 && PixelSize(EFormat::BC3) == 4 && PixelSize(EFormat::BC7) == 4 &&
        PixelSize(EFormat::BC4) == 1 && PixelSize(EFormat::BC5) == 2 && PixelSize(EFormat::None) == 0,
        "and unpack to pixels of the sizes of theirs");

    // --- BC1: four colours ------------------------------------------------------------------
    // White before black: the two in between are two thirds and one third white.
    {
        uint8_t block[8];
        ColourBlock(block, White, Black, AcrossIndices);

        std::vector<uint8_t> image(4 * 4 * 4, 0x55);
        Decode(EFormat::BC1, block, 8, 4, 4, image.data(), 16);

        const Rgba expected[4] = { { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, { 170, 170, 170, 255 }, { 85, 85, 85, 255 } };
        bool ok = true;
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                ok &= PixelAt(image, 16, x, y) == expected[x];

        Check(ok, "BC1 with four colours: every pixel has the colour its number picks", Text(PixelAt(image, 16, 2, 0)));
    }

    // Rows are rows: the same numbers down the columns.
    {
        uint8_t block[8];
        ColourBlock(block, Red, Blue, DownIndices);

        std::vector<uint8_t> image(4 * 4 * 4);
        Decode(EFormat::BC1, block, 8, 4, 4, image.data(), 16);

        Check(PixelAt(image, 16, 0, 0) == Rgba{ 255, 0, 0, 255 } && PixelAt(image, 16, 3, 0) == Rgba{ 255, 0, 0, 255 } &&
            PixelAt(image, 16, 0, 1) == Rgba{ 0, 0, 255, 255 } && PixelAt(image, 16, 3, 1) == Rgba{ 0, 0, 255, 255 },
            "BC1: the first numbers are the top row, red, green and blue in that order", Text(PixelAt(image, 16, 0, 0)) + " " + Text(PixelAt(image, 16, 0, 1)));
    }

    // --- BC1: three colours and a transparent one ---------------------------------------------
    {
        uint8_t block[8];
        ColourBlock(block, Black, White, AcrossIndices); // Black is not above white: the other mode.

        std::vector<uint8_t> image(4 * 4 * 4);
        Decode(EFormat::BC1, block, 8, 4, 4, image.data(), 16);

        Check(PixelAt(image, 16, 0, 0) == Rgba{ 0, 0, 0, 255 } && PixelAt(image, 16, 1, 0) == Rgba{ 255, 255, 255, 255 },
            "BC1 with transparency: the two colours stay opaque");
        const Rgba middle = PixelAt(image, 16, 2, 0);
        Check(middle.r >= 127 && middle.r <= 128 && middle.r == middle.g && middle.g == middle.b && middle.a == 255,
            "the third is halfway between them", Text(middle));
        Check(PixelAt(image, 16, 3, 0) == Rgba{ 0, 0, 0, 0 }, "and the fourth is transparent", Text(PixelAt(image, 16, 3, 0)));
    }

    // --- BC2: transparency of four bits per pixel ---------------------------------------------
    {
        uint8_t block[16];
        for (int i = 0; i < 8; i++)
            block[i] = uint8_t(0x10 * ((2 * i + 1) & 15) | ((2 * i) & 15)); // Pixel n has transparency n.

        // Black before white here would be the transparent mode of BC1; BC2 has no such mode.
        ColourBlock(block + 8, Black, White, AcrossIndices);

        std::vector<uint8_t> image(4 * 4 * 4);
        Decode(EFormat::BC2, block, 16, 4, 4, image.data(), 16);

        bool alphaOk = true;
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                alphaOk &= PixelAt(image, 16, x, y).a == (y * 4 + x) * 17;

        Check(alphaOk, "BC2: each pixel has its own transparency, four bits spread over eight", Text(PixelAt(image, 16, 1, 0)));
        Check(PixelAt(image, 16, 3, 0).r == 170 && PixelAt(image, 16, 2, 0).r == 85,
            "BC2 always has four colours, whichever of the two comes first", Text(PixelAt(image, 16, 3, 0)));
    }

    // --- BC3: transparency interpolated between two values -------------------------------------
    {
        uint8_t block[16] = {};
        block[0] = 255; // Above the second value: six steps in between.
        block[1] = 0;

        // Sixteen numbers of three bits: 0 (the first value), 1 (the second), 2 (the first step from the first) in turn.
        uint64_t bits = 0;
        for (int i = 0; i < 16; i++)
            bits |= uint64_t(i % 3) << (3 * i);
        memcpy(block + 2, &bits, 6);

        ColourBlock(block + 8, Red, Blue, DownIndices);

        std::vector<uint8_t> image(4 * 4 * 4);
        Decode(EFormat::BC3, block, 16, 4, 4, image.data(), 16);

        const Rgba first = PixelAt(image, 16, 0, 0), second = PixelAt(image, 16, 1, 0), third = PixelAt(image, 16, 2, 0);
        Check(first.a == 255 && second.a == 0 && third.a >= 218 && third.a <= 219,
            "BC3: transparency is one of two values or a step between them", Text(first) + " " + Text(second) + " " + Text(third));
        Check(first.r == 255 && first.b == 0 && PixelAt(image, 16, 0, 1).b == 255, "with the colours of BC1 next to it");
    }

    // --- BC4 and BC5: one and two channels ------------------------------------------------------
    {
        uint8_t block[16] = {};
        block[0] = 200; // All pixels have number 0: the first value.
        block[1] = 10;
        block[8] = 30;
        block[9] = 240;

        std::vector<uint8_t> one(4 * 4, 0x55);
        Decode(EFormat::BC4, block, 8, 4, 4, one.data(), 4);
        bool ok = true;
        for (uint8_t value : one)
            ok &= value == 200;
        Check(ok, "BC4 unpacks to one byte per pixel", std::to_string(one[0]));

        std::vector<uint8_t> two(4 * 4 * 2, 0x55);
        Decode(EFormat::BC5, block, 16, 4, 4, two.data(), 8);
        ok = true;
        for (size_t i = 0; i < two.size(); i += 2)
            ok &= two[i] == 200 && two[i + 1] == 30;
        Check(ok, "BC5 to two", std::to_string(two[0]) + " " + std::to_string(two[1]));
    }

    // --- Where the pixels go ----------------------------------------------------------------------
    // An image that is no multiple of four in either direction, with rows of blocks and
    // rows of pixels both longer than they need to be. Every block has one colour, so the
    // image shows which block each pixel was taken from; around it nothing may change.
    {
        constexpr uint32_t Width = 10, Height = 7;
        constexpr uint32_t BlocksWide = 3, BlocksHigh = 2;
        constexpr size_t SourcePitch = BlocksWide * 8 + 5;
        constexpr size_t DestinationPitch = Width * 4 + 9;
        constexpr size_t Before = 64, After = 64;

        // Colours with all bits of a channel set or clear unpack to exactly 0 or 255.
        const uint16_t colours[BlocksHigh][BlocksWide] = { { 0xF800, 0x07E0, 0x001F }, { 0xFFE0, 0xF81F, 0x07FF } };
        const Rgba expected[BlocksHigh][BlocksWide] = {
            { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } },
            { { 255, 255, 0, 255 }, { 255, 0, 255, 255 }, { 0, 255, 255, 255 } } };

        std::vector<uint8_t> source(SourcePitch * BlocksHigh, 0xEE);
        const uint8_t allFirst[16] = {};
        for (uint32_t by = 0; by < BlocksHigh; by++)
            for (uint32_t bx = 0; bx < BlocksWide; bx++)
                ColourBlock(source.data() + by * SourcePitch + bx * 8, colours[by][bx], colours[by][bx], allFirst);

        std::vector<uint8_t> buffer(Before + DestinationPitch * Height + After, 0xA5);
        Decode(EFormat::BC1, source.data(), SourcePitch, Width, Height, buffer.data() + Before, DestinationPitch);

        bool pixelsOk = true, paddingOk = true;
        std::string firstWrong;
        for (uint32_t y = 0; y < Height; y++)
        {
            const uint8_t* row = buffer.data() + Before + y * DestinationPitch;
            for (uint32_t x = 0; x < Width; x++)
            {
                const Rgba got = { row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3] };
                if (!(got == expected[y / 4][x / 4]) && firstWrong.empty())
                    firstWrong = "pixel " + std::to_string(x) + "," + std::to_string(y) + " is " + Text(got);
                pixelsOk &= got == expected[y / 4][x / 4];
            }

            // Between the end of this row's pixels and the start of the next row.
            const size_t rowEnd = y + 1 < Height ? DestinationPitch : Width * 4;
            for (size_t i = Width * 4; i < rowEnd; i++)
                paddingOk &= row[i] == 0xA5;
        }

        Check(pixelsOk, "an image of 10 x 7 pixels: every pixel comes from the block it lies in", firstWrong);
        Check(paddingOk, "nothing is written between its rows");

        bool aroundOk = true;
        for (size_t i = 0; i < Before; i++)
            aroundOk &= buffer[i] == 0xA5;
        for (size_t i = Before + DestinationPitch * (Height - 1) + Width * 4; i < buffer.size(); i++)
            aroundOk &= buffer[i] == 0xA5;
        Check(aroundOk, "nor before its first pixel or after its last");
    }

    // The same with blocks of sixteen bytes: the second block of a row is found.
    for (EFormat format : { EFormat::BC2, EFormat::BC3 })
    {
        uint8_t blocks[32];
        memset(blocks, 0xFF, sizeof(blocks)); // Opaque, in both formats.
        const uint8_t allFirst[16] = {};
        ColourBlock(blocks + 8, Red, Red, allFirst);
        ColourBlock(blocks + 24, Blue, Blue, allFirst);

        std::vector<uint8_t> image(8 * 4 * 4);
        Decode(format, blocks, 32, 8, 4, image.data(), 32);

        Check(PixelAt(image, 32, 3, 3) == Rgba{ 255, 0, 0, 255 } && PixelAt(image, 32, 4, 0) == Rgba{ 0, 0, 255, 255 } &&
            PixelAt(image, 32, 7, 3) == Rgba{ 0, 0, 255, 255 }, "two blocks of sixteen bytes side by side", Text(PixelAt(image, 32, 4, 0)));
    }

    // The smallest levels of a texture are smaller than one block.
    for (uint32_t size = 1; size <= 3; size++)
    {
        uint8_t block[8];
        ColourBlock(block, Red, Blue, AcrossIndices);

        std::vector<uint8_t> buffer(8 + size * size * 4 + 8, 0xA5);
        Decode(EFormat::BC1, block, 8, size, size, buffer.data() + 8, size * 4);

        bool ok = buffer[8] == 255 && buffer[9] == 0 && buffer[10] == 0 && buffer[11] == 255;
        if (size > 1)
            ok &= buffer[8 + 4 + 2] == 255 && buffer[8 + 4] == 0; // The second pixel is the second colour.
        for (size_t i = 0; i < 8; i++)
            ok &= buffer[i] == 0xA5 && buffer[buffer.size() - 1 - i] == 0xA5;

        Check(ok, "an image smaller than a block takes its corner of it and no more", std::to_string(size));
    }

    // Every format stays inside the image, whatever is in the blocks.
    for (EFormat format : { EFormat::BC1, EFormat::BC2, EFormat::BC3, EFormat::BC4, EFormat::BC5, EFormat::BC7 })
    {
        constexpr uint32_t Width = 9, Height = 6;
        const uint32_t pixelSize = PixelSize(format);
        const size_t sourcePitch = 3 * BlockSize(format);
        const size_t destinationPitch = Width * pixelSize + 3;

        std::vector<uint8_t> source(sourcePitch * 2);
        uint32_t noise = 0x1234567;
        for (uint8_t& byte : source)
        {
            noise = noise * 1664525u + 1013904223u;
            byte = uint8_t(noise >> 24);
        }

        std::vector<uint8_t> buffer(16 + destinationPitch * Height + 16, 0xA5);
        Decode(format, source.data(), sourcePitch, Width, Height, buffer.data() + 16, destinationPitch);

        bool ok = true;
        for (size_t i = 0; i < 16; i++)
            ok &= buffer[i] == 0xA5;
        for (uint32_t y = 0; y + 1 < Height; y++)
            for (size_t i = Width * pixelSize; i < destinationPitch; i++)
                ok &= buffer[16 + y * destinationPitch + i] == 0xA5;
        for (size_t i = 16 + destinationPitch * (Height - 1) + Width * pixelSize; i < buffer.size(); i++)
            ok &= buffer[i] == 0xA5;

        Check(ok, "blocks of any content stay inside the image", std::to_string(int(format)));
    }

    // Nothing to do, nothing done.
    {
        std::vector<uint8_t> buffer(64, 0xA5);
        const uint8_t block[16] = {};
        Decode(EFormat::None, block, 16, 4, 4, buffer.data(), 16);
        Decode(EFormat::BC1, block, 8, 0, 4, buffer.data(), 16);
        Decode(EFormat::BC1, block, 8, 4, 0, buffer.data(), 16);
        Decode(EFormat::BC1, nullptr, 8, 4, 4, buffer.data(), 16);
        Decode(EFormat::BC1, block, 8, 4, 4, nullptr, 16);

        bool ok = true;
        for (uint8_t byte : buffer)
            ok &= byte == 0xA5;
        Check(ok, "no format, no size or no memory: nothing is written");
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
