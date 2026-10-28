// Android JNI glue (compiles to nothing on any other platform).
//
// Aurora's window layer exports one JNI entry point of its own: AuroraSurface_nativeSetSurfaceReady, which the Java
// surface calls when the window really exists or is going away. Java resolves native methods by name at run time, so
// the linker cannot see that the C++ side is used and would drop it (and the object file holding it) from libmain.so.
// Referencing its address from a symbol that is kept keeps both.
#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>

#include "plat.h"   // plat::android_share_log, so the definition below is checked against its declaration

#include <mutex>

// Aurora's surface lock (third_party/aurora/lib/window.cpp, SurfaceLock) takes SDL's Android activity mutex around
// surface configuration and present, as SDL2 exported it. SDL3 does not: its shared library exports exactly the
// symbols listed in src/dynapi/SDL_dynapi.sym (a linker version script), and Android_LockActivityMutex is not one of
// them, so the app fails to link with "undefined symbol". Marking the definition visible does not help against a
// version script, which leaves one option: provide them here.
//
// What they guard here is our own mutex, not SDL's (SDL's is internal and out of reach). That still serialises
// Aurora's own surface work - the point of the lock - but it does not interlock with SDL's Java callbacks
// (surfaceCreated / surfaceDestroyed on the UI thread). Aurora reacts to those through SDL events and stops presenting
// when the surface is not ready, so the remaining window is narrow; it is a known, deliberate compromise.
namespace {
std::once_flag g_surface_mutex_once;
SDL_Mutex* g_surface_mutex = nullptr;

SDL_Mutex* surface_mutex() {
  std::call_once(g_surface_mutex_once, [] { g_surface_mutex = SDL_CreateMutex(); });
  return g_surface_mutex;
}
}  // namespace

extern "C" void Android_LockActivityMutex(void) {
  if (SDL_Mutex* mutex = surface_mutex()) SDL_LockMutex(mutex);
}
extern "C" void Android_UnlockActivityMutex(void) {
  if (SDL_Mutex* mutex = surface_mutex()) SDL_UnlockMutex(mutex);
}

// ---- native -> Java: the in-game menu's "Send the log" row ------------------------------------------------------------
// The only Java entry point the game itself calls. Everything it needs to do (find the newest log, hand it to the share
// sheet) is written in Java, where Android's APIs are: this only names the method.
//
// The class is taken from the running activity instead of FindClass(): on Android FindClass from a bare native thread
// searches the *system* class loader and does not find the app's own classes, while GetObjectClass(activity) is exactly
// the ShadowActivity the manifest declares.
namespace plat {
bool android_share_log() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) return false;
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) return false;
  bool ok = false;
  if (jclass cls = env->GetObjectClass(activity)) {
    if (jmethodID mid = env->GetMethodID(cls, "shareLatestLog", "()V")) {
      env->CallVoidMethod(activity, mid);
      ok = !env->ExceptionCheck();
      if (env->ExceptionCheck()) env->ExceptionClear();   // never let a Java error take the game down with it
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);   // SDL_GetAndroidActivity returns a local reference
  return ok;
}
}  // namespace plat

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
