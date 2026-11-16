#!/usr/bin/env python3
"""Runs shaders rewritten by gpu/spirv_int64_lowering.cpp on a real Vulkan driver.

    shader_execution_test.py <spirv_lower_tool> <vulkan_compute_harness> [--summary <file>]

spirv_lowering_test.py reads rewritten shaders and reasons about them. This test executes
them: compute shaders that find their data the way the game's shaders do, through 64-bit
addresses in their push constants, read from a buffer at addresses they work out, and
write what they found to another. The test knows what is in the first buffer and so what
has to end up in the second.

Two shaders are run:

  1. One built here, instruction by instruction, with every kind of arithmetic the rewrite
     handles. The addresses are handed to it with a bias it has to add back, chosen so
     that sums carry from the low half of the number into the high one, and offsets are
     negative numbers known only at run time.
  2. One written in HLSL in the forms the game's shaders use (gpu/shader/shader_common.h,
     the fixed-function shaders) and compiled by the compiler and with the flags of the
     build. Left out where that compiler does not run.

On a driver with 64-bit integers each is run as it is and rewritten, and both have to
give the same. On a driver without them, which is what the rewrite is for, only the
rewritten one can run.

The driver is whatever Vulkan implementation the machine has, or the library named by the
environment variable VULKAN_LIBRARY. Without one the test is skipped (exit code 77).
"""

import os
import subprocess
import sys
import tempfile

import spirv_lowering_test as lowering
from spirv_lowering_test import (MASK32, MASK64, OpAccessChain, OpBitcast, OpBitwiseAnd, OpBitwiseOr, OpBitwiseXor, OpBranch,
                                 OpBranchConditional, OpCompositeConstruct, OpCompositeExtract, OpConvertPtrToU, OpConvertUToPtr,
                                 OpCopyObject, OpFunction, OpFunctionCall, OpFunctionEnd, OpFunctionParameter, OpIAdd, OpIEqual,
                                 OpISub, OpLabel, OpLoad, OpNot, OpPhi, OpReturnValue, OpSConvert, OpSelectionMerge, OpStore,
                                 OpTypePointer, OpTypeStruct, OpUConvert, OpVariable, STORAGE_FUNCTION, STORAGE_PHYSICAL, check)

SKIPPED = 77

# What the harness subtracts from the two addresses before it pushes them. Adding the
# first back carries into the high half; so does the second, which is -32.
BIAS_INPUT = 0x00000001FFFFFFF0
BIAS_OUTPUT = 0xFFFFFFFFFFFFFFE0

# The input buffer: every word different, and a few with a meaning.
INPUT = [0xC0DE0000 + 0x101 * i for i in range(96)]
INPUT[1] = 0xFFFFFFF8          # -8: an offset that is negative
INPUT[2] = 0xFFFFFFFC          # A large unsigned number.
INPUT[3] = 1                   # What a branch depends on.
INPUT[6] = 12                  # A small offset.
POINTER_AT, POINTER_TO = 4, 24 # Words 4 and 5 hold the address of word 24.
INPUT[4] = INPUT[5] = 0


def build_shader():
    """The shader built here. Returns its words and what it has to write: (what, value)."""
    s = lowering.Scene(compute=True)
    b = s.b
    expected = []

    physical_int = b.declare(OpTypePointer, STORAGE_PHYSICAL, s.int)
    physical_ulong = b.declare(OpTypePointer, STORAGE_PHYSICAL, s.ulong)

    def wide(value):
        return b.constant(s.ulong, value & MASK32, (value >> 32) & MASK32)

    def add(left, right):
        return b.emit(OpIAdd, s.ulong, left, right)

    def sub(left, right):
        return b.emit(OpISub, s.ulong, left, right)

    pushed_input = s.load_address(0)
    pushed_output = s.load_address(1)
    base = add(pushed_input, wide(BIAS_INPUT))
    output = add(pushed_output, wide(BIAS_OUTPUT))

    def at(word):
        """The address of a word of the input, worked out the plain way."""
        return add(base, wide(4 * word))

    def read(address):
        return s.read(address)

    def write(what, value, want):
        pointer = b.emit(OpBitcast, s.physical_uint, add(output, wide(4 * len(expected))))
        b.statement(OpStore, pointer, value, 2, 4)  # Aligned 4
        expected.append((what, want & MASK32))

    def reads(what, address, word):
        write(what, read(address), INPUT[word])

    # The two bases themselves: everything below hangs on them.
    reads("a sum that carries into the high half", base, 0)
    reads("a small constant offset", at(7), 7)

    # Offsets known only at run time.
    negative = b.emit(OpLoad, s.int, b.emit(OpBitcast, physical_int, at(1)), 2, 4)
    negative_wide = b.emit(OpBitcast, s.ulong, b.emit(OpSConvert, s.long, negative))
    reads("a negative offset, sign-extended", add(at(12), negative_wide), 10)

    large = read(at(2))
    large_wide = b.emit(OpUConvert, s.ulong, large)
    # pushed + constant + 0xFFFFFFFC is word 11 only if the number was extended with zeroes.
    reads("a large offset, zero-extended",
          add(add(pushed_input, wide((BIAS_INPUT + 44 - INPUT[2]) & MASK64)), large_wide), 11)

    small_wide = b.emit(OpUConvert, s.ulong, read(at(6)))
    reads("a small offset from the buffer", add(at(32), small_wide), 35)
    reads("two offsets that are not constants, added", add(at(40), add(negative_wide, negative_wide)), 36)
    reads("an offset that is not a constant, subtracted", sub(at(35), negative_wide), 37)
    reads("a signed number extended as unsigned, and back",
          add(at(47), b.emit(OpSConvert, s.ulong, b.emit(OpUConvert, s.uint, b.emit(OpUConvert, s.ulong, negative)))), 45)

    # Subtraction.
    reads("a constant subtracted", sub(add(base, wide(0x100)), wide(0xD0)), 12)
    reads("a subtraction that borrows from the high half", sub(add(base, wide(0x300000000)), wide(0x2FFFFFFCC)), 13)
    distance = sub(output, base)
    reads("the distance between two addresses", add(sub(output, distance), wide(56)), 14)
    reads("a constant minus an address, and back", sub(wide(0x5000000000), sub(wide(0x5000000000 - 4 * 46), base)), 46)

    # Bit operations.
    mask = wide(0xA5A5A5A55A5A5A5A)
    scrambled = b.emit(OpBitwiseXor, s.ulong, base, mask)
    reads("exclusive or, twice", add(b.emit(OpBitwiseXor, s.ulong, scrambled, mask), wide(60)), 15)
    reads("not, twice", add(b.emit(OpNot, s.ulong, b.emit(OpNot, s.ulong, base)), wide(64)), 16)
    reads("or with nothing, and with everything",
          add(b.emit(OpBitwiseAnd, s.ulong, b.emit(OpBitwiseOr, s.ulong, base, wide(0)), wide(MASK64)), wide(68)), 17)

    # Down to 32 bits. The address itself is unknown here; the difference is not.
    low = b.emit(OpUConvert, s.uint, base)
    write("the low half of a sum", b.emit(OpISub, s.uint, b.emit(OpUConvert, s.uint, add(base, wide(0x7001234))), low), 0x7001234)
    signed_low = b.emit(OpSConvert, s.int, add(base, wide(0x1FFFFFFFF)))
    write("the low half as a signed number", b.emit(OpISub, s.uint, b.emit(OpBitcast, s.uint, signed_low), low), 0xFFFFFFFF)

    # Pointers and numbers.
    pointer = b.emit(OpBitcast, s.physical_uint, at(19))
    number = b.emit(OpConvertPtrToU, s.ulong, pointer)
    write("a pointer made into a number and back", b.emit(OpLoad, s.uint, b.emit(OpConvertUToPtr, s.physical_uint, add(number, wide(4))), 2, 4), INPUT[20])
    reads("a pointer reinterpreted as a number", add(b.emit(OpBitcast, s.ulong, pointer), wide(8)), 21)

    stored = b.emit(OpLoad, s.ulong, b.emit(OpBitcast, physical_ulong, at(POINTER_AT)), 2, 8)  # Aligned 8
    reads("an address read from the buffer", add(stored, wide(4)), POINTER_TO + 1)

    # The two halves, by hand.
    halves = b.emit(OpBitcast, s.pair, at(31))
    reads("an address as a vector of two", b.emit(OpBitcast, s.ulong, halves), 31)
    rebuilt = b.emit(OpCompositeConstruct, s.pair, b.emit(OpCompositeExtract, s.uint, halves, 0), b.emit(OpCompositeExtract, s.uint, halves, 1))
    reads("put together again from its halves", add(b.emit(OpBitcast, s.ulong, rebuilt), wide(4)), 32)

    # Values that travel: a variable, a structure, a copy, a call, a branch.
    function_pointer = b.declare(OpTypePointer, STORAGE_FUNCTION, s.ulong)
    variable = b.new_id()
    b.code = b.encode(OpVariable, function_pointer, variable, STORAGE_FUNCTION) + b.code  # Variables come first.
    b.statement(OpStore, variable, at(29))
    reads("through a variable", b.emit(OpLoad, s.ulong, variable), 29)

    record = b.declare(OpTypeStruct, s.uint, s.ulong)
    packed = b.emit(OpCompositeConstruct, record, large, at(30))
    reads("through a structure and a copy", b.emit(OpCopyObject, s.ulong, b.emit(OpCompositeExtract, s.ulong, packed, 1)), 30)

    helper_type = b.declare(lowering.OpTypeFunction, s.ulong, s.ulong)
    helper, parameter, helper_label, helper_sum = b.new_id(), b.new_id(), b.new_id(), b.new_id()
    helper_code = (b.encode(OpFunction, s.ulong, helper, 0, helper_type) + b.encode(OpFunctionParameter, s.ulong, parameter) +
                   b.encode(OpLabel, helper_label) + b.encode(OpIAdd, s.ulong, helper_sum, parameter, wide(4 * 28)) +
                   b.encode(OpReturnValue, helper_sum) + b.encode(OpFunctionEnd))
    reads("through a function", b.emit(OpFunctionCall, s.ulong, helper, base), 28)

    condition = b.emit(OpIEqual, s.bool, read(at(3)), b.constant(s.uint, 1))
    then_label, else_label, merge_label = b.new_id(), b.new_id(), b.new_id()
    b.statement(OpSelectionMerge, merge_label, 0)
    b.statement(OpBranchConditional, condition, then_label, else_label)
    b.statement(OpLabel, then_label)
    taken = at(26)
    b.statement(OpBranch, merge_label)
    b.statement(OpLabel, else_label)
    not_taken = at(27)
    b.statement(OpBranch, merge_label)
    b.statement(OpLabel, merge_label)
    chosen = b.emit(OpPhi, s.ulong, taken, then_label, not_taken, else_label)
    reads("through a branch", chosen, 26)

    # Arithmetic on constants alone, and with zero.
    reads("two constants added", sub(add(base, add(wide(0xFFFFFFFF), wide(0x99))), wide(0x100000000)), 38)
    reads("zero added and subtracted", sub(add(wide(0), add(at(39), wide(0))), wide(0)), 39)

    return b.words([helper_code]), expected


HLSL = """
struct PushConstants
{
    uint64_t Input;
    uint64_t Output;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// The forms the game's shaders are written in.
#define g_Word7             vk::RawBufferLoad<uint>(g_PushConstants.Input + 28)
#define g_Words8            vk::RawBufferLoad<uint2>(g_PushConstants.Input + 32)
#define REGISTER_ALIGNMENT  16
#define REG_U(cN)           vk::RawBufferLoad<uint4>(g_PushConstants.Input + (cN) * 16, REGISTER_ALIGNMENT)
#define LOAD_REG(addr)      vk::RawBufferLoad<uint4>((addr), REGISTER_ALIGNMENT)
#define LIGHT_BASE          3
#define LIGHT_SIZE_BYTES    32
#define LIGHT_WORD(i)       LOAD_REG(g_PushConstants.Input + (LIGHT_BASE * 16 + (i) * LIGHT_SIZE_BYTES + 16))

uint4 SecondRow(uint64_t addr)
{
    uint4 r0 = LOAD_REG(addr + 0);
    uint4 r1 = LOAD_REG(addr + 16);
    return r1 + (r0 - r0);
}

void Write(uint index, uint value)
{
    vk::RawBufferStore<uint>(g_PushConstants.Output + index * 4, value);
}

[numthreads(1, 1, 1)]
void main()
{
    Write(0, g_Word7);

    uint2 pair = g_Words8;
    Write(1, pair.x);
    Write(2, pair.y);

    uint4 fixedRegister = REG_U(4);
    Write(3, fixedRegister.x);
    Write(4, fixedRegister.w);

    // A register chosen at run time, by an unsigned and by a signed number.
    uint chosen = vk::RawBufferLoad<uint>(g_PushConstants.Input + 24);
    Write(5, REG_U(chosen).y);

    int back = (int)vk::RawBufferLoad<uint>(g_PushConstants.Input + 4);
    Write(6, vk::RawBufferLoad<uint>(g_PushConstants.Input + 200 + back * 4));

    // A loop over "lights", as the vertex shaders have.
    uint sum = 0;
    for (int i = 0; i < 3; i++)
        sum += LIGHT_WORD(i).z;
    Write(7, sum);

    Write(8, SecondRow(g_PushConstants.Input + 64).y);

    // A 64-bit literal that carries.
    uint64_t far = g_PushConstants.Input + 0x1FFFFFFF0ULL;
    Write(9, vk::RawBufferLoad<uint>(far - 0x1FFFFFFA0ULL));
}
"""

# Register 12 * 16 bytes = word 48; negative offset -8 * 4 bytes from byte 200 = word 42.
HLSL_EXPECTED = [
    ("a constant offset", INPUT[7]),
    ("a vector, first half", INPUT[8]),
    ("a vector, second half", INPUT[9]),
    ("a register, first word", INPUT[16]),
    ("a register, last word", INPUT[19]),
    ("a register chosen at run time", INPUT[12 * 4 + 1]),
    ("a negative offset computed at run time", INPUT[50 - 8]),
    ("a loop over structures", (INPUT[18] + INPUT[26] + INPUT[34]) & MASK32),
    ("an address handed to a function", INPUT[21]),
    ("a 64-bit literal that carries", INPUT[20]),
]


def compile_hlsl(directory):
    executable, environment = lowering.find_compiler()
    if executable is None:
        return None

    source = os.path.join(directory, "forms.hlsl")
    target = os.path.join(directory, "forms.spv")
    with open(source, "w") as file:
        file.write(HLSL)

    # The flags of the build (ShadowRecomp/CMakeLists.txt), for a compute shader.
    process = subprocess.run([executable, "-T", "cs_6_0", "-HV", "2021", "-all-resources-bound", "-spirv", "-fvk-use-dx-layout",
                              "-Fo", target, source], capture_output=True, text=True, env=environment)
    if not check(process.returncode == 0 and os.path.isfile(target), "the build's compiler compiles the HLSL of this test",
                 (process.stdout + process.stderr).strip()[-1500:]):
        return None

    return lowering.read_words(target)


class Harness:
    def __init__(self, path):
        self.path = path

    def info(self):
        process = subprocess.run([self.path, "--info"], capture_output=True, text=True)
        values = dict(line.split("=", 1) for line in process.stdout.splitlines() if "=" in line)
        return process.returncode, values

    def run(self, module_path, output_words, biased):
        command = [self.path, module_path, "--in", ",".join("%x" % word for word in INPUT), "--out", str(output_words),
                   "--pointer", "%d:%d" % (POINTER_AT, 4 * POINTER_TO)]
        if biased:
            command += ["--bias0", "%x" % BIAS_INPUT, "--bias1", "%x" % BIAS_OUTPUT]

        process = subprocess.run(command, capture_output=True, text=True)
        values = dict(line.split("=", 1) for line in process.stdout.splitlines() if "=" in line)
        if process.returncode != 0 or "output" not in values:
            return None, "exit code %d: %s %s" % (process.returncode, process.stdout.strip(), process.stderr.strip()[-1500:])

        return [int(word, 16) for word in values["output"].split(",")], ""


def run_and_compare(harness, module_path, expected, biased, name):
    """Runs one module and compares every word it wrote. Returns how many were right."""
    output, problem = harness.run(module_path, len(expected) + 2, biased)
    if not check(output is not None, name + " runs", problem):
        return 0

    right = 0
    for index, (what, want) in enumerate(expected):
        if check(output[index] == want, "%s: %s" % (name, what), "%#x, expected %#x" % (output[index], want)):
            right += 1

    check(output[len(expected):] == [0, 0], name + " writes nothing beyond its results", output[len(expected):])
    return right


def test_module(harness, tool, validator, has_int64, words, expected, biased, name, directory):
    """Runs a module rewritten, and as it is where the driver can. Returns the words that came out right."""
    original_path = os.path.join(directory, name.replace(" ", "_") + ".spv")
    lowering.write_words(original_path, words)

    if validator:
        ok, message = lowering.validate(validator, original_path)
        check(ok, name + " is a valid shader to begin with", message)

    status, detail, lowered = tool.run(original_path)
    if not check(status == "lowered", name + " is rewritten", (status, detail)):
        return 0

    lowering.check_no_64_bit_left(lowering.Module(lowered), name)
    if validator:
        ok, message = lowering.validate(validator, original_path + ".lowered")
        check(ok, name + ", rewritten, is a valid Vulkan 1.1 shader", message)

    right = run_and_compare(harness, original_path + ".lowered", expected, biased, name + ", rewritten,")

    # The test of the test: with real 64-bit integers the same numbers have to come out.
    if has_int64:
        right += run_and_compare(harness, original_path, expected, biased, name + ", as it is,")

    return right


def main():
    arguments = sys.argv[1:]
    summary_path = None
    if "--summary" in arguments:
        index = arguments.index("--summary")
        summary_path = arguments[index + 1]
        del arguments[index:index + 2]

    if len(arguments) != 2:
        raise SystemExit(__doc__)

    def finish(summary, code):
        print(summary)
        if summary_path:
            with open(summary_path, "w", encoding="utf-8") as file:
                file.write(summary + "\n")
        return code

    harness = Harness(os.path.abspath(arguments[1]))
    code, info = harness.info()
    if code == SKIPPED:
        return finish("Rewritten shaders were NOT executed: %s." % info.get("skipped", "no Vulkan driver"), SKIPPED)
    if not check(code == 0 and "device" in info, "the Vulkan driver of this machine answers", (code, info)):
        return finish("FAILED: the Vulkan driver of this machine could not be asked what it is.", 1)

    has_int64 = info.get("int64") == "1"
    validator = lowering.find_validator()

    with tempfile.TemporaryDirectory(prefix="shader_execution_") as directory:
        tool = lowering.Tool(os.path.abspath(arguments[0]), directory)

        words, expected = build_shader()
        right = test_module(harness, tool, validator, has_int64, words, expected, True, "the shader built here", directory)
        total = len(expected) * (2 if has_int64 else 1)

        hlsl_words = compile_hlsl(directory)
        if hlsl_words is not None:
            right += test_module(harness, tool, validator, has_int64, hlsl_words, HLSL_EXPECTED, False, "the HLSL shader", directory)
            total += len(HLSL_EXPECTED) * (2 if has_int64 else 1)

    summary = "rewritten shaders executed on %s (Vulkan %s, %s 64-bit integers in shaders): %d of %d results right%s%s." % (
        info["device"], info.get("api", "?"), "with" if has_int64 else "WITHOUT", right, total,
        ", the same shaders unrewritten included" if has_int64 else "",
        "" if hlsl_words is not None else "; the HLSL shader was NOT run (no shader compiler for this machine)")

    print("%d checks, %d failures" % (lowering.checks, len(lowering.failures)))
    if lowering.failures:
        return finish("FAILED: %s. Of the %s" % (lowering.failures[0], summary), 1)

    return finish(summary[0].upper() + summary[1:], 0)


if __name__ == "__main__":
    sys.exit(main())
