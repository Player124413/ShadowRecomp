#ifdef __ANDROID__

#include "android_video.h"

#include <os/android/logger_android.h>
#include <os/android/lifecycle_android.h>
#include <os/android/vulkan_driver_android.h>
#include <os/logger.h>

#include <plume_vulkan.h>

#include <SDL3/SDL.h>
#include <android/native_window.h>

#include <mutex>

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
