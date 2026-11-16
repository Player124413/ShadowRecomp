#pragma once

#include <cstdint>

// Lets the game run on Vulkan drivers without 64-bit integers in shaders (the shaderInt64
// feature): every shader is rewritten to do without them on its way to the driver. What
// the rewrite is, and why it is possible at all, is told in gpu/spirv_int64_lowering.h.
//
// plume hands the SPIR-V the game was built with straight to vkCreateShaderModule and has
// no place to step in. Install() therefore replaces volk's vkCreateShaderModule entry,
// which every shader of the process goes through, as gpu/vulkan_pipeline_cache.h does for
// pipelines. The plume submodule stays untouched.
//
// Only built for Android: desktop drivers all have the feature. Nothing in it is specific
// to Android.
namespace VulkanInt64Lowering
{
    // Call once, on a device without shaderInt64, before the first shader is created; no
    // other thread may be creating shaders at that point. Afterwards shaders may be
    // created from any number of threads, as before.
    void Install();

    struct Counts
    {
        uint64_t rewritten = 0; // Shaders handed to the driver without their 64-bit integers.
        uint64_t unchanged = 0; // Shaders that had none.
        uint64_t failed = 0;    // Shaders that could not be rewritten; the driver got them as they were.
        uint64_t refused = 0;   // Of the rewritten ones: those the driver did not accept.
    };

    // What happened to the shaders created since Install(). All zero if it was never called.
    Counts GetCounts();
}
