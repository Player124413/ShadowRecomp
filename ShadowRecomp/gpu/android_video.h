#pragma once

#ifdef __ANDROID__

// Android-specific parts of the Vulkan presentation path, kept out of video.cpp.
//
// Android destroys the window surface whenever the app leaves the foreground and hands
// out a brand new one when it returns. A VkSurfaceKHR is bound to the ANativeWindow it was
// created for, so the surface (and with it the swap chain) has to be recreated on the new
// window; until one exists, frames are rendered but not presented.

struct SDL_Window;

namespace plume
{
    struct RenderDevice;
    struct RenderSwapChain;
}

namespace AndroidVideo
{
    enum class ESurfaceState
    {
        Ready,       // The swap chain's surface is the window on screen.
        Replaced,    // Android handed out a new window; the surface must be recreated.
        Unavailable  // No window (app in the background): nothing can be presented.
    };

    // Cheap; meant to be called once per frame before the swap chain is used.
    ESurfaceState GetSurfaceState(plume::RenderSwapChain* swapChain, SDL_Window* window);

    // Recreates the Vulkan surface on the window currently on screen. The caller must have
    // waited for the GPU and released everything that references swap chain images, and
    // must rebuild the swap chain afterwards. Returns false if the old surface was kept.
    bool RecreateSurface(plume::RenderSwapChain* swapChain, SDL_Window* window);

    // Call once per presented frame while the swap chain is usable. Confirms to the driver
    // loader that Vulkan startup succeeded and feeds the hang watchdog.
    void OnFramePresentable();

    // Writes to the log which of the texture formats the renderer creates this GPU driver
    // supports. Nothing is enforced: unlike on desktop these vary between mobile GPUs, and
    // the list is what makes a "textures are broken on my phone" report actionable.
    void LogDeviceSupport(plume::RenderDevice* device);
}

#endif
