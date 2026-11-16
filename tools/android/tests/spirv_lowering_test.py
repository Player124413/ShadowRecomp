#!/usr/bin/env python3
"""Host test of gpu/spirv_int64_lowering.cpp, the pass that rewrites the shaders for
graphics drivers without 64-bit integers.

    spirv_lowering_test.py <spirv_lower_tool> [<shader_cache_dump>] [--summary <file>]

Three parts:

  1. Modules built here that use 64-bit integers in every way the pass rewrites, and in
     ways it has to turn down.
  2. Damaged input: the pass may refuse it, it may not misbehave (the tool is built with
     sanitizers).
  3. Every shader the game ships: the ones compiled from HLSL at build time, with the
     compiler and flags of the build, and the ones in the shader cache.

A rewritten module is checked three ways. Nothing of 64 bits may be left in it. What has
nothing to do with 64-bit integers must still be there, word for word and in order. And
the numbers must come out the same: a model of the arithmetic, written here independently
of the pass, computes every address a shader reads from, in the original and in the
rewritten module, for inputs chosen to make carries and sign extension matter.

If spirv-val (SPIRV-Tools) is installed, or named by the environment variable SPIRV_VAL,
every rewritten module also has to pass it as a Vulkan 1.1 shader.
"""

import concurrent.futures
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPOSITORY = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))

MAGIC = 0x07230203
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

OpUndef, OpName, OpExtension, OpExtInstImport, OpExtInst, OpMemoryModel = 1, 5, 10, 11, 12, 14
OpEntryPoint, OpExecutionMode, OpCapability = 15, 16, 17
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector = 19, 20, 21, 22, 23
OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer, OpTypeFunction = 28, 29, 30, 32, 33
OpConstant, OpConstantComposite, OpSpecConstant, OpSpecConstantOp = 43, 44, 50, 52
OpFunction, OpFunctionParameter, OpFunctionEnd, OpFunctionCall = 54, 55, 56, 57
OpVariable, OpLoad, OpStore, OpAccessChain, OpDecorate, OpMemberDecorate = 59, 61, 62, 65, 71, 72
OpVectorShuffle, OpCompositeConstruct, OpCompositeExtract, OpCopyObject = 79, 80, 81, 83
OpConvertUToF, OpUConvert, OpSConvert, OpConvertPtrToU, OpConvertUToPtr, OpBitcast = 112, 113, 114, 117, 120, 124
OpIAdd, OpISub, OpIMul = 128, 130, 132
OpSelect, OpIEqual, OpULessThan = 169, 170, 176
OpShiftRightLogical, OpShiftLeftLogical, OpBitwiseOr, OpBitwiseXor, OpBitwiseAnd, OpNot = 194, 196, 197, 198, 199, 200
OpPhi, OpSelectionMerge, OpLabel, OpBranch, OpBranchConditional, OpSwitch = 245, 247, 248, 249, 250, 251
OpReturn, OpReturnValue = 253, 254

CAPABILITY_SHADER, CAPABILITY_INT64, CAPABILITY_INT16, CAPABILITY_PHYSICAL_STORAGE_BUFFER = 1, 11, 22, 5347
STORAGE_INPUT, STORAGE_OUTPUT = 1, 3
STORAGE_PRIVATE, STORAGE_FUNCTION, STORAGE_PUSH_CONSTANT, STORAGE_PHYSICAL = 6, 7, 9, 5349

checks = 0
failures = []


def typed_opcodes():
    """The instructions that begin with the id of their result type and that of their result,
    from the table of the official SPIR-V grammar that the pass itself reads."""
    with open(os.path.join(REPOSITORY, "ShadowRecomp", "gpu", "spirv_operand_table.inc"), encoding="utf-8") as file:
        return {int(number) for number in re.findall(r'/\*\s*(\d+) \w+ \*/ "TR', file.read())}


TYPED = typed_opcodes()


def check(ok, what, detail=""):
    global checks
    checks += 1
    if not ok:
        failures.append(what + (": " + str(detail) if detail else ""))
        print("FAIL " + failures[-1])
    return ok


# --- SPIR-V in and out ---------------------------------------------------------------------

def read_words(path):
    with open(path, "rb") as file:
        data = file.read()
    return list(struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4]))


def write_words(path, words):
    with open(path, "wb") as file:
        file.write(struct.pack("<%dI" % len(words), *words))


def string_words(text):
    data = text.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


class Module:
    """A parsed module, and the model of what its integer arithmetic computes."""

    # Instructions the model computes. All of them have a result type and a result id.
    MODELLED = {OpConstant, OpConstantComposite, OpIAdd, OpISub, OpBitwiseOr, OpBitwiseXor, OpBitwiseAnd, OpNot,
                OpUConvert, OpSConvert, OpBitcast, OpCopyObject, OpConvertUToPtr, OpConvertPtrToU,
                OpCompositeExtract, OpCompositeConstruct, OpULessThan, OpSelect, OpShiftRightLogical}

    def __init__(self, words):
        if len(words) < 5 or words[0] != MAGIC:
            raise ValueError("not a SPIR-V module")

        self.bound = words[3]
        self.instructions = []
        at = 5
        while at < len(words):
            count = words[at] >> 16
            if count == 0 or at + count > len(words):
                raise ValueError("an instruction runs past the end of the module")
            self.instructions.append((words[at] & 0xFFFF, tuple(words[at:at + count])))
            at += count

        self.types = {}        # Of the kinds the checks look into.
        self.all_types = set() # The ids of all of them.
        self.type_of = {}      # By id of a value: the id of its type.
        self.definitions = {}
        for op, w in self.instructions:
            if OpTypeVoid <= op <= OpTypeFunction or op == 39:
                self.all_types.add(w[1])

            if op == OpTypeInt:
                self.types[w[1]] = ("int", w[2])
            elif op == OpTypeBool:
                self.types[w[1]] = ("bool",)
            elif op == OpTypeVector:
                self.types[w[1]] = ("vector", w[2], w[3])
            elif op == OpTypePointer:
                self.types[w[1]] = ("pointer", w[2], w[3])
            elif op in TYPED and len(w) >= 3:
                self.type_of[w[2]] = w[1]
                if op in self.MODELLED:
                    self.definitions[w[2]] = (op, w)

    def bits(self, type_id):
        """How many bits the model keeps for a value of the type; None if it keeps none."""
        kind = self.types.get(type_id)
        if kind is None:
            return None
        if kind[0] == "int":
            return kind[1] if kind[1] in (32, 64) else None
        if kind[0] == "bool":
            return 1
        if kind[0] == "pointer":
            return 64
        # Two 32-bit integers, low half first: what the pass makes of a 64-bit one.
        if kind[0] == "vector" and kind[2] == 2 and self.types.get(kind[1]) == ("int", 32):
            return 64
        return None

    def evaluate(self, value_id, inputs, memo):
        """The value of an id as a number of up to 64 bits.

        Whatever the model does not compute (loads, floating point, the rest of the shader)
        is an input: `inputs` gives the same number for the same id in both modules.
        """
        if value_id in memo:
            if memo[value_id] is None:
                raise ValueError("id %d is defined in terms of itself" % value_id)
            return memo[value_id]

        memo[value_id] = None  # Being worked out.
        definition = self.definitions.get(value_id)
        bits = self.bits(definition[1][1]) if definition else None
        if bits is None:
            result = inputs(value_id)
        else:
            op, w = definition
            mask = (1 << bits) - 1
            get = lambda operand: self.evaluate(operand, inputs, memo)

            if op == OpConstant:
                result = w[3] | (w[4] << 32 if len(w) > 4 else 0)
            elif op in (OpConstantComposite, OpCompositeConstruct):
                result = (get(w[3]) & MASK32) | ((get(w[4]) & MASK32) << 32) if len(w) == 5 else inputs(value_id)
            elif op == OpCompositeExtract:
                result = (get(w[3]) >> (32 * w[4])) & MASK32 if len(w) == 5 and w[4] < 2 else inputs(value_id)
            elif op == OpIAdd:
                result = get(w[3]) + get(w[4])
            elif op == OpISub:
                result = get(w[3]) - get(w[4])
            elif op == OpBitwiseOr:
                result = get(w[3]) | get(w[4])
            elif op == OpBitwiseXor:
                result = get(w[3]) ^ get(w[4])
            elif op == OpBitwiseAnd:
                result = get(w[3]) & get(w[4])
            elif op == OpNot:
                result = ~get(w[3])
            elif op == OpUConvert:
                # Between 32 and 64 bits, either way: the low half, zero above it.
                result = get(w[3]) & MASK32
            elif op == OpSConvert:
                result = get(w[3]) & MASK32
                if bits == 64 and result >> 31:
                    result |= MASK32 << 32
            elif op in (OpBitcast, OpCopyObject, OpConvertUToPtr, OpConvertPtrToU):
                result = get(w[3])
            elif op == OpULessThan:
                result = 1 if (get(w[3]) & MASK32) < (get(w[4]) & MASK32) else 0
            elif op == OpSelect:
                result = get(w[4]) if get(w[3]) & 1 else get(w[5])
            elif op == OpShiftRightLogical:
                shift = get(w[4]) & MASK32
                result = (get(w[3]) & mask) >> shift if shift < bits else 0
            else:
                raise AssertionError(op)

            result &= mask

        memo[value_id] = result
        return result

    def addresses(self):
        """Ids of the pointers made out of numbers: where the shaders read their constants."""
        return [w[2] for op, w in self.instructions
                if op in (OpBitcast, OpConvertUToPtr) and self.types.get(w[1], ("",))[0] == "pointer"]


def make_inputs(seed):
    """Numbers for the inputs of the model, by id. Many of them sit where carries and sign
    extension happen: low halves close to wrapping, negative 32-bit numbers, zeroes."""
    def inputs(value_id):
        x = (seed * 0x9E3779B97F4A7C15 + value_id * 0xBF58476D1CE4E5B9 + 0x94D049BB133111EB) & MASK64
        x ^= x >> 30
        x = (x * 0xBF58476D1CE4E5B9) & MASK64
        x ^= x >> 27
        x = (x * 0x94D049BB133111EB) & MASK64
        x ^= x >> 31

        kind = (x >> 59) % 8
        if kind == 0:
            x |= 0xFFFFFF00            # The low half is about to wrap.
        elif kind == 1:
            x &= ~MASK32 & MASK64      # The low half is zero.
        elif kind == 2:
            x |= 0x80000000            # A negative 32-bit number.
        elif kind == 3:
            x |= MASK32                # -1 as a 32-bit number.
        elif kind == 4:
            x &= 0xFFFF                # A small number.
        elif kind == 5:
            x |= MASK32 << 32          # The high half is about to wrap.
        return x

    return inputs


SEEDS = range(24)


def find_difference(original, lowered, value_ids):
    """Evaluates the ids in both modules for every set of inputs. Returns how many values
    were compared and a description of the first one that differs, or None."""
    compared = 0
    for seed in SEEDS:
        inputs = make_inputs(seed)
        memo_original, memo_lowered = {}, {}
        for value_id in value_ids:
            expected = original.evaluate(value_id, inputs, memo_original)
            try:
                got = lowered.evaluate(value_id, inputs, memo_lowered)
            except (ValueError, RecursionError) as error:
                return compared, str(error)
            compared += 1
            if expected != got:
                return compared, "id %d is %#x for input set %d, expected %#x" % (value_id, got, seed, expected)
    return compared, None


def compare_values(original, lowered, value_ids, name):
    compared, difference = find_difference(original, lowered, value_ids)
    check(difference is None, name + ": the numbers come out the same", difference)
    return compared


def check_no_64_bit_left(lowered, name):
    left = [w for op, w in lowered.instructions
            if (op == OpTypeInt and w[2] == 64) or (op == OpCapability and w[1] == CAPABILITY_INT64)]
    check(not left, name + ": nothing of 64-bit integers is left", left)


def check_declarations(lowered, name):
    """What spirv-val would say about the type section, for where it is not installed: a
    type is declared once, and nothing is used before it is declared."""
    seen = set()
    declared = set()
    for op, w in lowered.instructions:
        if op == OpFunction:
            break

        # Arrays, structures and pointers may be declared more than once.
        if OpTypeVoid <= op <= OpTypeFunction and op not in (OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer):
            key = (op,) + w[2:]
            if key in seen:
                check(False, name + ": a type is declared twice", w)
                return
            seen.add(key)

        uses = ()
        if op == OpTypeVector or op == OpTypeRuntimeArray:
            uses = (w[2],)
        elif op == OpTypeArray:
            uses = (w[2], w[3])
        elif op == OpTypePointer:
            uses = (w[3],)
        elif op in (OpTypeStruct, OpTypeFunction):
            uses = w[2:]
        elif op in (OpConstant, OpVariable):
            uses = (w[1],)
        elif op == OpConstantComposite:
            uses = (w[1],) + w[3:]

        missing = [value_id for value_id in uses if value_id not in declared]
        if missing:
            check(False, name + ": something is used before it is declared", (w, missing))
            return

        if OpTypeVoid <= op <= OpTypeFunction:
            declared.add(w[1])
        elif op in (OpConstant, OpConstantComposite, OpVariable, 41, 42, 46, OpSpecConstant, OpUndef):
            declared.add(w[2])

    check(True, name + ": declarations are in order")


def find_type_error(original, lowered):
    """What a validator would say about the types in the instructions the rewrite produces,
    for where none is installed. Returns the first thing that is wrong, or None."""
    types = lowered.types
    type_of = lowered.type_of

    def shape(type_id):
        """(number of components, bits of each) of an integer or boolean type or a vector of them."""
        kind = types.get(type_id)
        if kind is None:
            return None
        if kind[0] == "int":
            return 1, kind[1]
        if kind[0] == "bool":
            return 1, 1
        if kind[0] == "vector":
            component = shape(kind[1])
            return (kind[2], component[1]) if component else None
        return None

    # A value keeps its type, unless that was a 64-bit integer: then it is the vector of two.
    wide = {type_id for type_id, kind in original.types.items() if kind == ("int", 64)}
    for value_id, type_id in original.type_of.items():
        now = type_of.get(value_id)
        if type_id in wide:
            if shape(now) != (2, 32):
                return "id %d was a 64-bit integer and is not a vector of two 32-bit ones now" % value_id
        elif now != type_id:
            return "id %d changed its type" % value_id

    for op, w in lowered.instructions:
        if op in TYPED and w[1] not in lowered.all_types:
            return "an instruction has a result type that is not declared: %s" % (w,)

        if op == OpBitcast:
            if type_of.get(w[3]) == w[1]:
                return "a bitcast to the type the value already has: %s" % (w,)
        elif op == OpCopyObject:
            if type_of.get(w[3]) != w[1]:
                return "a copy with another type than the value has: %s" % (w,)
        elif op == OpConvertPtrToU:
            if types.get(w[1], ("",))[0] != "int":
                return "a pointer converted to something other than an integer: %s" % (w,)
        elif op == OpConvertUToPtr:
            if types.get(type_of.get(w[3]), ("",))[0] != "int":
                return "something other than an integer converted to a pointer: %s" % (w,)
        elif op in (OpCompositeConstruct, OpConstantComposite):
            kind = types.get(w[1])
            if kind and kind[0] == "vector" and (len(w) != 3 + kind[2] or any(type_of.get(part) != kind[1] for part in w[3:])):
                return "a vector put together from the wrong parts: %s" % (w,)
        elif op == OpCompositeExtract:
            kind = types.get(type_of.get(w[3]))
            if kind and kind[0] == "vector" and (len(w) != 5 or w[4] >= kind[2] or w[1] != kind[1]):
                return "a component taken out of a vector the wrong way: %s" % (w,)
        elif op in (OpIAdd, OpISub, OpBitwiseOr, OpBitwiseXor, OpBitwiseAnd, OpNot, OpShiftRightLogical):
            shapes = {shape(w[1])} | {shape(type_of.get(operand)) for operand in w[3:]}
            if len(shapes) != 1 or None in shapes:
                return "arithmetic on numbers of different sizes: %s" % (w,)
        elif op == OpULessThan:
            if shape(w[1]) != (1, 1) or shape(type_of.get(w[3])) != shape(type_of.get(w[4])) or shape(type_of.get(w[3])) is None:
                return "a comparison of numbers of different sizes: %s" % (w,)
        elif op == OpSelect:
            if type_of.get(w[4]) != w[1] or type_of.get(w[5]) != w[1] or (shape(w[1]) and shape(w[1])[0] == 1 and shape(type_of.get(w[3])) != (1, 1)):
                return "a choice between values of other types than its own: %s" % (w,)
        elif op == OpLoad:
            kind = types.get(type_of.get(w[3]))
            if kind and kind[0] == "pointer" and kind[2] != w[1]:
                return "a load of another type than the pointer points to: %s" % (w,)
        elif op == OpStore:
            kind = types.get(type_of.get(w[1]))
            if kind and kind[0] == "pointer" and kind[2] != type_of.get(w[2]):
                return "a store of another type than the pointer points to: %s" % (w,)
        elif op == OpPhi:
            if any(type_of.get(value) != w[1] for value in w[3::2]):
                return "a phi of values of other types than its own: %s" % (w,)
        elif op in (OpUConvert, OpSConvert):
            source, target = shape(type_of.get(w[3])), shape(w[1])
            if source is None or target is None or source[0] != target[0] or source[1] == target[1]:
                return "a conversion between integers of the same size, or of something else: %s" % (w,)

    return None


def check_rest_untouched(original, lowered, name):
    """Everything that has nothing to do with 64-bit integers is still there, word for word
    and in the same order."""
    wide = {type_id for type_id, kind in original.types.items() if kind == ("int", 64)}
    for op, w in original.instructions:
        if len(w) >= 3 and w[1] in wide:
            wide.add(w[2])

    expected = []
    for op, w in original.instructions:
        # A number that merely equals one of the ids leaves an instruction out of the
        # comparison; that only makes it compare a little less.
        if any(word in wide for word in w[1:]):
            continue
        if op == OpCapability and w[1] == CAPABILITY_INT64:
            continue
        if op in (OpTypeInt, OpTypeBool, OpTypeVector):
            continue  # May have been moved to the front of the type section.
        expected.append(w)

    at = 0
    for op, w in lowered.instructions:
        if at < len(expected) and w == expected[at]:
            at += 1

    check(at == len(expected), name + ": what does not concern 64-bit integers is untouched",
          "%d of %d instructions found in order; first missing: %s" % (at, len(expected), expected[at] if at < len(expected) else ""))
    return len(expected)


def check_lowered(original_words, lowered_words, name, value_ids=None):
    """All checks of one rewritten module. Returns how many values were compared."""
    original = Module(original_words)
    try:
        lowered = Module(lowered_words)
    except ValueError as error:
        check(False, name + ": the rewritten module is damaged", error)
        return 0

    check_no_64_bit_left(lowered, name)
    check_declarations(lowered, name)
    check_rest_untouched(original, lowered, name)
    check(find_type_error(original, lowered) is None, name + ": the types fit", find_type_error(original, lowered))

    addresses = original.addresses()
    check(sorted(addresses) == sorted(lowered.addresses()), name + ": the same pointers are made out of numbers")

    return compare_values(original, lowered, addresses if value_ids is None else list(value_ids) + addresses, name)


# --- Running the pass ----------------------------------------------------------------------

class Tool:
    def __init__(self, path, directory):
        self.path = path
        self.directory = directory
        self.counter = 0

    def run(self, words_or_path):
        """Returns (status, detail, words): status is "lowered", "unchanged" or "failed"."""
        if isinstance(words_or_path, str):
            source = words_or_path
        else:
            self.counter += 1
            source = os.path.join(self.directory, "in_%d.spv" % self.counter)
            write_words(source, words_or_path)

        target = source + ".lowered"
        process = subprocess.run([self.path, source, target], capture_output=True, text=True)
        output = process.stdout.strip()

        if process.returncode == 0 and output == "lowered":
            return "lowered", "", read_words(target)
        if process.returncode == 0 and output == "unchanged":
            return "unchanged", "", None
        if process.returncode == 1 and output.startswith("failed: "):
            return "failed", output[len("failed: "):], None

        return "crashed", "exit code %d: %s %s" % (process.returncode, output, process.stderr.strip()[-2000:]), None


def find_validator():
    path = os.environ.get("SPIRV_VAL") or shutil.which("spirv-val")
    if not path:
        return None
    try:
        subprocess.run([path, "--version"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return path


def validate(validator, path):
    process = subprocess.run([validator, "--target-env", "vulkan1.1", path], capture_output=True, text=True)
    return process.returncode == 0, (process.stdout + process.stderr).strip()


# --- Modules built here ----------------------------------------------------------------------

class Builder:
    """Assembles a shader that reads through addresses, the way the game's do: a fragment
    shader, or a compute shader for the test that runs them (shader_execution_test.py)."""

    def __init__(self, capabilities=(CAPABILITY_SHADER, CAPABILITY_INT64, CAPABILITY_PHYSICAL_STORAGE_BUFFER), compute=False):
        self.next_id = 1
        self.compute = compute
        self.capabilities = list(capabilities)
        self.annotations = []
        self.globals = []
        self.code = []
        self.names = []

        self.void = self.declare(OpTypeVoid)
        self.function_type = self.declare(OpTypeFunction, self.void)
        self.main = self.new_id()

    def new_id(self):
        self.next_id += 1
        return self.next_id - 1

    @staticmethod
    def encode(op, *operands):
        return [((len(operands) + 1) << 16) | op] + list(operands)

    def declare(self, op, *operands):
        """A type: the result id comes first."""
        result = self.new_id()
        self.globals += self.encode(op, result, *operands)
        return result

    def constant(self, type_id, *value_words):
        result = self.new_id()
        self.globals += self.encode(OpConstant, type_id, result, *value_words)
        return result

    def variable(self, pointer_type, storage):
        result = self.new_id()
        self.globals += self.encode(OpVariable, pointer_type, result, storage)
        return result

    def emit(self, op, type_id, *operands):
        """An instruction with a result, in the function."""
        result = self.new_id()
        self.code += self.encode(op, type_id, result, *operands)
        return result

    def statement(self, op, *operands):
        self.code += self.encode(op, *operands)

    def words(self, extra_functions=()):
        words = [MAGIC, 0x00010000, 0, 0, 0]
        for capability in self.capabilities:
            words += self.encode(OpCapability, capability)
        words += self.encode(OpExtension, *string_words("SPV_KHR_physical_storage_buffer"))
        words += self.encode(OpMemoryModel, 5348, 1)  # PhysicalStorageBuffer64, GLSL450
        if self.compute:
            words += self.encode(OpEntryPoint, 5, self.main, *string_words("main"))  # GLCompute
            words += self.encode(OpExecutionMode, self.main, 17, 1, 1, 1)  # LocalSize
        else:
            words += self.encode(OpEntryPoint, 4, self.main, *string_words("main"))  # Fragment
            words += self.encode(OpExecutionMode, self.main, 7)  # OriginUpperLeft
        words += self.names
        words += self.annotations
        words += self.globals
        for function in extra_functions:
            words += function
        words += self.encode(OpFunction, self.void, self.main, 0, self.function_type)
        words += self.encode(OpLabel, self.new_id())
        words += self.code
        words += self.encode(OpReturn)
        words += self.encode(OpFunctionEnd)
        words[3] = self.next_id
        return words


class Scene:
    """What every module below starts with: the push constants with two addresses in them
    and a few 32-bit numbers the shader gets from elsewhere.

    The 64-bit type is declared before the 32-bit one, and the module has a vector of two
    32-bit integers of its own: the pass has to cope with both.
    """

    def __init__(self, signed_wide=True, own_pair=True, compute=False):
        b = self.b = Builder(compute=compute)
        self.void_type = b.void
        self.ulong = b.declare(OpTypeInt, 64, 0)
        self.long = b.declare(OpTypeInt, 64, 1) if signed_wide else None
        self.bool = b.declare(OpTypeBool)
        self.int = b.declare(OpTypeInt, 32, 1)
        self.uint = b.declare(OpTypeInt, 32, 0)
        self.pair = b.declare(OpTypeVector, self.uint, 2) if own_pair else None
        self.float = b.declare(OpTypeFloat, 32)

        self.block = b.declare(OpTypeStruct, self.ulong, self.ulong)
        b.annotations += b.encode(OpDecorate, self.block, 2)  # Block
        b.annotations += b.encode(OpMemberDecorate, self.block, 0, 35, 0)  # Offset
        b.annotations += b.encode(OpMemberDecorate, self.block, 1, 35, 8)
        b.names += b.encode(OpName, self.ulong, *string_words("address"))

        block_pointer = b.declare(OpTypePointer, STORAGE_PUSH_CONSTANT, self.block)
        self.member_pointer = b.declare(OpTypePointer, STORAGE_PUSH_CONSTANT, self.ulong)
        self.push_constants = b.variable(block_pointer, STORAGE_PUSH_CONSTANT)

        self.physical_uint = b.declare(OpTypePointer, STORAGE_PHYSICAL, self.uint)
        private_uint = b.declare(OpTypePointer, STORAGE_PRIVATE, self.uint)
        private_int = b.declare(OpTypePointer, STORAGE_PRIVATE, self.int)
        private_bool = b.declare(OpTypePointer, STORAGE_PRIVATE, self.bool)
        self.unsigned_variable = b.variable(private_uint, STORAGE_PRIVATE)
        self.signed_variable = b.variable(private_int, STORAGE_PRIVATE)
        self.bool_variable = b.variable(private_bool, STORAGE_PRIVATE)

        self.zero = b.constant(self.int, 0)
        self.one = b.constant(self.int, 1)

    def load_address(self, member):
        pointer = self.b.emit(OpAccessChain, self.member_pointer, self.push_constants, self.one if member else self.zero)
        return self.b.emit(OpLoad, self.ulong, pointer)

    def read(self, address):
        """Reads through the address, as vk::RawBufferLoad does."""
        pointer = self.b.emit(OpBitcast, self.physical_uint, address)
        return self.b.emit(OpLoad, self.uint, pointer, 2, 4)  # Aligned 4


def supported_module():
    """Every use of 64-bit integers the pass rewrites. Returns the module and the ids whose
    values have to survive."""
    s = Scene()
    b = s.b
    keep = []

    a = s.load_address(0)
    c = s.load_address(1)
    x = b.emit(OpLoad, s.uint, s.unsigned_variable)
    y = b.emit(OpLoad, s.int, s.signed_variable)

    small = b.constant(s.ulong, 16, 0)
    large = b.constant(s.ulong, 0x10, 0x1)
    high_only = b.constant(s.ulong, 0, 5)
    nothing = b.constant(s.ulong, 0, 0)
    negative = b.constant(s.ulong, 0xFFFFFFF0, 0xFFFFFFFF)

    # Additions and subtractions: of two addresses, of constants on either side, of zero,
    # of constants with only a low or only a high half.
    for op in (OpIAdd, OpISub):
        for left, right in ((a, c), (a, small), (small, a), (a, large), (large, a), (a, high_only), (high_only, a), (a, nothing),
                            (nothing, a), (a, negative), (small, large), (high_only, negative)):
            keep.append(b.emit(op, s.ulong, left, right))

    # An offset computed at run time: sign-extended, as the vertex shaders do it.
    offset = b.emit(OpSConvert, s.long, y)
    offset = b.emit(OpBitcast, s.ulong, offset)
    indexed = b.emit(OpIAdd, s.ulong, a, offset)
    keep += [offset, indexed]

    # The same from unsigned and zero-extended numbers, in the four combinations.
    keep.append(b.emit(OpUConvert, s.ulong, x))
    keep.append(b.emit(OpUConvert, s.ulong, y))
    keep.append(b.emit(OpSConvert, s.ulong, x))
    keep.append(b.emit(OpSConvert, s.long, x))

    # Back to 32 bits.
    keep.append(b.emit(OpUConvert, s.uint, a))
    keep.append(b.emit(OpSConvert, s.int, c))
    keep.append(b.emit(OpUConvert, s.uint, small))

    # Bit operations work on both halves at once.
    keep.append(b.emit(OpBitwiseAnd, s.ulong, a, c))
    keep.append(b.emit(OpBitwiseOr, s.ulong, a, small))
    keep.append(b.emit(OpBitwiseXor, s.ulong, negative, c))
    keep.append(b.emit(OpNot, s.ulong, a))

    # To a pointer and back, by both instructions SPIR-V has for it.
    pointer = b.emit(OpConvertUToPtr, s.physical_uint, indexed)
    keep.append(b.emit(OpConvertPtrToU, s.ulong, pointer))
    keep.append(b.emit(OpBitcast, s.ulong, pointer))
    b.emit(OpLoad, s.uint, pointer, 2, 4)

    # The module's own vector of two 32-bit integers is the same type after the rewrite.
    as_pair = b.emit(OpBitcast, s.pair, c)
    keep.append(as_pair)
    keep.append(b.emit(OpBitcast, s.ulong, as_pair))

    # Through a variable, a structure, a copy.
    function_pointer = b.declare(OpTypePointer, STORAGE_FUNCTION, s.ulong)
    record = b.declare(OpTypeStruct, s.ulong, s.uint)
    variable = b.new_id()
    # Variables come first in a function.
    b.code = b.encode(OpVariable, function_pointer, variable, STORAGE_FUNCTION) + b.code
    b.statement(OpStore, variable, indexed)
    reloaded = b.emit(OpLoad, s.ulong, variable)
    packed = b.emit(OpCompositeConstruct, record, reloaded, x)
    unpacked = b.emit(OpCompositeExtract, s.ulong, packed, 0)
    copied = b.emit(OpCopyObject, s.ulong, unpacked)
    keep.append(b.emit(OpIAdd, s.ulong, copied, small))

    # Through a branch and a call.
    helper_type = b.declare(OpTypeFunction, s.ulong, s.ulong)
    helper = b.new_id()
    parameter = b.new_id()
    helper_label = b.new_id()
    helper_sum = b.new_id()
    helper_code = (b.encode(OpFunction, s.ulong, helper, 0, helper_type) + b.encode(OpFunctionParameter, s.ulong, parameter) +
                   b.encode(OpLabel, helper_label) + b.encode(OpIAdd, s.ulong, helper_sum, parameter, small) +
                   b.encode(OpReturnValue, helper_sum) + b.encode(OpFunctionEnd))

    condition = b.emit(OpLoad, s.bool, s.bool_variable)
    then_label, else_label, merge_label = b.new_id(), b.new_id(), b.new_id()
    b.statement(OpSelectionMerge, merge_label, 0)
    b.statement(OpBranchConditional, condition, then_label, else_label)
    b.statement(OpLabel, then_label)
    called = b.emit(OpFunctionCall, s.ulong, helper, a)
    b.statement(OpBranch, merge_label)
    b.statement(OpLabel, else_label)
    other = b.emit(OpISub, s.ulong, c, small)
    b.statement(OpBranch, merge_label)
    b.statement(OpLabel, merge_label)
    chosen = b.emit(OpPhi, s.ulong, called, then_label, other, else_label)
    keep.append(b.emit(OpIAdd, s.ulong, chosen, small))

    # Not about 64-bit integers, but about telling ids from numbers, which the pass has to
    # do for every instruction: operations on specialization constants, whose operands are
    # ids followed, in these two, by indices. An index of 0 is no id.
    two = b.new_id()
    b.globals += b.encode(OpConstantComposite, s.pair, two, b.constant(s.uint, 7), b.constant(s.uint, 9))
    b.globals += b.encode(OpSpecConstantOp, s.uint, b.new_id(), OpCompositeExtract, two, 0)
    b.globals += b.encode(OpSpecConstantOp, s.pair, b.new_id(), OpVectorShuffle, two, two, 1, 0)

    # Read through some of them, as the shaders do.
    s.read(indexed)
    s.read(keep[0])
    s.read(keep[-1])

    return b.words([helper_code]), keep


def refused_modules():
    """Uses of 64-bit integers the pass does not rewrite: name, module, a word of the reason."""
    cases = []

    def scene(name, reason, build, **arguments):
        s = Scene(**arguments)
        build(s, s.b, s.load_address(0), s.load_address(1))
        cases.append((name, s.b.words(), reason))

    cannot = "cannot be rewritten"
    scene("a multiplication", cannot, lambda s, b, a, c: s.read(b.emit(OpIMul, s.ulong, a, c)))
    scene("a shift", cannot, lambda s, b, a, c: s.read(b.emit(OpShiftLeftLogical, s.ulong, a, b.emit(OpLoad, s.uint, s.unsigned_variable))))
    scene("a comparison", cannot, lambda s, b, a, c: b.emit(OpIEqual, s.bool, a, c))
    scene("a choice between two", cannot, lambda s, b, a, c: s.read(b.emit(OpSelect, s.ulong, b.emit(OpLoad, s.bool, s.bool_variable), a, c)))
    scene("a conversion to floating point", cannot, lambda s, b, a, c: b.emit(OpConvertUToF, s.float, a))
    scene("an addition of a 64-bit and a 32-bit number", "mixes",
          lambda s, b, a, c: s.read(b.emit(OpIAdd, s.ulong, a, b.emit(OpLoad, s.uint, s.unsigned_variable))))
    scene("a bit operation on a 64-bit and a 32-bit number", "mixes",
          lambda s, b, a, c: s.read(b.emit(OpBitwiseAnd, s.ulong, a, b.emit(OpLoad, s.uint, s.unsigned_variable))))
    scene("a vector of them", "vectors of 64-bit integers", lambda s, b, a, c: b.declare(OpTypeVector, s.ulong, 2))
    scene("an array with a 64-bit length", "64-bit length", lambda s, b, a, c: b.declare(OpTypeArray, s.uint, b.constant(s.ulong, 4, 0)))
    scene("a 64-bit specialization constant", cannot, lambda s, b, a, c: b.globals.extend(b.encode(OpSpecConstant, s.ulong, b.new_id(), 1, 0)))
    scene("an operation on a 64-bit constant", cannot,
          lambda s, b, a, c: b.globals.extend(b.encode(OpSpecConstantOp, s.uint, b.new_id(), OpUConvert, b.constant(s.ulong, 4, 0))))
    scene("a 64-bit constant of one word", "does not hold a 64-bit number", lambda s, b, a, c: b.constant(s.ulong, 4))

    def glsl_function(s, b, a, c):
        glsl = b.new_id()
        # Imports go before the memory model; for the pass the place does not matter.
        b.globals = b.encode(OpExtInstImport, glsl, *string_words("GLSL.std.450")) + b.globals
        s.read(b.emit(OpExtInst, s.ulong, glsl, 38, a, c))  # UMin

    scene("a library function", cannot, glsl_function)

    def indexed_by_wide(s, b, a, c):
        array = b.declare(OpTypeArray, s.uint, b.constant(s.uint, 4))
        pointer = b.declare(OpTypePointer, STORAGE_PRIVATE, array)
        element = b.declare(OpTypePointer, STORAGE_PRIVATE, s.uint)
        b.emit(OpAccessChain, element, b.variable(pointer, STORAGE_PRIVATE), a)

    scene("an index", cannot, indexed_by_wide)

    def switched(s, b, a, c):
        default, merge = b.new_id(), b.new_id()
        b.statement(OpSelectionMerge, merge, 0)
        b.statement(OpSwitch, a, default)
        b.statement(OpLabel, default)
        b.statement(OpBranch, merge)
        b.statement(OpLabel, merge)

    scene("a switch", cannot, switched)

    def sixteen_bits(s, b):
        b.capabilities.append(CAPABILITY_INT16)
        return b.declare(OpTypeInt, 16, 1)

    def from_16_bits(s, b, a, c):
        short = sixteen_bits(s, b)
        pointer = b.declare(OpTypePointer, STORAGE_PRIVATE, short)
        value = b.emit(OpLoad, short, b.variable(pointer, STORAGE_PRIVATE))
        s.read(b.emit(OpSConvert, s.ulong, value))

    scene("a conversion from 16 bits", "widens something other", from_16_bits)
    scene("a conversion to 16 bits", "narrows to something other", lambda s, b, a, c: b.emit(OpSConvert, sixteen_bits(s, b), a))

    scene("an instruction of an extension", "not part of core SPIR-V", lambda s, b, a, c: b.statement(5000, a))

    # What the next stage, or the vertex buffers, would have to agree with.
    between = "between shader stages"
    scene("a 64-bit input", between, lambda s, b, a, c: b.variable(b.declare(OpTypePointer, STORAGE_INPUT, s.ulong), STORAGE_INPUT))
    scene("a 64-bit output inside a structure", between,
          lambda s, b, a, c: b.declare(OpTypePointer, STORAGE_OUTPUT, b.declare(OpTypeStruct, s.float, b.declare(OpTypeArray, s.long, b.constant(s.uint, 2)))))

    # Two function types that differ only in the 64-bit type they name: signed and unsigned,
    # or one of them and the vector that both turn into.
    def twin_function_types(s, b, a, c):
        b.declare(OpTypeFunction, s.ulong, s.uint)
        b.declare(OpTypeFunction, s.long, s.uint)

    def twin_of_the_pair(s, b, a, c):
        b.declare(OpTypeFunction, s.void_type, s.pair)
        b.declare(OpTypeFunction, s.void_type, s.ulong)

    scene("two function types that become one", "declared twice", twin_function_types)
    scene("a function type that becomes one the module has", "declared twice", twin_of_the_pair)

    return cases


def malformed_modules():
    """What is not a SPIR-V module, or one put together wrongly: name, words, a word of the reason."""
    cases = []
    encode = Builder.encode

    def scene(name, reason, change):
        s = Scene()
        s.read(s.load_address(0))
        words = change(s, s.b) or s.b.words()
        cases.append((name, words, reason))

    def with_words(change):
        def apply(s, b):
            words = b.words()
            change(words)
            return words
        return apply

    def set_word(index, value):
        return with_words(lambda words: words.__setitem__(index, value))

    not_a_module = "not a SPIR-V module"
    cases.append(("nothing at all", [], not_a_module))
    cases.append(("four words", [MAGIC, 0x00010000, 0, 8], not_a_module))
    scene("another magic number", not_a_module, set_word(0, 0x03022307))
    scene("no ids at all", "impossible number of ids", set_word(3, 0))
    scene("more ids than SPIR-V allows", "impossible number of ids", set_word(3, 0x400000))

    scene("an id of zero", "names an id the module does not have", lambda s, b: b.names.extend(encode(OpName, 0, *string_words("nobody"))))
    scene("an id beyond the bound", "names an id the module does not have", with_words(lambda words: words.extend(encode(OpName, words[3], *string_words("nobody")))))
    scene("an instruction without its operands", "shorter than it has to be", lambda s, b: b.globals.extend(encode(OpTypePointer, b.new_id(), STORAGE_PRIVATE)))
    scene("an operation on constants without the operation", "shorter than it has to be", lambda s, b: b.globals.extend(encode(OpSpecConstantOp, s.uint, b.new_id())))

    def shuffle_of_nothing(s, b):
        two = b.new_id()
        b.globals.extend(encode(OpConstantComposite, s.pair, two, b.constant(s.uint, 7), b.constant(s.uint, 9)))
        b.globals.extend(encode(OpSpecConstantOp, s.pair, b.new_id(), OpVectorShuffle, two, 0x3FFFFF, 1, 0))

    scene("an operation on constants with an id beyond the bound", "names an id the module does not have", shuffle_of_nothing)
    scene("an instruction with an operand too many", "longer than it can be", lambda s, b: b.globals.extend(encode(OpTypeBool, b.new_id(), 7)))
    scene("a name without an end", "string without an end", lambda s, b: b.names.extend(encode(OpName, s.ulong, 0x41414141, 0x42424242)))
    scene("an instruction of no words", "cut short or damaged", with_words(lambda words: words.append(OpName)))
    scene("an instruction that runs past the end", "cut short or damaged", with_words(lambda words: words.append((3 << 16) | OpName)))

    return cases


def test_built_modules(tool, validator, directory):
    # Nothing to do.
    plain = Builder(capabilities=(CAPABILITY_SHADER, CAPABILITY_PHYSICAL_STORAGE_BUFFER))
    plain.declare(OpTypeInt, 32, 0)
    status, detail, _ = tool.run(plain.words())
    check(status == "unchanged", "a module without 64-bit integers is left alone", (status, detail))

    # The capability alone is enough for a driver to refuse the module.
    declared_only = Builder()
    uint = declared_only.declare(OpTypeInt, 32, 0)
    declared_only.constant(uint, 11)  # The number of the capability, as a mere number.
    words = declared_only.words()
    status, detail, lowered = tool.run(words)
    if check(status == "lowered", "a module that only declares the capability is rewritten", (status, detail)):
        expected = [w for op, w in Module(words).instructions if not (op == OpCapability and w[1] == CAPABILITY_INT64)]
        check([w for op, w in Module(lowered).instructions] == expected, "only the capability is taken out of it")
        check(lowered[:5] == words[:5], "its header stays as it was")

    # Everything the pass rewrites.
    words, keep = supported_module()
    original_path = os.path.join(directory, "supported.spv")
    write_words(original_path, words)
    if validator:
        ok, message = validate(validator, original_path)
        check(ok, "the module of this test is a valid shader to begin with", message)

    status, detail, lowered = tool.run(words)
    compared = 0
    if check(status == "lowered", "a module with every supported use is rewritten", (status, detail)):
        compared = check_lowered(words, lowered, "supported uses", keep)
        check(compared == (len(keep) + len(Module(words).addresses())) * len(SEEDS), "all of its values were compared", compared)

        module = Module(lowered)
        check(sum(1 for op, w in module.instructions if op == OpTypeInt and w[2:] == (32, 0)) == 1, "the 32-bit type is declared once")
        check(sum(1 for op, w in module.instructions if op == OpTypeVector and w[3] == 2 and module.types.get(w[2]) == ("int", 32)) == 1,
              "the vector of two is declared once")
        gone = {w[1] for op, w in Module(words).instructions if op == OpTypeInt and w[2] == 64}
        check(not any(op == OpName and w[1] in gone for op, w in module.instructions), "no name is left for a type that is gone")
        check(lowered[3] > words[3], "the id bound grows with the ids that were added")

        if validator:
            lowered_path = os.path.join(directory, "supported.lowered.spv")
            write_words(lowered_path, lowered)
            ok, message = validate(validator, lowered_path)
            check(ok, "the rewritten module is a valid Vulkan 1.1 shader", message)

        # Rewriting what is already rewritten changes nothing.
        status, detail, _ = tool.run(lowered)
        check(status == "unchanged", "a rewritten module needs no second rewrite", (status, detail))

    # The model must be able to tell a wrong result from a right one, or the comparisons
    # above prove nothing: a module whose additions lose their carry is caught.
    if lowered:
        broken = list(lowered)
        at = 5
        changed = 0
        while at < len(broken):
            count = broken[at] >> 16
            if broken[at] & 0xFFFF == OpSelect:
                broken[at + 4], broken[at + 5] = broken[at + 5], broken[at + 5]  # The carry is never 1.
                changed += 1
            at += count

        _, difference = find_difference(Module(words), Module(broken), keep)
        check(changed > 0 and difference is not None, "the comparison notices a lost carry")

    # What the pass does not rewrite, it turns down, and says why. So it does with what is
    # no SPIR-V module, or a damaged one.
    for name, module_words, reason in refused_modules() + malformed_modules():
        status, detail, _ = tool.run(module_words)
        check(status == "failed" and reason in detail, "turned down: " + name, (status, detail))

    return compared


# --- Damaged input ---------------------------------------------------------------------------

def test_damaged_input(tool, sample_paths):
    """Cut short anywhere, or with any one word replaced: refused or rewritten, never a crash.

    The time this takes grows with the square of the size of a module, so of the larger
    ones only every so many words are damaged."""
    runs = 0
    for path in sample_paths:
        words = os.path.getsize(path) // 4
        every = max(1, words * words // 1500000)
        process = subprocess.run([tool.path, "--damage", path, str(every)], capture_output=True, text=True)
        ok = process.returncode == 0 and process.stdout.startswith("survived ")
        check(ok, "damaged copies of %s are survived" % os.path.basename(path),
              "exit code %d: %s %s" % (process.returncode, process.stdout.strip(), process.stderr.strip()[-2000:]))
        if ok:
            runs += int(process.stdout.split()[1])
    return runs


# --- The shaders of the game -----------------------------------------------------------------

def find_compiler():
    """The command line of the DirectX Shader Compiler the build uses (tools/android/android.cmake)."""
    machine = platform.machine().lower()
    arch = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if arch is None or not sys.platform.startswith("linux"):
        return None, None

    root = os.path.join(REPOSITORY, "tools", "XShaderRecomp", "thirdparty", "dxc-bin")
    executable = os.path.join(root, "bin", arch, "dxc-linux")
    if not os.path.isfile(executable):
        return None, None

    environment = dict(os.environ, LD_LIBRARY_PATH=os.path.join(root, "lib", arch))
    try:
        subprocess.run([executable, "--version"], capture_output=True, env=environment, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None, None

    return executable, environment


def shader_sources():
    """(name, profile, source, extra arguments) of every shader the build compiles."""
    game = os.path.join(REPOSITORY, "ShadowRecomp")
    fixed_function = os.path.join(REPOSITORY, "tools", "XShaderRecomp", "XShaderRecomp", "fixed_function")
    shaders = []

    with open(os.path.join(game, "CMakeLists.txt"), encoding="utf-8-sig") as file:
        for kind, name in re.findall(r"^compile_(vertex|pixel)_shader\((\w+)\)", file.read(), re.MULTILINE):
            source = os.path.join(game, "gpu", "shader", name + ".hlsl")
            shaders.append((name, "vs_6_0" if kind == "vertex" else "ps_6_0", source, ["-fvk-invert-y"] if kind == "vertex" else []))

    shaders.append(("FixedFunctionPixelShader", "ps_6_0", os.path.join(fixed_function, "FixedFunctionPixelShader.hlsl"), []))
    shaders.append(("FixedFunctionVertexShader", "vs_6_0", os.path.join(fixed_function, "FixedFunctionVertexShader.hlsl"), ["-fvk-invert-y"]))

    generated = os.path.join(game, "gpu", "ubershaders", "generated")
    for family, profile, extra in (("fixed_function_pixel_shader", "ps_6_0", []), ("fixed_function_vertex_shader", "vs_6_0", ["-fvk-invert-y"])):
        directory = os.path.join(generated, family)
        for name in sorted(os.listdir(directory)) if os.path.isdir(directory) else []:
            if re.fullmatch(r"variant_\d+\.hlsl", name):
                shaders.append((family + "_" + name[:-5], profile, os.path.join(directory, name), extra))

    return shaders, fixed_function


def compile_shaders(directory):
    """Compiles them as the build does (ShadowRecomp/CMakeLists.txt, XShaderRecompFixedFunction.cmake).
    Returns the paths of the SPIR-V files, or None if there is no compiler for this machine."""
    executable, environment = find_compiler()
    if executable is None:
        return None

    shaders, include = shader_sources()

    def compile_one(shader):
        name, profile, source, extra = shader
        target = os.path.join(directory, name + ".spv")
        command = [executable, "-T", profile, "-HV", "2021", "-all-resources-bound", "-spirv", "-fvk-use-dx-layout",
                   "-I", include] + extra + ["-Fo", target, source]
        process = subprocess.run(command, capture_output=True, text=True, env=environment)
        if process.returncode != 0 or not os.path.isfile(target):
            return name, None, (process.stdout + process.stderr).strip()[-1000:]
        return name, target, ""

    paths = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for name, target, message in pool.map(compile_one, shaders):
            if check(target is not None, "the build's compiler compiles " + name, message):
                paths.append(target)

    return paths


def dump_shader_cache(dump_tool, directory):
    process = subprocess.run([dump_tool, directory], capture_output=True, text=True)
    if not check(process.returncode == 0, "the shader cache unpacks", process.stderr.strip()):
        return []
    return sorted(os.path.join(directory, name) for name in os.listdir(directory) if name.startswith("cache_"))


def test_game_shaders(tool, validator, paths):
    rewritten = unchanged = compared = validated = 0
    for path in paths:
        name = os.path.basename(path)
        words = read_words(path)
        status, detail, lowered = tool.run(path)

        if status == "unchanged":
            unchanged += 1
            check(not any((op == OpTypeInt and w[2] == 64) or (op == OpCapability and w[1] == CAPABILITY_INT64)
                          for op, w in Module(words).instructions), name + ": left alone only without 64-bit integers")
            continue

        if not check(status == "lowered", name + " is rewritten", (status, detail)):
            continue

        rewritten += 1
        compared += check_lowered(words, lowered, name)

        if validator:
            # A validator too old for the original says nothing about the rewrite.
            if validate(validator, path)[0]:
                ok, message = validate(validator, path + ".lowered")
                check(ok, name + ": the rewritten module is a valid Vulkan 1.1 shader", message)
                validated += 1

    return rewritten, unchanged, compared, validated


def main():
    arguments = sys.argv[1:]
    summary_path = None
    if "--summary" in arguments:
        at = arguments.index("--summary")
        summary_path = arguments[at + 1]
        del arguments[at:at + 2]

    if not 1 <= len(arguments) <= 2:
        raise SystemExit(__doc__)

    validator = find_validator()

    with tempfile.TemporaryDirectory(prefix="spirv_lowering_") as directory:
        tool = Tool(os.path.abspath(arguments[0]), directory)
        test_built_modules(tool, validator, directory)

        compiled_directory = os.path.join(directory, "compiled")
        cache_directory = os.path.join(directory, "cache")
        os.mkdir(compiled_directory)
        os.mkdir(cache_directory)

        compiled = compile_shaders(compiled_directory)
        cached = dump_shader_cache(os.path.abspath(arguments[1]), cache_directory) if len(arguments) > 1 else []
        paths = (compiled or []) + cached

        rewritten, unchanged, compared, validated = test_game_shaders(tool, validator, paths)

        # The module built here, and of the game's shaders that are rewritten the smallest
        # and the largest, of each kind.
        samples = [os.path.join(directory, "supported.spv")]
        for group in (compiled or [], cached):
            group = sorted((path for path in group if os.path.isfile(path + ".lowered")), key=os.path.getsize)
            samples += group[:1] + group[-1:] if len(group) > 1 else group
        damaged = test_damaged_input(tool, samples)

    parts = ["%d of the game's shaders rewritten without 64-bit integers, %d never had any" % (rewritten, unchanged)]
    if compiled is None:
        parts.append("the HLSL shaders were NOT tested (no shader compiler for this machine)")
    if len(arguments) < 2:
        parts.append("the shader cache was NOT tested")
    parts.append("%d addresses computed both ways and compared" % compared)
    parts.append("%d rewritten shaders passed spirv-val" % validated if validator else "spirv-val is not installed")
    parts.append("%d damaged modules survived" % damaged)
    summary = "; ".join(parts) + "."

    print("%d checks, %d failures" % (checks, len(failures)))
    print(summary)

    if summary_path:
        with open(summary_path, "w", encoding="utf-8") as file:
            file.write(("FAILED: %s. " % failures[0] if failures else "") + summary + "\n")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
