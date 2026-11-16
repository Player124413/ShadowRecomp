#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Rewrites a SPIR-V shader so that it no longer needs 64-bit integers.
//
// The shaders of the game do not get their constants through descriptors. They are handed
// the address of the buffer that holds them, a 64-bit number in the push constants, add an
// offset to it and read from there (vk::RawBufferLoad, see gpu/shader/shader_common.h).
// That arithmetic is all they use 64-bit integers for, and it is enough to make a driver
// without the shaderInt64 feature refuse every one of them. Many phones have such a
// driver: reading from an address is a feature of its own (bufferDeviceAddress), which
// they do have.
//
// Lower() expresses the same arithmetic with pairs of 32-bit numbers, the way GLSL shaders
// written for such drivers do (GL_EXT_buffer_reference_uvec2):
//
//   - a 64-bit integer becomes a vector of two 32-bit ones, low half first. That is how a
//     little-endian 64-bit number lies in the push constants, so nothing changes for the
//     code that fills them in;
//   - a 64-bit addition becomes two 32-bit additions and a carry;
//   - the conversion of the number into a pointer takes the vector instead, which SPIR-V
//     allows for exactly this purpose.
//
// The pass knows what every operand of every core SPIR-V instruction is, so it finds each
// use of a 64-bit value. It rewrites the uses the shaders make (see the .cpp for the list)
// and turns the whole module down if it meets any other: it never hands back a module it
// only partly understood.
//
// Nothing here is specific to a platform or to Vulkan headers. The tests run it on the
// build machine over every shader the game ships (tools/android/tests).
namespace SpirvInt64Lowering
{
    enum class EResult
    {
        Unchanged, // The module uses no 64-bit integers. `output` and `error` are untouched.
        Lowered,   // `output` holds the rewritten module.
        Failed     // Not rewritten; `error` says why. `output` is untouched.
    };

    // `words` need not outlive the call. `output` may not alias the input.
    EResult Lower(const uint32_t* words, size_t wordCount, std::vector<uint32_t>& output, std::string& error);
}
