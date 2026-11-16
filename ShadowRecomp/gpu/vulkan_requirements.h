#pragma once

#include <cstdint>
#include <string>

// What the renderer cannot do without, checked against what a Vulkan driver reports.
//
// - Descriptor indexing. All textures and all samplers live in two large descriptor arrays
//   that the shaders index at run time.
// - Buffer device address. The shaders do not get their constants through descriptors:
//   they are handed the address of the buffer that holds them and read from it
//   (vk::RawBufferLoad, see gpu/shader/shader_common.h).
//
// Both are extensions of Vulkan 1.1 (VK_EXT_descriptor_indexing,
// VK_KHR_buffer_device_address) that became part of Vulkan 1.2, and are optional in both.
// The renderer uses no command newer than Vulkan 1.1 otherwise, so it is these features
// that decide whether a driver will do, not its version.
//
// Those addresses are 64-bit integers, which the shaders are built to compute with. A
// driver need not have them (shaderInt64), and many a phone's does not: the shaders are
// then rewritten to do without (gpu/vulkan_int64_lowering.h), so that is asked for the
// log only.
namespace VulkanRequirements
{
    struct DeviceReport
    {
        uint32_t apiVersion = 0; // VkPhysicalDeviceProperties::apiVersion.

        bool shaderInt64 = false; // VkPhysicalDeviceFeatures::shaderInt64. Not required.

        // Whether the driver offers the extension, and what it says about its features. It
        // can only be asked about them where it has the extension or Vulkan 1.2; they stay
        // false otherwise.
        bool descriptorIndexingExtension = false;
        bool runtimeDescriptorArray = false;
        bool descriptorBindingPartiallyBound = false;
        bool descriptorBindingVariableDescriptorCount = false;

        bool bufferDeviceAddressExtension = false;
        bool bufferDeviceAddress = false;
    };

    inline constexpr uint32_t MakeVersion(uint32_t major, uint32_t minor)
    {
        return (major << 22) | (minor << 12); // VK_MAKE_API_VERSION(0, major, minor, 0)
    }

    inline std::string FormatVersion(uint32_t apiVersion)
    {
        return std::to_string((apiVersion >> 22) & 0x7F) + "." + std::to_string((apiVersion >> 12) & 0x3FF);
    }

    // Returns what a device lacks to run the game, in words for the log and for the message
    // the player gets, or an empty string if it lacks nothing. Everything that is missing
    // is named, not just the first thing found.
    inline std::string FindMissingFeature(const DeviceReport& report)
    {
        if (report.apiVersion < MakeVersion(1, 1))
            return "Vulkan 1.1 (this driver supports Vulkan " + FormatVersion(report.apiVersion) + ")";

        // A driver older than 1.2 has a feature of 1.2 only through the extension.
        const bool before12 = report.apiVersion < MakeVersion(1, 2);
        const std::string driver = "this Vulkan " + FormatVersion(report.apiVersion) + " driver";

        std::string missing;
        const auto add = [&missing](const std::string& what)
        {
            missing += (missing.empty() ? "" : "; ") + what;
        };

        if (before12 && !report.descriptorIndexingExtension)
        {
            add("descriptor indexing (" + driver + " has no VK_EXT_descriptor_indexing)");
        }
        else
        {
            std::string features;
            const auto require = [&features](bool supported, const char* name)
            {
                if (!supported)
                    features += (features.empty() ? "" : ", ") + std::string(name);
            };

            require(report.runtimeDescriptorArray, "runtimeDescriptorArray");
            require(report.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
            require(report.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");

            if (!features.empty())
                add("descriptor indexing (this driver lacks " + features + ")");
        }

        if (before12 && !report.bufferDeviceAddressExtension)
            add("buffer device address (" + driver + " has no VK_KHR_buffer_device_address)");
        else if (!report.bufferDeviceAddress)
            add("buffer device address (this driver lacks bufferDeviceAddress)");

        return missing;
    }
}
