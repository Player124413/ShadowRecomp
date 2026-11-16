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

static DeviceReport Device(uint32_t major, uint32_t minor, bool extension, bool queried, bool runtimeArray, bool partiallyBound, bool variableCount)
{
    DeviceReport report;
    report.apiVersion = MakeVersion(major, minor) | 131; // Some patch version.
    report.descriptorIndexingExtension = extension;
    report.featuresQueried = queried;
    report.runtimeDescriptorArray = runtimeArray;
    report.descriptorBindingPartiallyBound = partiallyBound;
    report.descriptorBindingVariableDescriptorCount = variableCount;
    return report;
}

int main()
{
    Check(MakeVersion(1, 1) == 0x00401000 && MakeVersion(1, 2) == 0x00402000, "versions are encoded as Vulkan encodes them");
    Check(FormatVersion(MakeVersion(1, 3) | 275) == "1.3", "the patch number is left out", FormatVersion(MakeVersion(1, 3) | 275));

    // Accepted: any driver from 1.1 on that has the three features.
    Check(FindMissingFeature(Device(1, 1, true, true, true, true, true)).empty(), "Vulkan 1.1 with the extension is accepted");
    Check(FindMissingFeature(Device(1, 2, true, true, true, true, true)).empty(), "Vulkan 1.2 is accepted");
    Check(FindMissingFeature(Device(1, 3, true, true, true, true, true)).empty(), "Vulkan 1.3 is accepted");
    Check(FindMissingFeature(Device(1, 2, false, true, true, true, true)).empty(), "Vulkan 1.2 that no longer lists the extension is accepted");

    // Turned down, each with its own reason.
    std::string missing = FindMissingFeature(Device(1, 0, false, false, false, false, false));
    Check(missing == "Vulkan 1.1 (this driver supports Vulkan 1.0)", "Vulkan 1.0 is turned down", missing);

    // A 1.0 driver is turned down for its version whatever else it claims.
    missing = FindMissingFeature(Device(1, 0, true, true, true, true, true));
    Check(missing.rfind("Vulkan 1.1", 0) == 0, "Vulkan 1.0 with every feature is still turned down", missing);

    missing = FindMissingFeature(Device(1, 1, false, false, false, false, false));
    Check(missing == "descriptor indexing (this Vulkan 1.1 driver has no VK_EXT_descriptor_indexing)", "Vulkan 1.1 without the extension is turned down", missing);

    missing = FindMissingFeature(Device(1, 1, true, true, false, true, true));
    Check(missing == "descriptor indexing (this driver lacks runtimeDescriptorArray)", "one missing feature is named", missing);

    missing = FindMissingFeature(Device(1, 2, true, true, true, false, true));
    Check(missing == "descriptor indexing (this driver lacks descriptorBindingPartiallyBound)", "also on Vulkan 1.2", missing);

    missing = FindMissingFeature(Device(1, 1, true, true, true, true, false));
    Check(missing == "descriptor indexing (this driver lacks descriptorBindingVariableDescriptorCount)", "the third feature counts as well", missing);

    missing = FindMissingFeature(Device(1, 1, true, true, false, false, false));
    Check(missing == "descriptor indexing (this driver lacks runtimeDescriptorArray, descriptorBindingPartiallyBound, descriptorBindingVariableDescriptorCount)",
        "several missing features are all named", missing);

    // The extension by its name alone is not enough: the features are what is used.
    missing = FindMissingFeature(Device(1, 1, true, false, false, false, false));
    Check(!missing.empty(), "an extension whose features could not be asked for is not accepted", missing);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
