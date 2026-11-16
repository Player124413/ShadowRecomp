#pragma once

#include <filesystem>

// Declared instead of including the Vulkan headers: what those declare depends on macros
// that plume sets before its own include of them, so they must not be pulled in from here
// first. These are the types behind VkDevice and VkPhysicalDeviceProperties.
struct VkDevice_T;
struct VkPhysicalDeviceProperties;

// Keeps the Vulkan driver's pipeline cache on disk between runs.
//
// A VkPipelineCache collects what the driver produces when it compiles the shaders of a
// pipeline, which is nearly all of the cost of creating one. Created from the data of an
// earlier run, it lets the driver skip that work. Desktop drivers keep such a cache on disk
// by themselves. The Android ones do not (neither the vendors' nor Mesa Turnip, whose disk
// cache is compiled out on Android), so without this every launch compiles every pipeline
// from scratch. The file is only built for Android for that reason; nothing in it is
// specific to Android.
//
// plume creates its pipelines without a cache and has no way of being given one.
// Initialize() therefore replaces volk's vkCreateGraphicsPipelines entry, which every
// pipeline creation of the process goes through, with a function that fills the cache in.
// The plume submodule stays untouched.
namespace VulkanPipelineCache
{
    // Creates the cache, from the data in `file` if this device's driver wrote it, and
    // starts using it. Call once, after the device exists and before the first pipeline is
    // created; no other thread may be creating pipelines at that point.
    void Initialize(VkDevice_T* device, const VkPhysicalDeviceProperties& deviceProperties, const std::filesystem::path& file);

    // Writes the cache to the file if the driver has added to it since it was last read or
    // written, and does next to nothing otherwise. May be called from any thread, also
    // while other threads create pipelines.
    void Save();

    // Tells that pipelines were created from the data read at startup without incident.
    //
    // Until then a marker file sits next to the cache. A run that finds the marker of the
    // run before it discards the cache instead of reading it: data that takes the driver
    // down must not be handed to it a second time.
    void ConfirmLoadedData();
}
