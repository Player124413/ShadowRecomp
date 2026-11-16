#pragma once

#include <os/android/android_settings.h>

// Loads a custom Vulkan driver (Mesa Turnip, or a driver package the user imported)
// through libadrenotools instead of the device's own driver.
//
// The flow, in order:
//   1. AndroidSetVulkanDriverSelection() - the renderer passes the configured selection.
//   2. plume initializes volk, whose dlopen("libvulkan.so") is routed to
//      ShadowRecomp_AndroidVulkanDlopen() (see thirdparty/CMakeLists.txt). That provisions
//      the bundled drivers, processes driver_import/, picks the driver for this device and
//      returns the libvulkan instance that routes to it - or the system one.
//   3. AndroidMarkVulkanStartupSuccessful() - called once a device and a usable swapchain
//      exist. Until then a marker file records the attempt, so a driver that crashes or
//      hangs during startup is replaced by the system driver on the next launch instead of
//      leaving the app unbootable.

// Must be called before the Vulkan interface is created. Defaults to Auto/Auto.
void AndroidSetVulkanDriverSelection(EAndroidVulkanDriver driver, EAndroidRenderMode renderMode);

// dlopen() handle of the libvulkan.so instance bound to the selected custom driver, or
// nullptr when the system driver is used. Resolved once per process.
void *AndroidGetCustomVulkanLibrary();

// True when Vulkan was routed to a custom driver in this process.
bool AndroidIsCustomVulkanDriverActive();

// Clears the startup marker; see step 3 above. Safe to call every frame.
void AndroidMarkVulkanStartupSuccessful();

// dlopen() replacement compiled into plume. C linkage: it stands in for a libc symbol.
extern "C" void *ShadowRecomp_AndroidVulkanDlopen(const char *filename, int flags);
