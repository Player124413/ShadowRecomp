#ifdef __ANDROID__

#include "android_video.h"

#include <os/android/logger_android.h>
#include <os/android/lifecycle_android.h>
#include <os/android/storage_android.h>
#include <os/android/vulkan_driver_android.h>
#include <os/logger.h>
#include <user/config.h>

#include <plume_vulkan.h>

#include "vulkan_pipeline_cache.h"
#include "vulkan_requirements.h"

#include <SDL3/SDL.h>
#include <android/native_window.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace plume;

namespace AndroidVideo
{
    // A failed surface creation is retried, but not on every single frame.
    static constexpr Uint64 SurfaceRetryIntervalMs = 250;
    static Uint64 s_nextSurfaceAttemptMs;

    static ANativeWindow* GetNativeWindow(SDL_Window* window)
    {
        if (window == nullptr)
            return nullptr;

        // SDL keeps this property in sync with the surface callbacks of the activity: it is
        // null between surfaceDestroyed and the next surfaceCreated.
        return static_cast<ANativeWindow*>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    }

    ESurfaceState GetSurfaceState(RenderSwapChain* swapChain, SDL_Window* window)
    {
        if (os::android::IsAppPaused())
            return ESurfaceState::Unavailable;

        ANativeWindow* nativeWindow = GetNativeWindow(window);
        if (nativeWindow == nullptr)
            return ESurfaceState::Unavailable;

        auto* vulkanSwapChain = static_cast<VulkanSwapChain*>(swapChain);
        if (vulkanSwapChain->surface == VK_NULL_HANDLE || vulkanSwapChain->desc.renderWindow != nativeWindow)
        {
            // Keep presenting nothing until the retry interval of a failed attempt is over.
            return SDL_GetTicks() >= s_nextSurfaceAttemptMs ? ESurfaceState::Replaced : ESurfaceState::Unavailable;
        }

        return ESurfaceState::Ready;
    }

    bool RecreateSurface(RenderSwapChain* swapChain, SDL_Window* window)
    {
        auto* vulkanSwapChain = static_cast<VulkanSwapChain*>(swapChain);
        ANativeWindow* nativeWindow = GetNativeWindow(window);

        // The window can disappear again between the state check and this call.
        if (nativeWindow == nullptr)
            return false;

        if (vulkanSwapChain->surface != VK_NULL_HANDLE && vulkanSwapChain->desc.renderWindow == nativeWindow)
            return true;

        VulkanCommandQueue* commandQueue = vulkanSwapChain->commandQueue;
        VulkanDevice* device = commandQueue->device;
        VkInstance instance = device->renderInterface->instance;

        // Create the new surface first: if that fails, the old one stays in place and the
        // swap chain object remains in a state the rest of the renderer can keep using.
        VkAndroidSurfaceCreateInfoKHR surfaceCreateInfo = {};
        surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceCreateInfo.window = nativeWindow;

        VkSurfaceKHR newSurface = VK_NULL_HANDLE;
        VkResult res = vkCreateAndroidSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &newSurface);
        if (res != VK_SUCCESS || newSurface == VK_NULL_HANDLE)
        {
            LOGFN_ERROR("vkCreateAndroidSurfaceKHR failed with error code {:#X}; retrying.", uint32_t(res));
            s_nextSurfaceAttemptMs = SDL_GetTicks() + SurfaceRetryIntervalMs;
            return false;
        }

        // The spec requires asking again for every new surface before creating a swap chain on it.
        VkBool32 presentSupported = VK_FALSE;
        res = vkGetPhysicalDeviceSurfaceSupportKHR(device->physicalDevice, commandQueue->familyIndex, newSurface, &presentSupported);
        if (res != VK_SUCCESS || presentSupported == VK_FALSE)
        {
            LOGFN_ERROR("The new window surface cannot be presented to (error code {:#X}); retrying.", uint32_t(res));
            vkDestroySurfaceKHR(instance, newSurface, nullptr);
            s_nextSurfaceAttemptMs = SDL_GetTicks() + SurfaceRetryIntervalMs;
            return false;
        }

        // Presents are not covered by the renderer's fences, so drain the queue before the
        // images of the old swap chain go away.
        {
            std::scoped_lock lock(*commandQueue->queue->mutex);
            vkQueueWaitIdle(commandQueue->queue->vk);
        }

        vulkanSwapChain->releaseImageViews();
        vulkanSwapChain->releaseSwapChain();

        if (vulkanSwapChain->surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance, vulkanSwapChain->surface, nullptr);

        vulkanSwapChain->surface = newSurface;
        vulkanSwapChain->desc.renderWindow = nativeWindow;

        // Composite alpha support belongs to the surface, so pick it again. The surface
        // format is a property of the device and the window type, and carries over.
        VkSurfaceCapabilitiesKHR surfaceCapabilities = {};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physicalDevice, newSurface, &surfaceCapabilities) == VK_SUCCESS)
        {
            if (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                vulkanSwapChain->pickedAlphaFlag = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            else if (surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                vulkanSwapChain->pickedAlphaFlag = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }

        LOGN("The window surface was replaced; recreated the Vulkan surface.");
        return true;
    }

    void OnFramePresentable()
    {
        // Only ever called by the thread that presents.
        static bool s_firstFrame = true;
        if (s_firstFrame)
        {
            s_firstFrame = false;
            os::android::SetLogLocation("in game");
        }

        // A custom driver that loads is not proof that it works; a usable swap chain is.
        AndroidMarkVulkanStartupSuccessful();
        os::android::LogHeartbeat();
    }

    void OnFramePresented()
    {
        // Only ever called by the thread that presents.
        static bool s_firstFrame = true;
        if (s_firstFrame)
        {
            s_firstFrame = false;
            os::android::LogStartupEvent("first frame of the game presented");
        }
    }

    void InitPipelineCache(RenderDevice* device)
    {
        if (!Config::PipelineDiskCache)
        {
            LOGN("Vulkan pipeline cache: turned off with Video.PipelineDiskCache in config.toml.");
            return;
        }

        const std::filesystem::path& cacheDir = os::android::GetCacheDir();
        if (cacheDir.empty())
            return;

        auto* vulkanDevice = static_cast<VulkanDevice*>(device);
        VulkanPipelineCache::Initialize(vulkanDevice->vk, vulkanDevice->physicalDeviceProperties,
            cacheDir / "vulkan_pipeline_cache.bin");
    }

    void SavePipelineCache()
    {
        VulkanPipelineCache::Save();
    }

    void OnCachedPipelinesCreated()
    {
        // The data of the earlier run went into all of these pipelines.
        VulkanPipelineCache::ConfirmLoadedData();
        VulkanPipelineCache::Save();

        // Batches of pipelines that turn up later (for shaders the game creates at runtime)
        // are no longer part of the startup.
        static std::atomic<bool> s_first{ true };
        if (s_first.exchange(false))
            os::android::LogStartupEvent("pipelines of the caches created");
    }

    static std::string s_missingDeviceFeature;

    static VulkanRequirements::DeviceReport QueryDeviceRequirements(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceProperties& properties)
    {
        VulkanRequirements::DeviceReport report;
        report.apiVersion = properties.apiVersion;

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount > 0 && vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()) < 0)
            extensionCount = 0;

        for (uint32_t i = 0; i < extensionCount && i < extensions.size(); i++)
        {
            if (strcmp(extensions[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0)
                report.descriptorIndexingExtension = true;
            else if (strcmp(extensions[i].extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
                report.bufferDeviceAddressExtension = true;
        }

        // vkGetPhysicalDeviceFeatures2 came with Vulkan 1.1.
        if (properties.apiVersion < VulkanRequirements::MakeVersion(1, 1) || vkGetPhysicalDeviceFeatures2 == nullptr)
            return report;

        // A driver only knows the structure of a feature set where it has the extension or
        // Vulkan 1.2, so it is only handed the ones it knows.
        const bool atLeast12 = properties.apiVersion >= VulkanRequirements::MakeVersion(1, 2);

        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceDescriptorIndexingFeatures indexing = {};
        indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        if (report.descriptorIndexingExtension || atLeast12)
        {
            indexing.pNext = features.pNext;
            features.pNext = &indexing;
        }

        VkPhysicalDeviceBufferDeviceAddressFeatures deviceAddress = {};
        deviceAddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        if (report.bufferDeviceAddressExtension || atLeast12)
        {
            deviceAddress.pNext = features.pNext;
            features.pNext = &deviceAddress;
        }

        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

        report.shaderInt64 = features.features.shaderInt64;
        report.runtimeDescriptorArray = indexing.runtimeDescriptorArray;
        report.descriptorBindingPartiallyBound = indexing.descriptorBindingPartiallyBound;
        report.descriptorBindingVariableDescriptorCount = indexing.descriptorBindingVariableDescriptorCount;
        report.bufferDeviceAddress = deviceAddress.bufferDeviceAddress;

        // Not needed, but the first thing to look at when a driver refuses a pipeline.
        LOGFN("Vulkan device \"{}\": depthClamp {}, shaderClipDistance {}, textureCompressionBC {}.", properties.deviceName,
            features.features.depthClamp ? "yes" : "no", features.features.shaderClipDistance ? "yes" : "no",
            features.features.textureCompressionBC ? "yes" : "no");

        return report;
    }

    bool CheckDeviceRequirements(RenderInterface* renderInterface)
    {
        s_missingDeviceFeature.clear();

        auto* vulkanInterface = static_cast<VulkanInterface*>(renderInterface);
        if (vulkanInterface->instance == VK_NULL_HANDLE)
            return true;

        // A phone has one GPU. Should there ever be more, one that will do is enough.
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(vulkanInterface->instance, &deviceCount, nullptr);

        std::vector<VkPhysicalDevice> devices(deviceCount);
        if (deviceCount == 0 || vkEnumeratePhysicalDevices(vulkanInterface->instance, &deviceCount, devices.data()) < 0)
            return true; // Nothing to ask. Creating the device fails and reports that itself.

        std::string missing;
        for (uint32_t i = 0; i < deviceCount && i < devices.size(); i++)
        {
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(devices[i], &properties);

            const VulkanRequirements::DeviceReport report = QueryDeviceRequirements(devices[i], properties);
            missing = VulkanRequirements::FindMissingFeature(report);

            const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
            const auto offered = [](bool value) { return value ? "offered" : "not offered"; };

            LOGFN("Vulkan device \"{}\": Vulkan {}.{}; VK_EXT_descriptor_indexing {} (runtimeDescriptorArray {}, descriptorBindingPartiallyBound {}, "
                  "descriptorBindingVariableDescriptorCount {}); VK_KHR_buffer_device_address {} (bufferDeviceAddress {}); shaderInt64 {}.",
                properties.deviceName, VulkanRequirements::FormatVersion(properties.apiVersion), properties.apiVersion & 0xFFF,
                offered(report.descriptorIndexingExtension), yesNo(report.runtimeDescriptorArray), yesNo(report.descriptorBindingPartiallyBound),
                yesNo(report.descriptorBindingVariableDescriptorCount),
                offered(report.bufferDeviceAddressExtension), yesNo(report.bufferDeviceAddress), yesNo(report.shaderInt64));

            if (missing.empty())
            {
                // The renderer calls this command by the name it has from Vulkan 1.2 on. An
                // older driver only has it under the name of the extension.
                if (properties.apiVersion < VulkanRequirements::MakeVersion(1, 2) && vkGetBufferDeviceAddressKHR != nullptr)
                    vkGetBufferDeviceAddress = vkGetBufferDeviceAddressKHR;

                return true;
            }

            LOGFN_ERROR("This Vulkan driver cannot run the game. It lacks: {}.", missing);
        }

        s_missingDeviceFeature = missing;
        return false;
    }

    bool IsPipelineUsable(const RenderPipeline* pipeline)
    {
        // plume hands out a pipeline object whether or not the driver created the pipeline.
        return pipeline != nullptr && static_cast<const VulkanGraphicsPipeline*>(pipeline)->vk != VK_NULL_HANDLE;
    }

    const std::string& GetMissingDeviceFeature()
    {
        return s_missingDeviceFeature;
    }

    void LogDeviceSupport(RenderDevice* device)
    {
        auto* vulkanDevice = static_cast<VulkanDevice*>(device);

        struct FormatUse
        {
            VkFormat format;
            VkFormatFeatureFlags features;
            const char* name;
            const char* usedFor;
        };

        static constexpr VkFormatFeatureFlags Sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        static constexpr VkFormatFeatureFlags Filtered = Sampled | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        static constexpr VkFormatFeatureFlags Target = Filtered | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

        // The host formats gpu/guest_convert.cpp maps the Xbox formats to.
        static constexpr FormatUse Formats[] =
        {
            { VK_FORMAT_B8G8R8A8_UNORM, Target, "B8G8R8A8_UNORM", "32-bit textures, render targets and the back buffer" },
            { VK_FORMAT_R8G8B8A8_UNORM, Target, "R8G8B8A8_UNORM", "the window surface and interface images" },
            { VK_FORMAT_R8_UNORM, Filtered, "R8_UNORM", "8-bit textures" },
            { VK_FORMAT_R16_UNORM, Sampled, "R16_UNORM", "16-bit colour textures" },
            { VK_FORMAT_D32_SFLOAT, Sampled | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "D32_SFLOAT", "depth buffers" },
            { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, Filtered, "BC1", "DXT1 textures" },
            { VK_FORMAT_BC2_UNORM_BLOCK, Filtered, "BC2", "DXT3 textures" },
            { VK_FORMAT_BC3_UNORM_BLOCK, Filtered, "BC3", "DXT5 textures" },
        };

        for (const FormatUse& use : Formats)
        {
            VkFormatProperties properties = {};
            vkGetPhysicalDeviceFormatProperties(vulkanDevice->physicalDevice, use.format, &properties);

            if ((properties.optimalTilingFeatures & use.features) == use.features)
                LOGFN("Vulkan format {}: supported", use.name);
            else
                LOGFN_WARNING("Vulkan format {} is NOT fully supported by this driver (features {:#X}, needed {:#X}); used for {}.",
                    use.name, uint32_t(properties.optimalTilingFeatures), uint32_t(use.features), use.usedFor);
        }
    }
}

#endif
