package dev.shadowrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;

/** What the native side needs to pick a graphics preset: the chip, the cores, the RAM and the ABI. */
final class DeviceInfo {
    private DeviceInfo() {
    }

    static String soc() {
        String soc = Build.SOC_MODEL;
        if (soc == null || soc.isEmpty() || "unknown".equals(soc)) soc = Build.HARDWARE;
        return soc == null ? "unknown" : soc.replace(' ', '_');
    }

    static String gpu() {
        String g = Build.HARDWARE;
        return g == null ? "unknown" : g.replace(' ', '_');
    }

    static int cores() {
        return Runtime.getRuntime().availableProcessors();
    }

    static int ramMb(Context context) {
        ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (am == null) return 0;
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(info);
        return (int) (info.totalMem / (1024 * 1024));
    }

    /** key=value;key=value - parsed by plat::android_set_info() and used by perf.cpp. */
    static String report(Context context) {
        String abi = Build.SUPPORTED_64_BIT_ABIS.length > 0 ? Build.SUPPORTED_64_BIT_ABIS[0] : "unknown";
        return "soc=" + soc() + ";cores=" + cores() + ";ram_mb=" + ramMb(context) + ";gpu=" + gpu()
                + ";abi=" + abi + ";sdk=" + Build.VERSION.SDK_INT;
    }
}
