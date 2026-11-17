#ifdef __ANDROID__

#include "android_video.h"

#include <os/android/logger_android.h>
#include <os/android/lifecycle_android.h>
#include <os/android/storage_android.h>
#include <os/android/vulkan_driver_android.h>
#include <os/logger.h>
#include <user/config.h>

#include <plume_vulkan.h>

#include "vulkan_int64_lowering.h"
#include "vulkan_pipeline_cache.h"
#include "vulkan_requirements.h"

#include <SDL3/SDL.h>
#include <android/native_window.h>

#include <atomic>
#include <cstring>
#include <iterator>
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

    // Frames since a frame was last presentable; the black-screen watchdog's count. Both
    // this and OnFramePresentable() belong to the thread that renders, so the plain
    // counter needs no synchronization.
    static uint32_t s_framesSincePresentable = 0;
    static bool s_blackScreenNoted = false;

    void OnFramePresentable()
    {
        // Only ever called by the thread that presents.
        static bool s_firstFrame = true;
        if (s_firstFrame)
        {
            s_firstFrame = false;
            os::android::SetLogLocation("in game");
        }

        s_framesSincePresentable = 0;

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

    void NoteRenderFailed(const char* reason)
    {
        AndroidNoteRenderFailed(reason);
    }

    // The game normally presents every frame it runs. This many ticks without one, while
    // a window exists and the app is up, is a driver that never puts a frame on the
    // screen: about half a minute at the usual frame rate, longer on a slow one - a wrong
    // note is much worse than a late one, so the count is generous.
    static constexpr uint32_t BlackScreenTickLimit = 3600;

    void WatchdogFrameTick()
    {
        if (s_blackScreenNoted)
            return;

        if (++s_framesSincePresentable < BlackScreenTickLimit)
            return;

        s_blackScreenNoted = true;
        LOGN_ERROR("The game is running, but no frame has reached the screen for a long time. "
                   "This driver path is noted as broken; the next launch uses the other one.");
        AndroidNoteRenderFailed("blackscreen");
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

    static bool s_shadersRewritten = false; // Written once, before any other thread creates shaders.

    void InitShaderRewrite(RenderDevice* device)
    {
        // Asked of the device plume settled on. plume enables every feature a driver has.
        VkPhysicalDeviceFeatures features = {};
        vkGetPhysicalDeviceFeatures(static_cast<VulkanDevice*>(device)->physicalDevice, &features);

        if (features.shaderInt64 && !Config::ShadersWithoutInt64)
            return;

        VulkanInt64Lowering::Install();
        s_shadersRewritten = true;

        if (features.shaderInt64)
        {
            LOGN("Shaders: rewritten to do without 64-bit integers, as Video.ShadersWithoutInt64 in config.toml asks. This driver has them.");
        }
        else
        {
            LOGN("Shaders: this graphics driver has no 64-bit integers in shaders (shaderInt64). "
                 "Every shader is rewritten to do without them before the driver gets it.");
        }
    }

    // A healthy driver creates every pipeline the game asks for, so a handful of refusals
    // is already wrong; this many means whole parts of the picture are missing, not a
    // glitch. (The Adreno 710 system driver, for example, refuses hundreds.) When the
    // count crosses the threshold the driver loader is told once (it records the driver
    // as broken for the next launch, see os/android/vulkan_driver_android.h); while it
    // stays under, the checkpoints confirm the driver as healthy instead.
    //
    // `sustained` is false for the early checkpoint (the pipelines of the caches are
    // created) and true for the [render] reports, once frames have been drawn for a
    // while: only a sustained proof clears an override that is still on trial.
    static constexpr uint32_t BrokenDriverPipelineThreshold = 32;
    static std::atomic<bool> s_renderBrokenNoted{ false };

    static void NoteRenderHealth(bool sustained)
    {
        const uint32_t refused = GetRefusedPipelineCount();
        if (refused >= BrokenDriverPipelineThreshold)
        {
            if (!s_renderBrokenNoted.exchange(true))
                AndroidNoteBrokenRenderDriver(refused);
        }
        else if (!s_renderBrokenNoted.load())
        {
            AndroidNoteRenderHealthy(sustained);
        }
    }

    void OnCachedPipelinesCreated()
    {
        // The data of the earlier run went into all of these pipelines.
        VulkanPipelineCache::ConfirmLoadedData();
        VulkanPipelineCache::Save();

        // Every pipeline of the caches has been offered to the driver by now: the first
        // moment a driver that refuses them en masse can be told apart from a healthy one.
        NoteRenderHealth(false);

        // Batches of pipelines that turn up later (for shaders the game creates at runtime)
        // are no longer part of the startup.
        static std::atomic<bool> s_first{ true };
        if (s_first.exchange(false))
        {
            os::android::LogStartupEvent("pipelines of the caches created");

            if (s_shadersRewritten)
            {
                const VulkanInt64Lowering::Counts counts = VulkanInt64Lowering::GetCounts();
                LOGFN("Shaders: {} rewritten without 64-bit integers so far ({} of them refused by the driver), {} had none, "
                      "{} could not be rewritten.", counts.rewritten, counts.refused, counts.unchanged, counts.failed);
            }
        }
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

    static std::atomic<uint32_t> s_refusedPipelineCount{ 0 };

    uint32_t CountRefusedPipeline()
    {
        return ++s_refusedPipelineCount;
    }

    uint32_t GetRefusedPipelineCount()
    {
        return s_refusedPipelineCount.load();
    }

    // What the driver says about itself, for whoever reads a report of a wrong picture: the
    // numbers and switches a renderer runs into first on a GPU it was not written on.
    static void LogDeviceDetails(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceProperties& properties)
    {
        const auto yesNo = [](VkBool32 value) { return value ? "yes" : "no"; };
        const bool atLeast12 = properties.apiVersion >= VulkanRequirements::MakeVersion(1, 2);

        // Which of the structures below the driver knows.
        bool indexingKnown = atLeast12;
        bool driverPropertiesKnown = atLeast12;

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount > 0 && vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()) < 0)
            extensionCount = 0;

        for (uint32_t i = 0; i < extensionCount && i < extensions.size(); i++)
        {
            if (strcmp(extensions[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0)
                indexingKnown = true;
            else if (strcmp(extensions[i].extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0)
                driverPropertiesKnown = true;
        }

        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures = {};
        indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceProperties2 properties2 = {};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

        VkPhysicalDeviceDescriptorIndexingProperties indexingProperties = {};
        indexingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;

        VkPhysicalDeviceDriverProperties driverProperties = {};
        driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        if (indexingKnown)
        {
            features.pNext = &indexingFeatures;
            properties2.pNext = &indexingProperties;
        }

        if (driverPropertiesKnown)
        {
            driverProperties.pNext = properties2.pNext;
            properties2.pNext = &driverProperties;
        }

        // Both commands came with Vulkan 1.1, which CheckDeviceRequirements() insisted on.
        if (vkGetPhysicalDeviceFeatures2 == nullptr || vkGetPhysicalDeviceProperties2 == nullptr)
            return;

        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

        const VkPhysicalDeviceFeatures& f = features.features;
        const VkPhysicalDeviceLimits& limits = properties.limits;

        if (driverPropertiesKnown)
        {
            driverProperties.driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = '\0';
            driverProperties.driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = '\0';
        }

        LOGFN("Vulkan driver: {} {} (version {:#x}, vendor {:#x}, device {:#x}, type {}).",
            driverPropertiesKnown ? driverProperties.driverName : "unnamed", driverPropertiesKnown ? driverProperties.driverInfo : "",
            properties.driverVersion, properties.vendorID, properties.deviceID, uint32_t(properties.deviceType));

        LOGFN("Vulkan limits: {} descriptor sets, {} bytes of push constants, {} vertex attributes in {} buffers, {} colour targets, "
              "textures up to {}, anisotropy up to {:.0f}; descriptors that may change while bound: {} textures and {} samplers per stage.",
            limits.maxBoundDescriptorSets, limits.maxPushConstantsSize, limits.maxVertexInputAttributes, limits.maxVertexInputBindings,
            limits.maxColorAttachments, limits.maxImageDimension2D, limits.maxSamplerAnisotropy,
            indexingKnown ? indexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages : 0,
            indexingKnown ? indexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers : 0);

        LOGFN("Vulkan features: depthClamp {}, depthBiasClamp {}, fillModeNonSolid {}, largePoints {}, wideLines {}, samplerAnisotropy {}, "
              "independentBlend {}, dualSrcBlend {}, sampleRateShading {}, shaderClipDistance {}, shaderInt16 {}, shaderInt64 {}, "
              "textureCompressionBC {}, textureCompressionETC2 {}, textureCompressionASTC_LDR {}; sampled images updated after bind {}.",
            yesNo(f.depthClamp), yesNo(f.depthBiasClamp), yesNo(f.fillModeNonSolid), yesNo(f.largePoints), yesNo(f.wideLines), yesNo(f.samplerAnisotropy),
            yesNo(f.independentBlend), yesNo(f.dualSrcBlend), yesNo(f.sampleRateShading), yesNo(f.shaderClipDistance), yesNo(f.shaderInt16), yesNo(f.shaderInt64),
            yesNo(f.textureCompressionBC), yesNo(f.textureCompressionETC2), yesNo(f.textureCompressionASTC_LDR),
            yesNo(indexingFeatures.descriptorBindingSampledImageUpdateAfterBind));

        // Where buffers the CPU writes can end up. Memory that is not coherent has to be
        // flushed after writing, which plume does not do.
        VkPhysicalDeviceMemoryProperties memory = {};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

        std::string types;
        for (uint32_t i = 0; i < memory.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
        {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            std::string name;
            const auto add = [&name, flags](VkMemoryPropertyFlags flag, const char* text)
            {
                if ((flags & flag) != 0)
                    name += (name.empty() ? "" : "+") + std::string(text);
            };

            add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device");
            add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "visible");
            add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "coherent");
            add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "cached");
            add(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazy");
            add(VK_MEMORY_PROPERTY_PROTECTED_BIT, "protected");

            types += fmt::format("{}{}: {}", i == 0 ? "" : "; ", i, name.empty() ? "plain" : name);
        }

        LOGFN("Vulkan memory types: {}.", types);
    }

    // Written once, by CheckCreatedDevice(), before any texture is created.
    static bool s_decodeCompressedTextures = false;

    RenderFormat HostTextureFormat(RenderFormat format, BcDecode::EFormat& decode)
    {
        decode = BcDecode::EFormat::None;

        if (!s_decodeCompressedTextures || RenderFormatBlockWidth(format) <= 1)
            return format;

        switch (format)
        {
        case RenderFormat::BC1_TYPELESS:
        case RenderFormat::BC1_UNORM:
            decode = BcDecode::EFormat::BC1;
            break;

        case RenderFormat::BC1_UNORM_SRGB:
            decode = BcDecode::EFormat::BC1_SRGB;
            break;

        case RenderFormat::BC2_TYPELESS:
        case RenderFormat::BC2_UNORM:
            decode = BcDecode::EFormat::BC2;
            break;

        case RenderFormat::BC2_UNORM_SRGB:
            decode = BcDecode::EFormat::BC2_SRGB;
            break;

        case RenderFormat::BC3_TYPELESS:
        case RenderFormat::BC3_UNORM:
            decode = BcDecode::EFormat::BC3;
            break;

        case RenderFormat::BC3_UNORM_SRGB:
            decode = BcDecode::EFormat::BC3_SRGB;
            break;

        case RenderFormat::BC4_TYPELESS:
        case RenderFormat::BC4_UNORM:
            decode = BcDecode::EFormat::BC4;
            return RenderFormat::R8_UNORM;

        case RenderFormat::BC5_TYPELESS:
        case RenderFormat::BC5_UNORM:
            decode = BcDecode::EFormat::BC5;
            return RenderFormat::R8G8_UNORM;

        case RenderFormat::BC7_TYPELESS:
        case RenderFormat::BC7_UNORM:
            decode = BcDecode::EFormat::BC7;
            break;

        case RenderFormat::BC7_UNORM_SRGB:
            decode = BcDecode::EFormat::BC7_SRGB;
            break;

        default:
        {
            // Signed and floating-point blocks (BC4/BC5 SNORM, BC6H) have no plain format
            // here to unpack to. Mods are the only place they could come from.
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                LOGFN_WARNING("A texture in a block-compressed format this GPU cannot sample, and that is not unpacked (format {}), will not show.", uint32_t(format));

            return format;
        }
        }

        // The sRGB forms unpack to the same plain format; the sRGB curve is applied to the
        // colours while they are unpacked (BcDecode), so the picture keeps its brightness.
        return RenderFormat::R8G8B8A8_UNORM;
    }

    bool CheckCreatedDevice(RenderDevice* device)
    {
        auto* vulkanDevice = static_cast<VulkanDevice*>(device);
        const RenderDeviceCapabilities& capabilities = device->getCapabilities();

        LogDeviceDetails(vulkanDevice->physicalDevice, vulkanDevice->physicalDeviceProperties);

        // Block-compressed textures: Adreno samples them, most other phone GPUs do not.
        {
            VkPhysicalDeviceFeatures features = {};
            vkGetPhysicalDeviceFeatures(vulkanDevice->physicalDevice, &features);

            bool bcUsable = features.textureCompressionBC;
            if (bcUsable)
            {
                // The feature bit is a promise; the formats answer whether the textures the
                // game uses are really sampled (with linear filtering). A driver that keeps
                // only part of the promise would otherwise hand out textures that show
                // nothing: unpack them instead, exactly as on a GPU without the feature.
                static constexpr VkFormat BcFormats[] =
                {
                    VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                    VK_FORMAT_BC2_UNORM_BLOCK,
                    VK_FORMAT_BC3_UNORM_BLOCK,
                };

                for (VkFormat bcFormat : BcFormats)
                {
                    VkFormatProperties formatProperties = {};
                    vkGetPhysicalDeviceFormatProperties(vulkanDevice->physicalDevice, bcFormat, &formatProperties);

                    static constexpr VkFormatFeatureFlags Needed =
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

                    if ((formatProperties.optimalTilingFeatures & Needed) != Needed)
                    {
                        LOGFN_WARNING("Vulkan format {} is not sampled with linear filtering although the driver claims "
                            "block-compressed textures (features {:#X}); they are unpacked when they are loaded instead.",
                            uint32_t(bcFormat), uint32_t(formatProperties.optimalTilingFeatures));
                        bcUsable = false;
                    }
                }
            }

            s_decodeCompressedTextures = !bcUsable || Config::DecodeCompressedTextures;
            if (!features.textureCompressionBC)
            {
                LOGN("Textures: this GPU cannot sample block-compressed (BC, DXT) textures. "
                     "They are unpacked when they are loaded, which takes four to eight times the memory.");
            }
            else if (!bcUsable)
            {
                LOGN("Textures: the driver cannot sample every block-compressed (BC, DXT) texture format the game needs. "
                     "They are unpacked when they are loaded, which takes four to eight times the memory.");
            }
            else if (s_decodeCompressedTextures)
            {
                LOGN("Textures: block-compressed textures are unpacked when they are loaded, as Video.DecodeCompressedTextures in config.toml asks. This GPU can sample them.");
            }
        }

        // A system driver that never saw Vulkan 1.2 is a decade of phone-driver bugs away
        // from the ones the game is tested with. Nothing is enforced - some of them work -
        // but when the picture is wrong on such a driver, it is the first suspect. The
        // bundled Turnip driver (the Auto setting) is newer than all of them on Adreno.
        if (!AndroidIsCustomVulkanDriverActive() &&
            vulkanDevice->physicalDeviceProperties.apiVersion < VulkanRequirements::MakeVersion(1, 2))
        {
            LOGFN_WARNING("The system Vulkan driver is Vulkan {}.{} only. Old system drivers are known to refuse the game's "
                "pipelines or to render a wrong picture; if either happens, set Vulkan driver to Auto or Bundled in the "
                "launcher, which uses the bundled Turnip driver on Adreno GPUs.",
                VulkanRequirements::FormatVersion(vulkanDevice->physicalDeviceProperties.apiVersion),
                vulkanDevice->physicalDeviceProperties.apiVersion & 0xFFF);
        }

        // CheckDeviceRequirements() found the features in the driver. Whether plume switched
        // them on is another matter: it goes by the names of the extensions they began as,
        // which a driver need not list once they are part of its Vulkan version. Whether
        // the game draws correctly without is up to the driver then, so this is said and
        // the game goes on.
        if (!capabilities.descriptorIndexing)
        {
            LOGN_ERROR("The renderer did not switch on descriptor indexing: the driver has it, but does not list " VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME
                       ". Textures may be missing from the picture.");
        }

        if (!capabilities.bufferDeviceAddress)
        {
            LOGN_ERROR("The renderer did not switch on buffer device address: the driver has it, but does not list " VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
                       ". The picture may be wrong or empty.");
        }

        // The address of a buffer is asked for by the name the command has from Vulkan 1.2
        // on. An older driver has it under the name of the extension, and what the loader
        // hands out for either name before there is a device need not be the driver's own
        // command. Asked of the device, it is.
        auto getAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(vulkanDevice->vk, "vkGetBufferDeviceAddress"));
        const char* name = "vkGetBufferDeviceAddress";
        if (getAddress == nullptr)
        {
            getAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(vulkanDevice->vk, "vkGetBufferDeviceAddressKHR"));
            name = "vkGetBufferDeviceAddressKHR";
        }

        if (getAddress == nullptr)
        {
            // Without it no shader of the game finds its constants.
            s_missingDeviceFeature = "buffer device address (the driver has no command that tells the address of a buffer)";
            LOGFN_ERROR("This Vulkan driver cannot run the game. It lacks: {}.", s_missingDeviceFeature);
            return false;
        }

        vkGetBufferDeviceAddress = getAddress;
        LOGFN("Vulkan: buffer addresses come from the driver's {}.", name);
        return true;
    }

    void CheckFramebufferSizes(uint32_t colourWidth, uint32_t colourHeight, uint32_t depthWidth, uint32_t depthHeight)
    {
        // Only ever called by the thread that renders.
        static uint32_t s_reported = 0;

        // A depth buffer larger than the colour target is within the rules.
        if ((depthWidth >= colourWidth && depthHeight >= colourHeight) || s_reported >= 4)
            return;

        s_reported++;

        LOGFN_WARNING("A frame buffer combines a {}x{} colour target with a {}x{} depth buffer. Vulkan does not allow the depth buffer "
                      "to be the smaller one: this driver may draw nothing into such a frame buffer (Video.ResolutionScale is {:.2f}).",
            colourWidth, colourHeight, depthWidth, depthHeight, Config::ResolutionScale.Value);
    }

    // The frame written to the log draw by draw. All of this belongs to the thread that renders.
    static constexpr uint32_t LoggedDrawLimit = 48;
    static constexpr Uint64 LoggedFrameTimesMs[] = { 20 * 1000, 120 * 1000 };
    static bool s_frameLogged = false;
    static uint32_t s_loggedDraws = 0;
    static uint32_t s_loggedFrames = 0;
    static Uint64 s_firstFrameMs = 0;

    bool IsFrameLogged()
    {
        return s_frameLogged;
    }

    void LogDraw(const std::string& description)
    {
        if (!s_frameLogged)
            return;

        if (s_loggedDraws < LoggedDrawLimit)
            LOGFN("[frame] draw {}: {}", s_loggedDraws + 1, description);

        s_loggedDraws++;
    }

    bool CountFrame(uint32_t draws, uint32_t skippedDraws, uint32_t framebufferChanges)
    {
        // Only ever called by the thread that renders.
        static constexpr Uint64 FirstReportMs = 15 * 1000;
        static constexpr Uint64 ReportIntervalMs = 60 * 1000;

        const Uint64 frameEndMs = SDL_GetTicks();
        if (s_firstFrameMs == 0)
            s_firstFrameMs = frameEndMs;

        // The frame that just ended was the one written out.
        if (s_frameLogged)
        {
            s_frameLogged = false;
            LOGFN("[frame] end of the frame: {} draws{}.", s_loggedDraws,
                s_loggedDraws > LoggedDrawLimit ? fmt::format(", of which the first {} are listed", LoggedDrawLimit) : std::string());
        }

        bool logNextFrame = false;
        if (s_loggedFrames < std::size(LoggedFrameTimesMs) && frameEndMs - s_firstFrameMs >= LoggedFrameTimesMs[s_loggedFrames])
        {
            s_loggedFrames++;
            s_loggedDraws = 0;
            s_frameLogged = true;
            logNextFrame = true;
        }

        static Uint64 s_windowStartMs = 0;
        static Uint64 s_windowLengthMs = FirstReportMs;
        static uint64_t s_frames = 0;
        static uint64_t s_draws = 0;
        static uint64_t s_skippedDraws = 0;
        static uint64_t s_framebufferChanges = 0;
        static uint32_t s_refusedAtLastReport = 0;

        const Uint64 now = SDL_GetTicks();
        if (s_windowStartMs == 0)
            s_windowStartMs = now;

        s_frames++;
        s_draws += draws;
        s_skippedDraws += skippedDraws;
        s_framebufferChanges += framebufferChanges;

        const Uint64 elapsed = now - s_windowStartMs;
        if (elapsed < s_windowLengthMs)
            return logNextFrame;

        const double seconds = double(elapsed) / 1000.0;
        const double frames = double(s_frames);
        const uint32_t refused = GetRefusedPipelineCount();

        LOGFN("[render] {:.0f} s: {:.1f} frames per second; per frame {:.0f} draws ({:.0f} of them left out: the driver refused their pipeline) "
              "and {:.0f} changes of frame buffer; pipelines the driver refused: {} ({} since the last line).",
            seconds, frames / seconds, double(s_draws) / frames, double(s_skippedDraws) / frames, double(s_framebufferChanges) / frames,
            refused, refused - s_refusedAtLastReport);

        // Frames have been drawn for a while: the sustained checkpoint of render health.
        NoteRenderHealth(true);

        s_refusedAtLastReport = refused;
        s_windowStartMs = now;
        s_windowLengthMs = ReportIntervalMs;
        s_frames = 0;
        s_draws = 0;
        s_skippedDraws = 0;
        s_framebufferChanges = 0;

        return logNextFrame;
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
