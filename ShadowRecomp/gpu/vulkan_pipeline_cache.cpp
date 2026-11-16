#include "vulkan_pipeline_cache.h"

#include <os/logger.h>

#include <volk.h>
#include <xxhash.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <system_error>
#include <vector>

namespace VulkanPipelineCache
{
    // The file is this header followed by the data exactly as vkGetPipelineCacheData returned it.
    //
    // The Vulkan specification has the driver check the header of that data against itself,
    // but leaves it to the application that everything behind the header is what the driver
    // once wrote: hence the checksum. The device fields repeat the driver's check, and add
    // the driver version, so that the data of another driver is not even handed to this one.
    struct FileHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t vendorID;
        uint32_t deviceID;
        uint32_t driverVersion;
        uint8_t pipelineCacheUUID[VK_UUID_SIZE];
        uint32_t reserved;
        uint64_t dataSize;
        uint64_t dataHash; // XXH3_64bits of the data.
    };

    static_assert(sizeof(FileHeader) == 56, "The layout of the pipeline cache file changed: bump FILE_VERSION.");

    static constexpr uint32_t FILE_MAGIC = 0x43505253; // "SRPC"
    static constexpr uint32_t FILE_VERSION = 1;

    // Far more than any driver produces for the few thousand pipelines of this game. A file
    // that claims more is not read into memory, and a cache that grew beyond it is not written.
    static constexpr uint64_t MAX_DATA_SIZE = 512ull * 1024 * 1024;

    static VkDevice s_device = VK_NULL_HANDLE;
    static VkPipelineCache s_cache = VK_NULL_HANDLE;
    static PFN_vkCreateGraphicsPipelines s_createGraphicsPipelines = nullptr;
    static FileHeader s_identity{}; // Magic, version and the device fields; the data fields stay zero.
    static std::filesystem::path s_file;
    static std::filesystem::path s_marker;

    // Pipelines created since startup. Only its changes matter: no change, nothing to save.
    static std::atomic<uint64_t> s_createdCount{ 0 };

    static std::mutex s_saveMutex;
    static uint64_t s_createdCountAtSave = 0; // Guarded by s_saveMutex, as are the next three.
    static uint64_t s_savedDataSize = 0;      // What the driver reported for the data the file holds.
    static bool s_markerPresent = false;      // Data was read in this run and is not confirmed yet.
    static bool s_oversizeReported = false;

    static double MillisecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // What plume's pipeline creation calls instead of the driver.
    static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelinesWithCache(VkDevice device, VkPipelineCache pipelineCache,
        uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
    {
        if (pipelineCache == VK_NULL_HANDLE && device == s_device)
            pipelineCache = s_cache;

        const VkResult result = s_createGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

        // Counted afterwards: a save that runs meanwhile must not conclude that it has
        // captured a pipeline the driver is still working on.
        s_createdCount.fetch_add(createInfoCount, std::memory_order_relaxed);
        return result;
    }

    // Reads the driver data out of the file. Returns false, with the reason, if there is
    // none that this device's driver wrote and that is still intact.
    static bool ReadCacheFile(std::vector<uint8_t>& data, const char*& reason)
    {
        data.clear();

        FILE* file = fopen(s_file.c_str(), "rb");
        if (file == nullptr)
        {
            reason = "there is no file yet";
            return false;
        }

        FileHeader header{};
        bool ok = false;

        if (fread(&header, sizeof(header), 1, file) != 1 || header.magic != FILE_MAGIC)
        {
            reason = "the file is not a pipeline cache";
        }
        else if (header.version != FILE_VERSION)
        {
            reason = "the file was written by another version of the game";
        }
        else if (header.vendorID != s_identity.vendorID || header.deviceID != s_identity.deviceID ||
            header.driverVersion != s_identity.driverVersion ||
            memcmp(header.pipelineCacheUUID, s_identity.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        {
            reason = "the file was written by another graphics driver";
        }
        else if (header.dataSize < sizeof(VkPipelineCacheHeaderVersionOne) || header.dataSize > MAX_DATA_SIZE)
        {
            reason = "the file has an implausible size";
        }
        else
        {
            data.resize(size_t(header.dataSize));

            // One byte more than expected must not be there either.
            uint8_t extra = 0;
            if (fread(data.data(), 1, data.size(), file) != data.size() || fread(&extra, 1, 1, file) != 0)
            {
                reason = "the file is incomplete or has trailing data";
            }
            else if (XXH3_64bits(data.data(), data.size()) != header.dataHash)
            {
                reason = "the file is damaged";
            }
            else
            {
                // What the driver is going to check as well. Done here too, because a driver
                // that omits the check would go on to use data that is not its own.
                VkPipelineCacheHeaderVersionOne driverHeader{};
                memcpy(&driverHeader, data.data(), sizeof(driverHeader));

                if (driverHeader.headerSize < sizeof(driverHeader) || driverHeader.headerSize > data.size() ||
                    driverHeader.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                    driverHeader.vendorID != s_identity.vendorID || driverHeader.deviceID != s_identity.deviceID ||
                    memcmp(driverHeader.pipelineCacheUUID, s_identity.pipelineCacheUUID, VK_UUID_SIZE) != 0)
                {
                    reason = "the data in the file does not belong to this graphics driver";
                }
                else
                {
                    ok = true;
                }
            }
        }

        fclose(file);

        if (!ok)
            data.clear();

        return ok;
    }

    static bool WriteCacheFile(const std::vector<uint8_t>& data)
    {
        FileHeader header = s_identity;
        header.dataSize = data.size();
        header.dataHash = XXH3_64bits(data.data(), data.size());

        std::error_code ec;
        std::filesystem::create_directories(s_file.parent_path(), ec);

        // Written next to the file and then renamed over it: a process that is killed while
        // it writes, which is how most Android processes end, leaves the previous file whole.
        std::filesystem::path temporary = s_file;
        temporary += ".tmp";

        FILE* file = fopen(temporary.c_str(), "wb");
        if (file == nullptr)
            return false;

        bool ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(data.data(), 1, data.size(), file) == data.size();
        ok = fclose(file) == 0 && ok;

        if (ok)
        {
            std::filesystem::rename(temporary, s_file, ec);
            ok = !ec;
        }

        if (!ok)
            std::filesystem::remove(temporary, ec);

        return ok;
    }

    static bool CreateMarker()
    {
        std::error_code ec;
        std::filesystem::create_directories(s_marker.parent_path(), ec);

        FILE* file = fopen(s_marker.c_str(), "wb");
        if (file == nullptr)
            return false;

        return fclose(file) == 0;
    }

    static VkResult CreateCache(const std::vector<uint8_t>& initialData)
    {
        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = initialData.size();
        createInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();

        s_cache = VK_NULL_HANDLE;
        const VkResult result = vkCreatePipelineCache(s_device, &createInfo, nullptr, &s_cache);
        if (result != VK_SUCCESS)
            s_cache = VK_NULL_HANDLE;

        return result;
    }

    void Initialize(VkDevice device, const VkPhysicalDeviceProperties& deviceProperties, const std::filesystem::path& file)
    {
        // Save() may already be called by other threads, which then find no cache yet.
        std::lock_guard lock(s_saveMutex);

        if (s_device != VK_NULL_HANDLE || device == VK_NULL_HANDLE || file.empty())
            return;

        if (vkCreatePipelineCache == nullptr || vkGetPipelineCacheData == nullptr || vkCreateGraphicsPipelines == nullptr)
            return;

        const auto start = std::chrono::steady_clock::now();

        s_device = device;
        s_file = file;
        s_marker = file;
        s_marker += ".loading";

        s_identity = {};
        s_identity.magic = FILE_MAGIC;
        s_identity.version = FILE_VERSION;
        s_identity.vendorID = deviceProperties.vendorID;
        s_identity.deviceID = deviceProperties.deviceID;
        s_identity.driverVersion = deviceProperties.driverVersion;
        memcpy(s_identity.pipelineCacheUUID, deviceProperties.pipelineCacheUUID, VK_UUID_SIZE);

        std::error_code ec;
        std::vector<uint8_t> data;
        const char* reason = "";
        bool loaded = false;

        if (std::filesystem::exists(s_marker, ec))
        {
            // The run that read this file did not get as far as ConfirmLoadedData(). Most
            // likely it was simply closed early, but the data may also be what ended it.
            std::filesystem::remove(s_file, ec);
            if (std::filesystem::exists(s_file, ec))
            {
                // Neither removable nor trustworthy: the marker stays, for the next run to
                // try again.
                reason = "the file of an unconfirmed earlier run could not be removed";
            }
            else
            {
                std::filesystem::remove(s_marker, ec);
                reason = "the previous run ended before the data it read was confirmed";
            }
        }
        else
        {
            loaded = ReadCacheFile(data, reason);

            // No marker, no safety net: then rather compile everything once more.
            if (loaded && !CreateMarker())
            {
                data.clear();
                loaded = false;
                reason = "the marker file could not be created";
            }

            s_markerPresent = loaded;
        }

        VkResult result = CreateCache(data);
        if (result != VK_SUCCESS && loaded)
        {
            // The driver turned the data down in a way the checks above do not cover.
            std::filesystem::remove(s_file, ec);
            std::filesystem::remove(s_marker, ec);
            s_markerPresent = false;
            data.clear();
            loaded = false;
            reason = "the graphics driver did not accept the data";

            result = CreateCache(data);
        }

        if (result != VK_SUCCESS)
        {
            LOGFN_WARNING("Vulkan pipeline cache: the driver could not create one (error {}); pipelines are compiled without.", int32_t(result));
            return;
        }

        // The baseline for Save(). Asked of the driver rather than taken from the file: it
        // may report its data at another size than it was given.
        size_t reportedSize = 0;
        if (loaded && vkGetPipelineCacheData(s_device, s_cache, &reportedSize, nullptr) == VK_SUCCESS)
            s_savedDataSize = reportedSize;

        s_createGraphicsPipelines = vkCreateGraphicsPipelines;
        vkCreateGraphicsPipelines = CreateGraphicsPipelinesWithCache;

        if (loaded)
        {
            LOGFN("Vulkan pipeline cache: read {} KiB from an earlier run in {:.1f} ms.", data.size() / 1024, MillisecondsSince(start));
        }
        else
        {
            LOGFN("Vulkan pipeline cache: starting empty, {}. Pipelines are compiled from scratch in this run and reused in the next.", reason);
        }
    }

    void Save()
    {
        std::lock_guard lock(s_saveMutex);

        if (s_cache == VK_NULL_HANDLE)
            return;

        const uint64_t createdCount = s_createdCount.load(std::memory_order_relaxed);
        if (createdCount == s_createdCountAtSave)
            return;

        const auto start = std::chrono::steady_clock::now();

        size_t size = 0;
        if (vkGetPipelineCacheData(s_device, s_cache, &size, nullptr) != VK_SUCCESS)
            return;

        if (size == s_savedDataSize)
        {
            // Every pipeline created since was already in the cache.
            s_createdCountAtSave = createdCount;
            return;
        }

        if (size < sizeof(VkPipelineCacheHeaderVersionOne) || size > MAX_DATA_SIZE)
        {
            if (!s_oversizeReported)
            {
                s_oversizeReported = true;
                LOGFN_WARNING("Vulkan pipeline cache: the driver reports {} bytes of data, which is not written to disk.", size);
            }

            return;
        }

        std::vector<uint8_t> data(size);
        const VkResult result = vkGetPipelineCacheData(s_device, s_cache, &size, data.data());

        // VK_INCOMPLETE: the cache grew between the two calls. What was written is whole and
        // usable by itself; the rest is picked up by the next save.
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || size < sizeof(VkPipelineCacheHeaderVersionOne) || size > data.size())
            return;

        data.resize(size);

        if (!WriteCacheFile(data))
        {
            LOGFN_WARNING("Vulkan pipeline cache: could not write {}.", s_file.string());
            return;
        }

        s_createdCountAtSave = createdCount;
        s_savedDataSize = result == VK_SUCCESS ? size : 0;

        LOGFN("Vulkan pipeline cache: wrote {} KiB in {:.1f} ms.", size / 1024, MillisecondsSince(start));
    }

    void ConfirmLoadedData()
    {
        std::lock_guard lock(s_saveMutex);

        if (!s_markerPresent)
            return;

        std::error_code ec;
        std::filesystem::remove(s_marker, ec);
        s_markerPresent = false;
    }
}
