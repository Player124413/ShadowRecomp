#include "vulkan_int64_lowering.h"

#include "spirv_int64_lowering.h"

#include <os/logger.h>

#include <volk.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace VulkanInt64Lowering
{
    static PFN_vkCreateShaderModule s_createShaderModule = nullptr;

    static std::atomic<uint64_t> s_rewritten{ 0 };
    static std::atomic<uint64_t> s_unchanged{ 0 };
    static std::atomic<uint64_t> s_failed{ 0 };
    static std::atomic<uint64_t> s_refused{ 0 };

    // The game has a few hundred shaders. Should the rewrite fail, or the driver refuse
    // what it produces, that happens to most of them alike, and the log needs to hear of
    // it only so often.
    static constexpr uint64_t MAX_REPORTED_FAILURES = 4;

    // Holds no state between calls: any number of threads may be in here at once.
    static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModuleWithoutInt64(VkDevice device, const VkShaderModuleCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator, VkShaderModule* shaderModule)
    {
        std::vector<uint32_t> rewritten;
        std::string error;
        auto result = SpirvInt64Lowering::EResult::Failed;

        if (createInfo == nullptr || createInfo->pCode == nullptr)
        {
            error = "there is no code";
        }
        else if (createInfo->codeSize % sizeof(uint32_t) != 0)
        {
            error = "its size is not a whole number of words";
        }
        else
        {
            // Copied first: the game keeps its shaders in byte arrays, which need not start
            // where a 32-bit word may.
            std::vector<uint32_t> words(createInfo->codeSize / sizeof(uint32_t));
            if (!words.empty())
                memcpy(words.data(), createInfo->pCode, createInfo->codeSize);

            result = SpirvInt64Lowering::Lower(words.data(), words.size(), rewritten, error);
        }

        switch (result)
        {
        case SpirvInt64Lowering::EResult::Lowered:
        {
            // Everything else the caller asked for stays as it is.
            VkShaderModuleCreateInfo rewrittenInfo = *createInfo;
            rewrittenInfo.pCode = rewritten.data();
            rewrittenInfo.codeSize = rewritten.size() * sizeof(uint32_t);

            s_rewritten.fetch_add(1, std::memory_order_relaxed);

            const VkResult driverResult = s_createShaderModule(device, &rewrittenInfo, allocator, shaderModule);
            if (driverResult != VK_SUCCESS)
            {
                // plume only tells stderr, and then goes on without the shader.
                const uint64_t refusals = s_refused.fetch_add(1, std::memory_order_relaxed) + 1;
                if (refusals <= MAX_REPORTED_FAILURES)
                {
                    LOGFN_ERROR("The graphics driver refused a shader that was rewritten to do without 64-bit integers (error {}, {} words).{}",
                        int32_t(driverResult), rewritten.size(), refusals == MAX_REPORTED_FAILURES ? " Further shaders like this are only counted." : "");
                }
            }

            return driverResult;
        }

        case SpirvInt64Lowering::EResult::Unchanged:
            s_unchanged.fetch_add(1, std::memory_order_relaxed);
            break;

        case SpirvInt64Lowering::EResult::Failed:
        {
            // The driver gets the shader as it is. It will most likely turn down the
            // pipelines made of it, which the renderer reports; the reason is this line.
            const uint64_t failures = s_failed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failures <= MAX_REPORTED_FAILURES)
            {
                LOGFN_ERROR("A shader could not be rewritten to do without 64-bit integers: {}. "
                            "The driver is given the shader unchanged; one that lacks them will most likely refuse it.{}",
                    error, failures == MAX_REPORTED_FAILURES ? " Further shaders like this are only counted." : "");
            }

            break;
        }
        }

        return s_createShaderModule(device, createInfo, allocator, shaderModule);
    }

    void Install()
    {
        if (s_createShaderModule != nullptr || vkCreateShaderModule == nullptr)
            return;

        s_createShaderModule = vkCreateShaderModule;
        vkCreateShaderModule = CreateShaderModuleWithoutInt64;
    }

    Counts GetCounts()
    {
        Counts counts;
        counts.rewritten = s_rewritten.load(std::memory_order_relaxed);
        counts.unchanged = s_unchanged.load(std::memory_order_relaxed);
        counts.failed = s_failed.load(std::memory_order_relaxed);
        counts.refused = s_refused.load(std::memory_order_relaxed);
        return counts;
    }
}
