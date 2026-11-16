// One simulated run of the app for ShadowRecomp/gpu/vulkan_pipeline_cache.cpp, built for
// the build machine with a stand-in for the graphics driver. pipeline_cache_test.py strings
// runs together.
//
//   pipeline_cache_harness <cache file> <operation>...
//   pipeline_cache_harness --rehash <cache file>
//
// Operations, carried out in order:
//   init                     VulkanPipelineCache::Initialize()
//   create:<id>[,<id>...]    create pipelines the way plume does (no cache passed)
//   save                     VulkanPipelineCache::Save()
//   confirm                  VulkanPipelineCache::ConfirmLoadedData()
//   threads:<n>:<count>      n threads create <count> pipelines each while this one saves
//
// A run ends like an Android process does: without any cleanup. What happened is printed
// as key=value lines on stdout, log messages go to stderr.
//
// The stand-in driver keeps the ids of the pipelines it has "compiled" in its cache. The
// TEST_* environment variables give it an identity and make it misbehave:
//   TEST_VENDOR, TEST_DEVICE, TEST_DRIVER_VERSION, TEST_UUID   identity (numbers; one character)
//   TEST_NO_CACHE          cannot create a pipeline cache at all
//   TEST_REJECT_DATA       fails to create a cache from data, works without
//   TEST_CRASH_ON_DATA     dies when it is given data
//   TEST_HUGE              reports an absurd amount of cache data
//   TEST_SIZE_DRIFT        reports data it was given at another size than it had
//   TEST_GROW_ON_QUERY     adds to the cache between the size query and the read, once
//
// --rehash makes the checksum of a cache file match its contents again, for tests that
// alter the driver's data on purpose.
#include <gpu/vulkan_pipeline_cache.h>
#include <os/logger.h>

#include <volk.h>
#include <xxhash.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace os::logger
{
    void Log(const std::string_view str, ELogType type, const char*)
    {
        fprintf(stderr, "[%s] %.*s\n", type == ELogType::Error ? "E" : type == ELogType::Warning ? "W" : "I", int(str.size()), str.data());
    }
}

// --- The stand-in driver ----------------------------------------------------------------

static const VkDevice g_device = reinterpret_cast<VkDevice>(uintptr_t(0x1000));
static const VkPipelineCache g_cacheHandle = reinterpret_cast<VkPipelineCache>(uintptr_t(0x2000));

static VkPhysicalDeviceProperties g_properties{};

static std::mutex g_driverMutex;
static std::set<uint32_t> g_entries; // Guarded by g_driverMutex, as is everything below.
static size_t g_padding = 0;
static bool g_cacheCreated = false;
static bool g_driverIgnoredData = false;
static bool g_grewOnQuery = false;
static int g_createCacheCalls = 0;
static int g_getDataCalls = 0;
static long long g_initialDataSize = -1;
static int g_hits = 0;
static int g_misses = 0;
static int g_uncached = 0;

static uint32_t EnvNumber(const char* name, uint32_t fallback)
{
    const char* value = getenv(name);
    return value != nullptr ? uint32_t(strtoul(value, nullptr, 0)) : fallback;
}

static std::vector<uint8_t> SerializeLocked()
{
    VkPipelineCacheHeaderVersionOne header{};
    header.headerSize = sizeof(header);
    header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    header.vendorID = g_properties.vendorID;
    header.deviceID = g_properties.deviceID;
    memcpy(header.pipelineCacheUUID, g_properties.pipelineCacheUUID, VK_UUID_SIZE);

    std::vector<uint8_t> data(sizeof(header) + 4 + g_entries.size() * 4 + g_padding, 0xEE);
    memcpy(data.data(), &header, sizeof(header));
    const uint32_t count = uint32_t(g_entries.size());
    memcpy(data.data() + sizeof(header), &count, 4);
    size_t at = sizeof(header) + 4;
    for (uint32_t id : g_entries)
    {
        memcpy(data.data() + at, &id, 4);
        at += 4;
    }

    return data;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* createInfo,
    const VkAllocationCallbacks*, VkPipelineCache* cache)
{
    std::lock_guard lock(g_driverMutex);
    g_createCacheCalls++;

    if (device != g_device || createInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO || createInfo->flags != 0)
        abort(); // not how the cache is meant to be created

    if (getenv("TEST_NO_CACHE"))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (createInfo->initialDataSize != 0)
    {
        if (getenv("TEST_CRASH_ON_DATA"))
            abort();

        if (getenv("TEST_REJECT_DATA"))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    g_initialDataSize = (long long)createInfo->initialDataSize;
    g_entries.clear();
    g_padding = 0;

    if (createInfo->initialDataSize != 0)
    {
        const uint8_t* data = static_cast<const uint8_t*>(createInfo->pInitialData);
        VkPipelineCacheHeaderVersionOne header{};
        uint32_t count = 0;

        // What a real driver does: data that is not its own is ignored.
        const bool usable = createInfo->initialDataSize >= sizeof(header) + 4 &&
            (memcpy(&header, data, sizeof(header)), true) &&
            header.headerSize == sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
            header.vendorID == g_properties.vendorID && header.deviceID == g_properties.deviceID &&
            memcmp(header.pipelineCacheUUID, g_properties.pipelineCacheUUID, VK_UUID_SIZE) == 0 &&
            (memcpy(&count, data + sizeof(header), 4), true) &&
            createInfo->initialDataSize >= sizeof(header) + 4 + size_t(count) * 4;

        if (usable)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t id = 0;
                memcpy(&id, data + sizeof(header) + 4 + size_t(i) * 4, 4);
                g_entries.insert(id);
            }

            if (getenv("TEST_SIZE_DRIFT"))
                g_padding = 8;
        }
        else
        {
            g_driverIgnoredData = true;
        }
    }

    g_cacheCreated = true;
    *cache = g_cacheHandle;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t* size, void* data)
{
    std::lock_guard lock(g_driverMutex);
    g_getDataCalls++;

    if (device != g_device || cache != g_cacheHandle)
        abort();

    if (getenv("TEST_HUGE"))
    {
        *size = size_t(1) << 40;
        return data == nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    const std::vector<uint8_t> serialized = SerializeLocked();
    if (data == nullptr)
    {
        *size = serialized.size();

        // Another thread finishes a pipeline right after the size was reported.
        if (getenv("TEST_GROW_ON_QUERY") && !g_grewOnQuery && !g_entries.empty())
        {
            g_grewOnQuery = true;
            g_entries.insert(0xFFFFFF00u);
        }

        return VK_SUCCESS;
    }

    if (*size < serialized.size())
    {
        // Whole entries only, as the specification demands of what is written.
        const size_t headerSize = sizeof(VkPipelineCacheHeaderVersionOne) + 4;
        const uint32_t fit = *size >= headerSize ? uint32_t((*size - headerSize) / 4) : 0;
        if (*size >= headerSize)
        {
            memcpy(data, serialized.data(), headerSize + size_t(fit) * 4);
            memcpy(static_cast<uint8_t*>(data) + sizeof(VkPipelineCacheHeaderVersionOne), &fit, 4);
            *size = headerSize + size_t(fit) * 4;
        }
        else
        {
            *size = 0;
        }

        return VK_INCOMPLETE;
    }

    memcpy(data, serialized.data(), serialized.size());
    *size = serialized.size();
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* createInfos, const VkAllocationCallbacks*, VkPipeline* pipelines)
{
    std::lock_guard lock(g_driverMutex);

    if (device != g_device || (cache != VK_NULL_HANDLE && cache != g_cacheHandle))
        abort();

    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = createInfos[i].subpass; // the tests' pipeline id
        if (cache == VK_NULL_HANDLE)
            g_uncached++;
        else if (g_entries.insert(id).second)
            g_misses++;
        else
            g_hits++;

        pipelines[i] = reinterpret_cast<VkPipeline>(uintptr_t(0x3000) + id);
    }

    return VK_SUCCESS;
}

// volk's entry points, which plume and the code under test call.
extern "C"
{
    PFN_vkCreatePipelineCache vkCreatePipelineCache = FakeCreatePipelineCache;
    PFN_vkGetPipelineCacheData vkGetPipelineCacheData = FakeGetPipelineCacheData;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = FakeCreateGraphicsPipelines;
}

// --- The run ------------------------------------------------------------------------------

static void CreatePipelines(const std::vector<uint32_t>& ids)
{
    std::vector<VkGraphicsPipelineCreateInfo> createInfos(ids.size());
    std::vector<VkPipeline> pipelines(ids.size());
    for (size_t i = 0; i < ids.size(); i++)
    {
        createInfos[i].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createInfos[i].subpass = ids[i];
    }

    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, uint32_t(ids.size()), createInfos.data(), nullptr, pipelines.data()) != VK_SUCCESS)
        abort();

    for (size_t i = 0; i < ids.size(); i++)
    {
        if (pipelines[i] != reinterpret_cast<VkPipeline>(uintptr_t(0x3000) + ids[i]))
            abort(); // the driver's result must come back unchanged
    }
}

static int Rehash(const char* path)
{
    FILE* file = fopen(path, "r+b");
    if (file == nullptr)
        return 2;

    std::vector<uint8_t> contents;
    uint8_t buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
        contents.insert(contents.end(), buffer, buffer + read);

    constexpr size_t HeaderSize = 56;
    if (contents.size() < HeaderSize)
        return 3;

    const uint64_t hash = XXH3_64bits(contents.data() + HeaderSize, contents.size() - HeaderSize);
    fseek(file, long(HeaderSize - 8), SEEK_SET);
    fwrite(&hash, sizeof(hash), 1, file);
    fclose(file);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--rehash") == 0)
        return Rehash(argv[2]);

    if (argc < 2)
        return 2;

    g_properties.vendorID = EnvNumber("TEST_VENDOR", 0x5143);
    g_properties.deviceID = EnvNumber("TEST_DEVICE", 0x43050A01);
    g_properties.driverVersion = EnvNumber("TEST_DRIVER_VERSION", 100);
    const char* uuid = getenv("TEST_UUID");
    memset(g_properties.pipelineCacheUUID, uuid != nullptr && uuid[0] != '\0' ? uuid[0] : 'u', VK_UUID_SIZE);

    const std::string file = argv[1];

    for (int i = 2; i < argc; i++)
    {
        const std::string operation = argv[i];

        if (operation == "init")
        {
            VulkanPipelineCache::Initialize(g_device, g_properties, file);
        }
        else if (operation == "init-nothing")
        {
            // Neither of these can be used; nothing may happen.
            VulkanPipelineCache::Initialize(VK_NULL_HANDLE, g_properties, file);
            VulkanPipelineCache::Initialize(g_device, g_properties, std::filesystem::path());
        }
        else if (operation.rfind("create:", 0) == 0)
        {
            std::vector<uint32_t> ids;
            const char* at = operation.c_str() + 7;
            while (*at != '\0')
            {
                char* end = nullptr;
                ids.push_back(uint32_t(strtoul(at, &end, 10)));
                at = *end == ',' ? end + 1 : end;
            }

            CreatePipelines(ids);
        }
        else if (operation == "save")
        {
            VulkanPipelineCache::Save();
        }
        else if (operation == "confirm")
        {
            VulkanPipelineCache::ConfirmLoadedData();
        }
        else if (operation.rfind("threads:", 0) == 0)
        {
            int threadCount = 0;
            int perThread = 0;
            if (sscanf(operation.c_str(), "threads:%d:%d", &threadCount, &perThread) != 2)
                return 2;

            std::atomic<int> running{ threadCount };
            std::vector<std::thread> threads;
            for (int t = 0; t < threadCount; t++)
            {
                threads.emplace_back([t, perThread, &running]()
                {
                    for (int n = 0; n < perThread; n++)
                        CreatePipelines({ uint32_t(100000 + t * 10000 + n) });

                    running--;
                });
            }

            while (running.load() > 0)
            {
                VulkanPipelineCache::Save();
                VulkanPipelineCache::ConfirmLoadedData();
                usleep(200);
            }

            for (auto& thread : threads)
                thread.join();
        }
        else
        {
            return 2;
        }
    }

    {
        std::lock_guard lock(g_driverMutex);
        printf("cache_created=%d\n", g_cacheCreated ? 1 : 0);
        printf("create_cache_calls=%d\n", g_createCacheCalls);
        printf("initial_data=%lld\n", g_initialDataSize);
        printf("driver_ignored_data=%d\n", g_driverIgnoredData ? 1 : 0);
        printf("hooked=%d\n", vkCreateGraphicsPipelines != FakeCreateGraphicsPipelines ? 1 : 0);
        printf("hits=%d\n", g_hits);
        printf("misses=%d\n", g_misses);
        printf("uncached=%d\n", g_uncached);
        printf("entries=%zu\n", g_entries.size());
        printf("get_data_calls=%d\n", g_getDataCalls);
    }

    fflush(stdout);
    _Exit(0);
}
