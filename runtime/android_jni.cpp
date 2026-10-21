// Android JNI glue (compiles to nothing on any other platform).
//
// Aurora's window layer exports one JNI entry point of its own: AuroraSurface_nativeSetSurfaceReady, which the Java
// surface calls when the window really exists or is going away. Java resolves native methods by name at run time, so
// the linker cannot see that the C++ side is used and would drop it (and the object file holding it) from libmain.so.
// Referencing its address from a symbol that is kept keeps both.
#if defined(__ANDROID__)
#include <jni.h>

extern "C" {
void Java_dev_encounter_aurora_AuroraSurface_nativeSetSurfaceReady(JNIEnv*, jclass, jboolean);

#if defined(__clang__) || defined(__GNUC__)
// 'used': this pointer stays in the binary even with nothing reading it, so the symbol above must be resolved.
__attribute__((used))
#endif
void* shadow_aurora_surface_ready_hook =
    reinterpret_cast<void*>(&Java_dev_encounter_aurora_AuroraSurface_nativeSetSurfaceReady);
}
#endif
