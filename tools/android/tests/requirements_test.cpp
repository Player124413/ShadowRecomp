// Host test of gpu/vulkan_requirements.h: which Vulkan drivers the game accepts, and what it
// tells the player about the ones it does not.
#include <gpu/vulkan_requirements.h>

#include <cstdio>
#include <string>

using namespace VulkanRequirements;

static int g_checks = 0;
static int g_failures = 0;

static void Check(bool ok, const char* what, const std::string& got = {})
{
    g_checks++;
    if (!ok)
    {
        g_failures++;
        printf("FAIL %s%s%s\n", what, got.empty() ? "" : ": ", got.c_str());
    }
}

// A driver of the given version that has everything the game needs.
static DeviceReport Complete(uint32_t major, uint32_t minor)
{
    DeviceReport report;
    report.apiVersion = MakeVersion(major, minor) | 131; // Some patch version.
    report.shaderInt64 = true;
    report.descriptorIndexingExtension = true;
    report.runtimeDescriptorArray = true;
    report.descriptorBindingPartiallyBound = true;
    report.descriptorBindingVariableDescriptorCount = true;
    report.bufferDeviceAddressExtension = true;
    report.bufferDeviceAddress = true;
    return report;
}

int main()
{
    Check(MakeVersion(1, 1) == 0x00401000 && MakeVersion(1, 2) == 0x00402000, "versions are encoded as Vulkan encodes them");
    Check(FormatVersion(MakeVersion(1, 3) | 275) == "1.3", "the patch number is left out", FormatVersion(MakeVersion(1, 3) | 275));

    // Accepted: any driver from 1.1 on that has all of it.
    Check(FindMissingFeature(Complete(1, 1)).empty(), "Vulkan 1.1 with both extensions is accepted", FindMissingFeature(Complete(1, 1)));
    Check(FindMissingFeature(Complete(1, 2)).empty(), "Vulkan 1.2 is accepted");
    Check(FindMissingFeature(Complete(1, 3)).empty(), "Vulkan 1.3 is accepted");

    // From 1.2 on the features are part of Vulkan: the extensions need not be listed.
    DeviceReport report = Complete(1, 2);
    report.descriptorIndexingExtension = false;
    report.bufferDeviceAddressExtension = false;
    Check(FindMissingFeature(report).empty(), "Vulkan 1.2 that no longer lists the extensions is accepted", FindMissingFeature(report));

    // Vulkan 1.0 is turned down for its version, whatever else it claims.
    report = Complete(1, 0);
    std::string missing = FindMissingFeature(report);
    Check(missing == "Vulkan 1.1 (this driver supports Vulkan 1.0)", "Vulkan 1.0 is turned down", missing);

    // Descriptor indexing.
    report = Complete(1, 1);
    report.descriptorIndexingExtension = false;
    report.runtimeDescriptorArray = report.descriptorBindingPartiallyBound = report.descriptorBindingVariableDescriptorCount = false;
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this Vulkan 1.1 driver has no VK_EXT_descriptor_indexing)", "Vulkan 1.1 without descriptor indexing is turned down", missing);

    report = Complete(1, 1);
    report.runtimeDescriptorArray = false;
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this driver lacks runtimeDescriptorArray)", "one missing feature is named", missing);

    report = Complete(1, 2);
    report.descriptorBindingPartiallyBound = false;
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this driver lacks descriptorBindingPartiallyBound)", "also on Vulkan 1.2", missing);

    report = Complete(1, 1);
    report.descriptorBindingVariableDescriptorCount = false;
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this driver lacks descriptorBindingVariableDescriptorCount)", "the third feature counts as well", missing);

    report = Complete(1, 1);
    report.runtimeDescriptorArray = report.descriptorBindingPartiallyBound = report.descriptorBindingVariableDescriptorCount = false;
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this driver lacks runtimeDescriptorArray, descriptorBindingPartiallyBound, descriptorBindingVariableDescriptorCount)",
        "an extension without its features is not enough, and all of them are named", missing);

    // Buffer device address.
    report = Complete(1, 1);
    report.bufferDeviceAddressExtension = false;
    report.bufferDeviceAddress = false;
    missing = FindMissingFeature(report);
    Check(missing == "buffer device address (this Vulkan 1.1 driver has no VK_KHR_buffer_device_address)", "Vulkan 1.1 without buffer device address is turned down", missing);

    report = Complete(1, 1);
    report.bufferDeviceAddress = false;
    missing = FindMissingFeature(report);
    Check(missing == "buffer device address (this driver lacks bufferDeviceAddress)", "the extension without the feature is not enough", missing);

    report = Complete(1, 3);
    report.bufferDeviceAddressExtension = false;
    report.bufferDeviceAddress = false;
    missing = FindMissingFeature(report);
    Check(missing == "buffer device address (this driver lacks bufferDeviceAddress)", "nor is a newer version without the feature", missing);

    // 64-bit integers are not required: the shaders are rewritten for a driver without.
    report = Complete(1, 1);
    report.shaderInt64 = false;
    missing = FindMissingFeature(report);
    Check(missing.empty(), "Vulkan 1.1 without shaderInt64 is accepted", missing);

    report = Complete(1, 3);
    report.shaderInt64 = false;
    missing = FindMissingFeature(report);
    Check(missing.empty(), "and so is a newer version without it", missing);

    // Everything that is missing is named at once.
    report = DeviceReport{};
    report.apiVersion = MakeVersion(1, 1);
    missing = FindMissingFeature(report);
    Check(missing == "descriptor indexing (this Vulkan 1.1 driver has no VK_EXT_descriptor_indexing); "
                     "buffer device address (this Vulkan 1.1 driver has no VK_KHR_buffer_device_address)",
        "a bare Vulkan 1.1 driver is told both", missing);

    report.shaderInt64 = true;
    Check(FindMissingFeature(report) == missing, "which shaderInt64 does not make up for");

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
