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

    // Call after every frame handed to the swap chain. Notes the first one in the startup
    // timeline of log.txt: the end of the black screen the game starts on.
    void OnFramePresented();

    // Writes to the log which of the texture formats the renderer creates this GPU driver
    // supports. Nothing is enforced: unlike on desktop these vary between mobile GPUs, and
    // the list is what makes a "textures are broken on my phone" report actionable.
    void LogDeviceSupport(plume::RenderDevice* device);

    // The driver's pipeline cache, kept in the app's cache directory between runs (see
    // gpu/vulkan_pipeline_cache.h; Video.PipelineDiskCache = false in config.toml turns it
    // off). No Android driver keeps what it compiles by itself, and compiling is what makes
    // creating a pipeline slow.
    //
    // InitPipelineCache() must be called after the device was created and before the first
    // pipeline is. SavePipelineCache() writes the cache out if the driver has added to it
    // and costs next to nothing otherwise; any thread may call it.
    void InitPipelineCache(plume::RenderDevice* device);
    void SavePipelineCache();

    // Call when every pipeline listed in the pipeline caches has been created, which is
    // most of what the game will ever use. Saves the driver's cache, and counts as proof
    // that the cache data read at startup is sound. Any thread may call it.
    void OnCachedPipelinesCreated();
}

#endif
