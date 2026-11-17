#pragma once

#ifdef __ANDROID__

// Android-specific parts of the Vulkan presentation path, kept out of video.cpp.
//
// Android destroys the window surface whenever the app leaves the foreground and hands
// out a brand new one when it returns. A VkSurfaceKHR is bound to the ANativeWindow it was
// created for, so the surface (and with it the swap chain) has to be recreated on the new
// window; until one exists, frames are rendered but not presented.

struct SDL_Window;

#include <cstdint>
#include <string>

#include <plume_render_interface_types.h>

#include "bc_decode.h"

namespace plume
{
    struct RenderDevice;
    struct RenderInterface;
    struct RenderPipeline;
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

    // Render recovery's failure notes (os/android/vulkan_driver_android.h), for callers
    // that only know this header. `reason` is one of "requirements" (the driver lacks
    // features the game needs), "device" (the renderer could not be created) or
    // "blackscreen" (the game runs, but no frame reaches the screen).
    void NoteRenderFailed(const char* reason);

    // Call once per frame from the swap-chain check, while a window exists and the app is
    // in the foreground. If the game keeps running but no frame becomes presentable for a
    // long while (a driver that never puts anything on the screen), the driver path is
    // noted as broken once: the next launch runs the other driver.
    void WatchdogFrameTick();

    // Asks the driver, before a device is created on it, for what the renderer cannot do
    // without (see gpu/vulkan_requirements.h) and writes the answer to the log. Returns
    // false if something is missing: GetMissingDeviceFeature() then says what, for the
    // message the player gets instead of a renderer that fails further down the line.
    bool CheckDeviceRequirements(plume::RenderInterface* renderInterface);
    const std::string& GetMissingDeviceFeature();

    // Call right after the device was created. Looks at what plume made of the driver:
    //
    // - plume switches descriptor indexing and buffer device address on by the names of
    //   the extensions they began as. A driver need not list those names once the features
    //   are part of its Vulkan version, and then they stay off, although the check above
    //   found them. What the game draws then is up to the driver; the log is told.
    // - The command that tells the address of a buffer is asked of the device itself, under
    //   its name from Vulkan 1.2 or the one of the extension, whichever the driver has.
    //   Should it have neither, this returns false and GetMissingDeviceFeature() says so.
    //
    // Also writes the driver's limits, features and kinds of memory to the log, and finds
    // out whether block-compressed textures have to be unpacked (HostTextureFormat()).
    bool CheckCreatedDevice(plume::RenderDevice* device);

    // Call after the device was created and before the first shader is. On a driver
    // without 64-bit integers in shaders, makes every shader reach it rewritten to do
    // without them (see gpu/vulkan_int64_lowering.h), and says so in the log.
    // Video.ShadersWithoutInt64 = true in config.toml does the same on any driver, to tell
    // a fault of the rewrite from one of a driver.
    void InitShaderRewrite(plume::RenderDevice* device);

    // The format to create a texture in whose data comes in `format`.
    //
    // On a GPU that samples block-compressed (BC, DXT) textures this is `format` itself and
    // `decode` is EFormat::None. Most phone GPUs other than Adreno cannot; there the result
    // is the plain format the data unpacks to, and `decode` says what it is unpacked from
    // (gpu/bc_decode.h) when it is uploaded. Video.DecodeCompressedTextures = true in
    // config.toml does this on any GPU. Valid once CheckCreatedDevice() has run.
    plume::RenderFormat HostTextureFormat(plume::RenderFormat format, BcDecode::EFormat& decode);

    // Whether the driver actually created a graphics pipeline. plume returns an object
    // either way and only writes the driver's error to stderr.
    bool IsPipelineUsable(const plume::RenderPipeline* pipeline);

    // Counts a pipeline the driver refused and returns how many that makes; and how many
    // there have been. Any thread may call these.
    uint32_t CountRefusedPipeline();
    uint32_t GetRefusedPipelineCount();

    // Call when a frame buffer is put together from a colour target and a depth buffer.
    // Vulkan wants every part of a frame buffer to be at least as large as the frame
    // buffer, which plume sizes by the colour target. Desktop drivers and Turnip draw into
    // a smaller depth buffer regardless; a driver that goes by the rules may draw nothing.
    // The depth buffer of the game is kept large enough (CoverBackBuffer() in video.cpp);
    // should another one turn up that is not, the log is told the first few times.
    void CheckFramebufferSizes(uint32_t colourWidth, uint32_t colourHeight, uint32_t depthWidth, uint32_t depthHeight);

    // Call at the end of every frame, from the thread that renders, with what the frame
    // took. Every minute, and once shortly after the start, a line that starts with
    // [render] tells the log how the game is drawing: without it a report of a wrong
    // picture or a low frame rate says nothing about where to look.
    //
    // Twice in a run, 20 seconds and 2 minutes in, it returns true: the frame that follows
    // is one to write to the log draw by draw (IsFrameLogged() is true while it lasts, and
    // LogDraw() takes a line for each draw, of which the first few dozen are kept).
    bool CountFrame(uint32_t draws, uint32_t skippedDraws, uint32_t framebufferChanges);
    bool IsFrameLogged();
    void LogDraw(const std::string& description);

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
