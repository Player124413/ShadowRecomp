// Writes out the SPIR-V of every shader in the game's shader cache
// (ShadowRecompLib/shader/shader_cache.cpp), unpacked the way gpu/video.cpp unpacks it,
// for spirv_lowering_test.py.
//
//   shader_cache_dump <directory>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <shader/shader_cache.h>

#include <smolv.h>
#include <zstd.h>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: shader_cache_dump <directory>\n");
        return 2;
    }

    std::vector<uint8_t> cache(g_spirvCacheDecompressedSize);
    const size_t size = ZSTD_decompress(cache.data(), cache.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize);
    if (ZSTD_isError(size) || size != cache.size())
    {
        fprintf(stderr, "the shader cache does not decompress\n");
        return 1;
    }

    for (size_t i = 0; i < g_shaderCacheEntryCount; i++)
    {
        const ShaderCacheEntry& entry = g_shaderCacheEntries[i];
        if (size_t(entry.spirvOffset) + entry.spirvSize > cache.size())
        {
            fprintf(stderr, "shader %zu lies outside the cache\n", i);
            return 1;
        }

        const uint8_t* packed = cache.data() + entry.spirvOffset;
        std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(packed, entry.spirvSize));
        if (spirv.empty() || !smolv::Decode(packed, entry.spirvSize, spirv.data(), spirv.size()))
        {
            fprintf(stderr, "shader %zu does not unpack\n", i);
            return 1;
        }

        char name[64];
        snprintf(name, sizeof(name), "/cache_%016llX.spv", static_cast<unsigned long long>(entry.hash));

        FILE* file = fopen((std::string(argv[1]) + name).c_str(), "wb");
        if (file == nullptr || fwrite(spirv.data(), 1, spirv.size(), file) != spirv.size() || fclose(file) != 0)
        {
            fprintf(stderr, "cannot write %s%s\n", argv[1], name);
            return 2;
        }
    }

    printf("%zu\n", g_shaderCacheEntryCount);
    return 0;
}
