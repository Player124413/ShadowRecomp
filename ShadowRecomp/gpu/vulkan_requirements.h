#pragma once

#include <cstdint>
#include <string>

// What the renderer cannot do without, checked against what a Vulkan driver reports.
//
// The renderer keeps all textures and all samplers in two large descriptor arrays that the
// shaders index at run time. That takes the descriptor indexing features named below: an
// extension of Vulkan 1.1 (VK_EXT_descriptor_indexing), part of Vulkan 1.2, and optional in
// both. Everything else the renderer uses is Vulkan 1.1 or an extension it can do without,
// so the version of the driver does not matter beyond 1.1; these features do.
namespace VulkanRequirements
{
    struct DeviceReport
    {
        uint32_t apiVersion = 0;                  // VkPhysicalDeviceProperties::apiVersion.
        bool descriptorIndexingExtension = false; // The driver offers VK_EXT_descriptor_indexing.

        // Whether the driver could be asked for the features at all: only where it has the
        // extension or Vulkan 1.2. The features are false otherwise.
        bool featuresQueried = false;
        bool runtimeDescriptorArray = false;
        bool descriptorBindingPartiallyBound = false;
        bool descriptorBindingVariableDescriptorCount = false;
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
    // the player gets, or an empty string if it lacks nothing.
    inline std::string FindMissingFeature(const DeviceReport& report)
    {
        if (report.apiVersion < MakeVersion(1, 1))
            return "Vulkan 1.1 (this driver supports Vulkan " + FormatVersion(report.apiVersion) + ")";

        if (!report.featuresQueried)
            return "descriptor indexing (this Vulkan " + FormatVersion(report.apiVersion) + " driver has no VK_EXT_descriptor_indexing)";

        std::string missing;
        const auto require = [&missing](bool supported, const char* name)
        {
            if (!supported)
                missing += (missing.empty() ? "" : ", ") + std::string(name);
        };

        require(report.runtimeDescriptorArray, "runtimeDescriptorArray");
        require(report.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
        require(report.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");

        if (!missing.empty())
            return "descriptor indexing (this driver lacks " + missing + ")";

        return {};
    }
}
