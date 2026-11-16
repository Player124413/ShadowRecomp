// Stand-in for the DirectX Shader Compiler on Android.
//
// XShaderRecomp can translate an Xbox shader to HLSL at run time and compile it with
// libdxcompiler, as a fallback for shaders missing from the precompiled shader cache. No
// Android build of that compiler exists, and linking the desktop one would make
// libmain.so depend on a library the device cannot load. The Android build therefore
// never takes that path (see SHADOW_RECOMP_NO_RUNTIME_SHADER_COMPILER) and links this
// archive in place of the compiler, so the references XShaderRecomp still carries resolve.
// Any call that does get through reports failure instead of crashing.

#include <cstdint>

extern "C"
{
    static constexpr int32_t DxcNotImplemented = int32_t(0x80004001); // E_NOTIMPL

    __attribute__((visibility("hidden"))) int32_t DxcCreateInstance(const void* classId, const void* interfaceId, void** object)
    {
        (void)classId;
        (void)interfaceId;

        if (object != nullptr)
            *object = nullptr;

        return DxcNotImplemented;
    }

    __attribute__((visibility("hidden"))) int32_t DxcCreateInstance2(void* allocator, const void* classId, const void* interfaceId, void** object)
    {
        (void)allocator;
        (void)classId;
        (void)interfaceId;

        if (object != nullptr)
            *object = nullptr;

        return DxcNotImplemented;
    }
}
