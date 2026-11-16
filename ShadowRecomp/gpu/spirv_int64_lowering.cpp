#include "spirv_int64_lowering.h"

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <utility>

// What the pass rewrites. `wide` stands for a 64-bit integer type, signed or unsigned;
// both become the same vector of two 32-bit unsigned integers.
//
//   OpTypeInt 64                  dropped; every mention of it names the vector type
//   OpConstant wide               OpConstantComposite of the two halves
//   OpIAdd, OpISub wide           the 32-bit operation on each half, with carry or borrow
//   OpBitwiseAnd/Or/Xor, OpNot    unchanged: they work on each half
//   OpUConvert, OpSConvert        to wide: the value and a zero or its sign as high half;
//                                 from wide: the low half
//   OpBitcast wide <-> wide       OpCopyObject (signed and unsigned are one type now)
//   OpBitcast wide <-> pointer    unchanged: SPIR-V lets a vector of two 32-bit integers
//                                 stand in for the address
//   OpConvertUToPtr, ...PtrToU    OpBitcast, for the same reason
//
// Instructions that only move values around (load, store, phi, calls, members of structs
// and so on) work on the vector as they did on the integer. Anything else that touches a
// 64-bit integer, a multiplication for example, makes the pass turn the module down. So do
// 64-bit integers that pass from one shader stage to the next, where the other side would
// have to change with them, and two function types that would become one and the same.

namespace
{
    constexpr uint32_t Magic = 0x07230203;
    constexpr uint32_t HeaderWords = 5;
    constexpr uint32_t BoundWord = 3;
    constexpr uint32_t MaxIdBound = 0x3FFFFF; // The limit SPIR-V sets itself.
    constexpr uint32_t CapabilityInt64 = 11;
    constexpr uint32_t StorageClassInput = 1;
    constexpr uint32_t StorageClassOutput = 3;

    // What each operand of each instruction is; see tools/android/gen-spirv-operands.py.
    const char* const OperandPatterns[] =
    {
#include "spirv_operand_table.inc"
    };

    constexpr uint32_t KnownOpcodeCount = sizeof(OperandPatterns) / sizeof(OperandPatterns[0]);

    enum Op : uint32_t
    {
        OpUndef = 1,
        OpName = 5,
        OpMemberName = 6,
        OpCapability = 17,
        OpTypeVoid = 19,
        OpTypeBool = 20,
        OpTypeInt = 21,
        OpTypeVector = 23,
        OpTypeArray = 28,
        OpTypeRuntimeArray = 29,
        OpTypeStruct = 30,
        OpTypePointer = 32,
        OpTypeFunction = 33,
        OpTypeForwardPointer = 39,
        OpConstantTrue = 41,
        OpConstant = 43,
        OpConstantComposite = 44,
        OpConstantNull = 46,
        OpSpecConstantOp = 52,
        OpFunction = 54,
        OpFunctionParameter = 55,
        OpFunctionCall = 57,
        OpVariable = 59,
        OpLoad = 61,
        OpStore = 62,
        OpCopyMemory = 63,
        OpDecorate = 71,
        OpMemberDecorate = 72,
        OpVectorShuffle = 79,
        OpCompositeConstruct = 80,
        OpCompositeExtract = 81,
        OpCompositeInsert = 82,
        OpCopyObject = 83,
        OpUConvert = 113,
        OpSConvert = 114,
        OpConvertPtrToU = 117,
        OpConvertUToPtr = 120,
        OpBitcast = 124,
        OpIAdd = 128,
        OpISub = 130,
        OpSelect = 169,
        OpULessThan = 176,
        OpShiftRightLogical = 194,
        OpBitwiseOr = 197,
        OpBitwiseXor = 198,
        OpBitwiseAnd = 199,
        OpNot = 200,
        OpPhi = 245,
        OpReturnValue = 254,
        OpDecorateId = 332,
    };

    // Types, constants and global variables form one section of a module, after the
    // decorations and before the functions. It opens with a type or a constant: everything
    // else in it needs one declared first.
    bool OpensTypeSection(uint32_t op)
    {
        return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= OpConstantTrue && op <= OpSpecConstantOp);
    }

    class Pass
    {
    public:
        Pass(const uint32_t* words, size_t wordCount) : m_words(words), m_count(wordCount)
        {
        }

        SpirvInt64Lowering::EResult Run(std::vector<uint32_t>& output, std::string& error)
        {
            using SpirvInt64Lowering::EResult;

            if (m_words == nullptr || m_count < HeaderWords || m_words[0] != Magic)
            {
                error = "not a SPIR-V module";
                return EResult::Failed;
            }

            m_bound = m_words[BoundWord];
            if (m_bound == 0 || m_bound > MaxIdBound)
            {
                error = "the module claims an impossible number of ids";
                return EResult::Failed;
            }

            m_typeOf.assign(m_bound, 0);
            m_flags.assign(m_bound, 0);

            if (!Scan())
            {
                error = m_error;
                return EResult::Failed;
            }

            if (!m_hasWideType && !m_hasInt64Capability)
                return EResult::Unchanged;

            if (!Rewrite())
            {
                error = m_error;
                return EResult::Failed;
            }

            if (m_bound > MaxIdBound)
            {
                error = "the rewritten module would have too many ids";
                return EResult::Failed;
            }

            output.clear();
            output.reserve(HeaderWords + m_head.size() + m_prelude.size() + m_body.size());
            output.insert(output.end(), m_words, m_words + HeaderWords);
            output[BoundWord] = m_bound;
            output.insert(output.end(), m_head.begin(), m_head.end());
            output.insert(output.end(), m_prelude.begin(), m_prelude.end());
            output.insert(output.end(), m_body.begin(), m_body.end());
            return EResult::Lowered;
        }

    private:
        enum : uint8_t
        {
            FlagWideType = 1 << 0,    // A 64-bit integer type.
            FlagHoisted = 1 << 1,     // A type declaration that was moved to the front.
            FlagInt32Type = 1 << 2,   // A 32-bit integer type, signed or unsigned.
            FlagHoldsWide = 1 << 3,   // A 64-bit integer type, or an array or structure with one inside.
        };

        // Where the ids of the instruction being looked at are.
        struct Operands
        {
            uint32_t typeWord = 0;   // Index of the word with the result type; 0 if none.
            uint32_t resultWord = 0; // Index of the word with the result id; 0 if none.
            std::vector<uint32_t> idWords; // Indices of all other words that hold an id.
        };

        bool Fail(std::string message)
        {
            m_error = std::move(message);
            return false;
        }

        bool FailAt(uint32_t op, const char* what)
        {
            return Fail("instruction " + std::to_string(op) + " " + what);
        }

        // Fills m_operands for the instruction of `count` words at `offset`.
        bool Decode(size_t offset, uint32_t op, uint32_t count)
        {
            const char* pattern = op < KnownOpcodeCount ? OperandPatterns[op] : nullptr;
            if (pattern == nullptr)
                return Fail("instruction " + std::to_string(op) + " is not part of core SPIR-V");

            m_operands.typeWord = 0;
            m_operands.resultWord = 0;
            m_operands.idWords.clear();

            const uint32_t* inst = m_words + offset;
            uint32_t word = 1;
            bool readToEnd = false;

            for (; *pattern != '\0' && !readToEnd; pattern++)
            {
                switch (*pattern)
                {
                case 'T':
                case 'R':
                case 'i':
                case 'l':
                    if (word >= count)
                        return FailAt(op, "is shorter than it has to be");

                    if (*pattern == 'T')
                        m_operands.typeWord = word;
                    else if (*pattern == 'R')
                        m_operands.resultWord = word;
                    else if (*pattern == 'i')
                        m_operands.idWords.push_back(word);

                    word++;
                    break;

                case 's':
                {
                    // Up to and including the word that holds the terminating zero.
                    bool terminated = false;
                    while (word < count && !terminated)
                    {
                        const uint32_t value = inst[word++];
                        terminated = ((value - 0x01010101u) & ~value & 0x80808080u) != 0;
                    }

                    if (!terminated)
                        return FailAt(op, "has a string without an end");

                    break;
                }

                case 'I':
                    while (word < count)
                        m_operands.idWords.push_back(word++);
                    readToEnd = true;
                    break;

                case 'L':
                    word = count;
                    readToEnd = true;
                    break;

                case 'g':
                    if (word < count)
                        word++; // The mask.
                    while (word < count)
                        m_operands.idWords.push_back(word++);
                    readToEnd = true;
                    break;

                case 'p':
                    while (word + 1 < count)
                    {
                        word++;
                        m_operands.idWords.push_back(word++);
                    }
                    word = count;
                    readToEnd = true;
                    break;

                case 'q':
                    while (word < count)
                    {
                        m_operands.idWords.push_back(word++);
                        word++;
                    }
                    word = count;
                    readToEnd = true;
                    break;

                case 'c':
                {
                    // The opcode of the operation, then what that operation takes: ids,
                    // which these three follow up with literal indices.
                    if (word >= count)
                        return FailAt(op, "is shorter than it has to be");

                    const uint32_t operation = inst[word++];
                    uint32_t ids = UINT32_MAX;
                    if (operation == OpVectorShuffle || operation == OpCompositeInsert)
                        ids = 2;
                    else if (operation == OpCompositeExtract)
                        ids = 1;

                    for (; word < count && ids > 0; ids--)
                        m_operands.idWords.push_back(word++);

                    word = count;
                    readToEnd = true;
                    break;
                }

                default:
                    return Fail("the operand table is damaged");
                }
            }

            if (word != count)
                return FailAt(op, "is longer than it can be");

            // Every id has to be one the module can have.
            const auto inRange = [this, inst](uint32_t at) { return at == 0 || (inst[at] != 0 && inst[at] < m_idLimit); };
            if (!inRange(m_operands.typeWord) || !inRange(m_operands.resultWord))
                return FailAt(op, "names an id the module does not have");

            for (uint32_t at : m_operands.idWords)
            {
                if (!inRange(at))
                    return FailAt(op, "names an id the module does not have");
            }

            return true;
        }

        bool IsWideType(uint32_t id) const { return (m_flags[id] & FlagWideType) != 0; }
        bool IsWideValue(uint32_t id) const { return m_typeOf[id] != 0 && IsWideType(m_typeOf[id]); }
        bool IsInt32Value(uint32_t id) const { return m_typeOf[id] != 0 && (m_flags[m_typeOf[id]] & FlagInt32Type) != 0; }

        // First pass: finds the types the rewrite is about and the type of every value.
        bool Scan()
        {
            m_idLimit = m_bound;

            for (size_t offset = HeaderWords; offset < m_count;)
            {
                const uint32_t* inst = m_words + offset;
                const uint32_t count = inst[0] >> 16;
                const uint32_t op = inst[0] & 0xFFFF;

                if (count == 0 || count > m_count - offset)
                    return Fail("the module is cut short or damaged");

                if (!Decode(offset, op, count))
                    return false;

                if (m_operands.typeWord != 0 && m_operands.resultWord != 0)
                    m_typeOf[inst[m_operands.resultWord]] = inst[m_operands.typeWord];

                switch (op)
                {
                case OpCapability:
                    if (inst[1] == CapabilityInt64)
                        m_hasInt64Capability = true;
                    break;

                case OpTypeInt:
                {
                    if (count != 4)
                        return FailAt(op, "is longer than it can be");

                    const uint32_t id = inst[1];
                    if (inst[2] == 64)
                    {
                        m_flags[id] |= FlagWideType | FlagHoldsWide;
                        m_hasWideType = true;
                    }
                    else if (inst[2] == 32)
                    {
                        m_flags[id] |= FlagInt32Type;
                        if (inst[3] == 0 && m_uintType == 0)
                            m_uintType = id;
                    }
                    break;
                }

                case OpTypeBool:
                    if (m_boolType == 0)
                        m_boolType = inst[1];
                    break;

                case OpTypeVector:
                    if (IsWideType(inst[2]))
                        return Fail("the module has vectors of 64-bit integers");

                    if (m_uintType != 0 && inst[2] == m_uintType && inst[3] == 2 && m_pairType == 0)
                        m_pairType = inst[1];
                    break;

                case OpTypeArray:
                case OpTypeRuntimeArray:
                    m_flags[inst[1]] |= m_flags[inst[2]] & FlagHoldsWide;
                    break;

                case OpTypeStruct:
                    for (uint32_t at : m_operands.idWords)
                        m_flags[inst[1]] |= m_flags[inst[at]] & FlagHoldsWide;
                    break;

                case OpTypePointer:
                    // What one shader stage hands to the next, or gets from the vertex
                    // buffers, would change its meaning with its type.
                    if ((inst[2] == StorageClassInput || inst[2] == StorageClassOutput) && (m_flags[inst[3]] & FlagHoldsWide) != 0)
                        return Fail("the module passes 64-bit integers between shader stages");
                    break;

                case OpIAdd:
                case OpISub:
                    if (IsWideType(inst[1]))
                        m_needsCarry = true;
                    break;

                default:
                    break;
                }

                if (m_typesOffset == 0 && OpensTypeSection(op))
                    m_typesOffset = offset;

                offset += count;
            }

            return true;
        }

        uint32_t NewId()
        {
            return m_bound++;
        }

        void Emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands)
        {
            out.push_back((uint32_t(operands.size() + 1) << 16) | op);
            out.insert(out.end(), operands);
        }

        void Emit(uint32_t op, std::initializer_list<uint32_t> operands)
        {
            Emit(*m_out, op, operands);
        }

        // A 32-bit unsigned constant, declared at the front of the type section.
        uint32_t Constant(uint32_t value)
        {
            const auto found = m_constants.find(value);
            if (found != m_constants.end())
                return found->second;

            const uint32_t id = NewId();
            Emit(m_prelude, OpConstant, { m_uintType, id, value });
            m_constants.emplace(value, id);
            return id;
        }

        // The 32-bit unsigned integer, the vector of two of them and, where carries are
        // computed, the boolean type. A module may declare a type only once, and only
        // before its first use, so a declaration the module already has is moved to the
        // front of the type section rather than repeated.
        void BeginPrelude()
        {
            if (m_uintType != 0)
                m_flags[m_uintType] |= FlagHoisted;
            else
                m_uintType = NewId();

            Emit(m_prelude, OpTypeInt, { m_uintType, 32, 0 });

            if (m_pairType != 0)
                m_flags[m_pairType] |= FlagHoisted;
            else
                m_pairType = NewId();

            Emit(m_prelude, OpTypeVector, { m_pairType, m_uintType, 2 });

            if (m_needsCarry)
            {
                if (m_boolType != 0)
                    m_flags[m_boolType] |= FlagHoisted;
                else
                    m_boolType = NewId();

                Emit(m_prelude, OpTypeBool, { m_boolType });
            }
        }

        // The value of a 64-bit constant, if `id` is one.
        std::optional<uint64_t> FindConstant(uint32_t id) const
        {
            const auto found = m_wideConstants.find(id);
            if (found == m_wideConstants.end())
                return std::nullopt;

            return found->second;
        }

        // The id of one half of a 64-bit value: 0 the low one, 1 the high one.
        uint32_t Half(uint32_t wideValue, uint32_t index)
        {
            if (const auto constant = FindConstant(wideValue))
                return Constant(uint32_t(*constant >> (32 * index)));

            const uint32_t id = NewId();
            Emit(OpCompositeExtract, { m_uintType, id, wideValue, index });
            return id;
        }

        // A 32-bit integer value as an unsigned one.
        uint32_t AsUint(uint32_t value)
        {
            if (m_typeOf[value] == m_uintType)
                return value;

            const uint32_t id = NewId();
            Emit(OpBitcast, { m_uintType, id, value });
            return id;
        }

        void LowerAddSub(bool subtract, uint32_t result, uint32_t a, uint32_t b)
        {
            const std::optional<uint64_t> constantA = FindConstant(a);
            const std::optional<uint64_t> constantB = FindConstant(b);

            if (constantB && *constantB == 0)
            {
                Emit(OpCopyObject, { m_pairType, result, a });
                return;
            }

            if (!subtract && constantA && *constantA == 0)
            {
                Emit(OpCopyObject, { m_pairType, result, b });
                return;
            }

            const uint32_t op = subtract ? OpISub : OpIAdd;
            const uint32_t lowA = Half(a, 0);
            const uint32_t lowB = Half(b, 0);

            const uint32_t low = NewId();
            Emit(op, { m_uintType, low, lowA, lowB });

            // An addition carries if the sum wrapped around and so ended up below either
            // operand. A subtraction borrows if it takes away more than there is.
            const uint32_t wrapped = NewId();
            if (subtract)
                Emit(OpULessThan, { m_boolType, wrapped, lowA, lowB });
            else
                Emit(OpULessThan, { m_boolType, wrapped, low, lowA });

            const uint32_t carry = NewId();
            Emit(OpSelect, { m_uintType, carry, wrapped, Constant(1), Constant(0) });

            // The high halves, then the carry. A high half known to be zero is left out;
            // the offsets the shaders add are small constants, so that is the usual case.
            uint32_t high;
            if (constantB && (*constantB >> 32) == 0)
            {
                high = Half(a, 1);
            }
            else if (!subtract && constantA && (*constantA >> 32) == 0)
            {
                high = Half(b, 1);
            }
            else
            {
                const uint32_t highA = Half(a, 1);
                const uint32_t highB = Half(b, 1);
                high = NewId();
                Emit(op, { m_uintType, high, highA, highB });
            }

            const uint32_t highWithCarry = NewId();
            Emit(op, { m_uintType, highWithCarry, high, carry });

            Emit(OpCompositeConstruct, { m_pairType, result, low, highWithCarry });
        }

        void LowerWiden(bool signExtend, uint32_t result, uint32_t value)
        {
            const uint32_t low = AsUint(value);
            uint32_t high = Constant(0);

            if (signExtend)
            {
                // All ones if the top bit is set: 0 - (value >> 31).
                const uint32_t sign = NewId();
                Emit(OpShiftRightLogical, { m_uintType, sign, low, Constant(31) });

                high = NewId();
                Emit(OpISub, { m_uintType, high, Constant(0), sign });
            }

            Emit(OpCompositeConstruct, { m_pairType, result, low, high });
        }

        void LowerNarrow(uint32_t type, uint32_t result, uint32_t wideValue)
        {
            if (type == m_uintType)
            {
                Emit(OpCompositeExtract, { m_uintType, result, wideValue, 0 });
                return;
            }

            const uint32_t low = NewId();
            Emit(OpCompositeExtract, { m_uintType, low, wideValue, 0 });
            Emit(OpBitcast, { type, result, low });
        }

        // Copies the instruction, naming the vector type wherever a 64-bit type was named.
        void CopyWithTypes(const uint32_t* inst, uint32_t count)
        {
            const size_t start = m_out->size();
            m_out->insert(m_out->end(), inst, inst + count);

            const auto substitute = [this, start, inst](uint32_t at)
            {
                if (at != 0 && IsWideType(inst[at]))
                    (*m_out)[start + at] = m_pairType;
            };

            substitute(m_operands.typeWord);
            for (uint32_t at : m_operands.idWords)
                substitute(at);
        }

        void Copy(const uint32_t* inst, uint32_t count)
        {
            m_out->insert(m_out->end(), inst, inst + count);
        }

        // Whether the instruction names a 64-bit type or uses a 64-bit value anywhere.
        bool TouchesWide(const uint32_t* inst) const
        {
            if (m_operands.typeWord != 0 && IsWideType(inst[m_operands.typeWord]))
                return true;

            for (uint32_t at : m_operands.idWords)
            {
                if (IsWideType(inst[at]) || IsWideValue(inst[at]))
                    return true;
            }

            return false;
        }

        // Second pass: writes the module out again.
        bool Rewrite()
        {
            if (m_hasWideType)
                BeginPrelude();

            for (size_t offset = HeaderWords; offset < m_count;)
            {
                const uint32_t* inst = m_words + offset;
                const uint32_t count = inst[0] >> 16;
                const uint32_t op = inst[0] & 0xFFFF;

                // The first pass checked all of this.
                if (!Decode(offset, op, count))
                    return false;

                m_out = (m_typesOffset != 0 && offset >= m_typesOffset) ? &m_body : &m_head;

                if (!RewriteInstruction(inst, op, count))
                    return false;

                offset += count;
            }

            return true;
        }

        bool RewriteInstruction(const uint32_t* inst, uint32_t op, uint32_t count)
        {
            const uint32_t type = m_operands.typeWord != 0 ? inst[m_operands.typeWord] : 0;
            const uint32_t result = m_operands.resultWord != 0 ? inst[m_operands.resultWord] : 0;
            const bool wideResult = type != 0 && IsWideType(type);

            switch (op)
            {
            case OpCapability:
                if (inst[1] != CapabilityInt64)
                    Copy(inst, count);
                return true;

            case OpTypeInt:
            case OpTypeBool:
            case OpTypeVector:
                // Dropped, or already written at the front of the type section.
                if ((m_flags[inst[1]] & (FlagWideType | FlagHoisted)) == 0)
                    Copy(inst, count);
                return true;

            case OpName:
            case OpMemberName:
            case OpDecorate:
            case OpMemberDecorate:
            case OpDecorateId:
                // Whatever was said about a 64-bit type goes with it. Names and decorations
                // of values stay: the values keep their ids.
                if (!IsWideType(inst[1]))
                    Copy(inst, count);
                return true;

            case OpConstant:
                if (!wideResult)
                    break;

                if (count != 5)
                    return FailAt(op, "does not hold a 64-bit number");

                m_wideConstants[result] = uint64_t(inst[3]) | (uint64_t(inst[4]) << 32);
                Emit(OpConstantComposite, { m_pairType, result, Constant(inst[3]), Constant(inst[4]) });
                return true;

            case OpIAdd:
            case OpISub:
                if (!wideResult)
                    break;

                if (!IsWideValue(inst[3]) || !IsWideValue(inst[4]))
                    return FailAt(op, "mixes 64-bit integers with other types");

                LowerAddSub(op == OpISub, result, inst[3], inst[4]);
                return true;

            case OpBitwiseOr:
            case OpBitwiseXor:
            case OpBitwiseAnd:
            case OpNot:
                if (!wideResult)
                    break;

                for (uint32_t at : m_operands.idWords)
                {
                    if (!IsWideValue(inst[at]))
                        return FailAt(op, "mixes 64-bit integers with other types");
                }

                CopyWithTypes(inst, count);
                return true;

            case OpUConvert:
            case OpSConvert:
            {
                const uint32_t value = inst[3];
                if (wideResult)
                {
                    if (!IsInt32Value(value))
                        return FailAt(op, "widens something other than a 32-bit integer");

                    LowerWiden(op == OpSConvert, result, value);
                    return true;
                }

                if (IsWideValue(value))
                {
                    if ((m_flags[type] & FlagInt32Type) == 0)
                        return FailAt(op, "narrows to something other than a 32-bit integer");

                    LowerNarrow(type, result, value);
                    return true;
                }

                break;
            }

            case OpBitcast:
            {
                const uint32_t value = inst[3];
                const bool wideValue = IsWideValue(value);
                if (!wideResult && !wideValue)
                    break;

                // Between two types that are the same one now.
                const bool fromPair = wideValue || m_typeOf[value] == m_pairType;
                const bool toPair = wideResult || type == m_pairType;
                if (fromPair && toPair)
                    Emit(OpCopyObject, { m_pairType, result, value });
                else
                    CopyWithTypes(inst, count);

                return true;
            }

            case OpConvertUToPtr:
                if (!IsWideValue(inst[3]))
                    break;

                Emit(OpBitcast, { type, result, inst[3] });
                return true;

            case OpConvertPtrToU:
                if (!wideResult)
                    break;

                Emit(OpBitcast, { m_pairType, result, inst[3] });
                return true;

            case OpTypeArray:
                // The element may be a 64-bit integer, the length may not.
                if (IsWideValue(inst[3]))
                    return FailAt(op, "has a 64-bit length");

                CopyWithTypes(inst, count);
                return true;

            case OpTypeFunction:
            {
                // A module may not declare the same function type twice, and two that only
                // differed in which 64-bit type they name are the same one now.
                const size_t start = m_out->size();
                CopyWithTypes(inst, count);

                std::vector<uint32_t> signature(m_out->begin() + start + 2, m_out->end());
                if (!m_functionTypes.insert(std::move(signature)).second)
                    return FailAt(op, "would be declared twice once its 64-bit integers are rewritten");

                return true;
            }

            // These move values around without looking into them.
            case OpUndef:
            case OpConstantNull:
            case OpConstantComposite:
            case OpTypeRuntimeArray:
            case OpTypeStruct:
            case OpTypePointer:
            case OpFunction:
            case OpFunctionParameter:
            case OpFunctionCall:
            case OpVariable:
            case OpLoad:
            case OpStore:
            case OpCopyMemory:
            case OpCompositeConstruct:
            case OpCompositeExtract:
            case OpCompositeInsert:
            case OpCopyObject:
            case OpPhi:
            case OpReturnValue:
                CopyWithTypes(inst, count);
                return true;

            default:
                break;
            }

            if (TouchesWide(inst))
                return FailAt(op, "uses a 64-bit integer in a way that cannot be rewritten");

            Copy(inst, count);
            return true;
        }

        const uint32_t* m_words;
        size_t m_count;
        std::string m_error;

        uint32_t m_bound = 0;   // Ids below this are taken. Grows as the rewrite adds some.
        uint32_t m_idLimit = 0; // The bound of the module as it came in.

        std::vector<uint32_t> m_typeOf; // By id: the type of the value, 0 if it is not one.
        std::vector<uint8_t> m_flags;   // By id: Flag... bits.
        Operands m_operands;

        bool m_hasInt64Capability = false;
        bool m_hasWideType = false;
        bool m_needsCarry = false;
        size_t m_typesOffset = 0; // Of the first instruction of the type section; 0 if there is none.

        uint32_t m_uintType = 0; // OpTypeInt 32 0
        uint32_t m_pairType = 0; // OpTypeVector of two of those
        uint32_t m_boolType = 0;

        std::map<uint32_t, uint32_t> m_constants;     // 32-bit value -> id of its constant.
        std::map<uint32_t, uint64_t> m_wideConstants; // id of a 64-bit constant -> its value.
        std::set<std::vector<uint32_t>> m_functionTypes; // Return and parameter types of each one written.

        // The rewritten module after its header: what precedes the type section, what the
        // rewrite puts at the front of that section, and the rest.
        std::vector<uint32_t> m_head;
        std::vector<uint32_t> m_prelude;
        std::vector<uint32_t> m_body;
        std::vector<uint32_t>* m_out = nullptr;
    };
}

namespace SpirvInt64Lowering
{
    EResult Lower(const uint32_t* words, size_t wordCount, std::vector<uint32_t>& output, std::string& error)
    {
        Pass pass(words, wordCount);
        return pass.Run(output, error);
    }
}
