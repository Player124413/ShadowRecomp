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

// Render recovery: the crash recovery above covers a driver that takes the process down;
// this covers a driver that stays up but cannot draw the game - one that refuses to create
// the game's pipelines en masse, which is how an old or exotic system driver fails.
//
// When the renderer reports that (AndroidNoteBrokenRenderDriver), a marker file records
// which of the two driver paths - the system one or a custom one - failed, and the next
// launch runs the other path once, without touching the configured selection. While the
// other path keeps drawing correctly it stays in use (the launcher shows a notice saying
// so, with a way to insist on the configured selection); a run that never proved itself
// counts as failed, so neither path is forced forever, and when both have failed the
// configured selection simply runs and the notice asks for the log.
//
// AndroidNoteRenderHealthy is called at the render-health checkpoints of a run that is
// drawing correctly: an early one when the pipelines of the caches are created
// (sustained=false) and the periodic [render] reports (sustained=true). Only a sustained
// proof clears an override that is still on trial; a healthy configured selection clears
// the marker at either checkpoint (a driver update may have fixed it).
//
// Both are no-ops before the renderer exists and may be called from any thread.
void AndroidNoteBrokenRenderDriver(uint32_t refusedPipelineCount);
void AndroidNoteRenderHealthy(bool sustained);

// dlopen() replacement compiled into plume. C linkage: it stands in for a libc symbol.
extern "C" void *ShadowRecomp_AndroidVulkanDlopen(const char *filename, int flags);
