// Host test of gpu/vulkan_int64_lowering.cpp: what reaches the graphics driver once the
// game rewrites its shaders for a driver without 64-bit integers. The driver is a stand-in
// that records what it is handed; what the rewrite itself does to a shader is the subject
// of spirv_lowering_test.py.
#include <gpu/vulkan_int64_lowering.h>
#include <gpu/spirv_int64_lowering.h>
#include <os/logger.h>

#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

static void Check(bool ok, const char* what, const std::string& got = {})
{
    g_checks++;
    if (!ok)
    {
        g_failures++;
        printf("FAIL %s%s%s\n", what, got.empty() ? "" : ": ", got.c_str());
    }
}

// --- The log ------------------------------------------------------------------------------

static std::mutex g_logMutex;
static std::vector<std::string> g_errors; // Guarded by g_logMutex.
static int g_otherMessages = 0;

namespace os::logger
{
    void Log(const std::string_view str, ELogType type, const char*)
    {
        std::lock_guard lock(g_logMutex);
        if (type == ELogType::Error)
            g_errors.emplace_back(str);
        else
            g_otherMessages++;
    }
}

// --- The stand-in driver ------------------------------------------------------------------

static const VkDevice g_device = reinterpret_cast<VkDevice>(uintptr_t(0x1000));
static VkAllocationCallbacks g_allocator{};

struct Call
{
    VkDevice device;
    VkStructureType sType;
    const void* next;
    VkShaderModuleCreateFlags flags;
    const uint32_t* codePointer;
    std::vector<uint8_t> code;
    const VkAllocationCallbacks* allocator;
};

static std::mutex g_driverMutex;
static std::vector<Call> g_calls; // Guarded by g_driverMutex, as is the next.
static VkResult g_driverResult = VK_SUCCESS;
static std::atomic<uint64_t> g_callCount{ 0 };
static std::atomic<uint64_t> g_callsWithInt64{ 0 };
static std::atomic<bool> g_record{ true };

static bool HasInt64(const uint8_t* code, size_t size)
{
    // OpCapability Int64, among the instructions that open a module. All the modules of
    // this test are made of one-word-operand instructions up to there.
    for (size_t at = 20; at + 8 <= size; at += 8)
    {
        uint32_t words[2];
        memcpy(words, code + at, 8);
        if (words[0] != ((2u << 16) | 17))
            break;
        if (words[1] == 11)
            return true;
    }

    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator, VkShaderModule* shaderModule)
{
    const uint64_t number = g_callCount.fetch_add(1) + 1;

    // A driver reads the code as 32-bit words and may read all of it.
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(createInfo->pCode);
    if (HasInt64(bytes, createInfo->codeSize))
        g_callsWithInt64.fetch_add(1);

    std::lock_guard lock(g_driverMutex);
    if (g_record)
    {
        Call call;
        call.device = device;
        call.sType = createInfo->sType;
        call.next = createInfo->pNext;
        call.flags = createInfo->flags;
        call.codePointer = createInfo->pCode;
        call.code.assign(bytes, bytes + createInfo->codeSize);
        call.allocator = allocator;
        g_calls.push_back(std::move(call));
    }

    if (g_driverResult == VK_SUCCESS)
        *shaderModule = reinterpret_cast<VkShaderModule>(uintptr_t(0x5000 + number));

    return g_driverResult;
}

// The one volk entry point the code under test uses.
extern "C"
{
    PFN_vkCreateShaderModule vkCreateShaderModule = FakeCreateShaderModule;
}

// --- Shaders --------------------------------------------------------------------------------

static void Instruction(std::vector<uint32_t>& words, uint32_t op, std::initializer_list<uint32_t> operands)
{
    words.push_back((uint32_t(operands.size() + 1) << 16) | op);
    words.insert(words.end(), operands);
}

enum class EKind
{
    Plain,       // No 64-bit integers.
    WithInt64,   // Declares and uses them in a way that can be rewritten.
    Unsupported  // Uses them in a way that cannot.
};

static std::vector<uint32_t> MakeModule(EKind kind, uint32_t marker)
{
    std::vector<uint32_t> words = { 0x07230203, 0x00010000, 0, 16, 0 };
    Instruction(words, 17, { 1 }); // OpCapability Shader
    if (kind != EKind::Plain)
        Instruction(words, 17, { 11 }); // OpCapability Int64
    Instruction(words, 14, { 0, 1 }); // OpMemoryModel Logical GLSL450

    Instruction(words, 21, { 1, 32, 0 });      // %1 = OpTypeInt 32 0
    Instruction(words, 43, { 1, 2, marker });  // %2 = OpConstant %1 marker
    if (kind != EKind::Plain)
    {
        Instruction(words, 21, { 3, 64, 0 });               // %3 = OpTypeInt 64 0
        Instruction(words, 43, { 3, 4, marker, 0x12345 });  // %4 = OpConstant %3 ...
    }
    if (kind == EKind::Unsupported)
        Instruction(words, 23, { 5, 3, 2 }); // %5 = OpTypeVector %3 2

    return words;
}

static std::vector<uint32_t> Rewritten(const std::vector<uint32_t>& words)
{
    std::vector<uint32_t> output;
    std::string error;
    if (SpirvInt64Lowering::Lower(words.data(), words.size(), output, error) != SpirvInt64Lowering::EResult::Lowered)
        output.clear();
    return output;
}

static std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words)
{
    std::vector<uint8_t> bytes(words.size() * 4);
    memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

static VkResult Create(const void* code, size_t size, VkShaderModule* shaderModule, const void* next = nullptr)
{
    // The way plume does it.
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.pNext = next;
    info.pCode = reinterpret_cast<const uint32_t*>(code);
    info.codeSize = size;
    return vkCreateShaderModule(g_device, &info, &g_allocator, shaderModule);
}

static std::string CountsText()
{
    const VulkanInt64Lowering::Counts counts = VulkanInt64Lowering::GetCounts();
    return std::to_string(counts.rewritten) + " rewritten, " + std::to_string(counts.unchanged) + " unchanged, " + std::to_string(counts.failed) + " failed";
}

int main()
{
    const std::vector<uint32_t> plain = MakeModule(EKind::Plain, 7);
    const std::vector<uint32_t> wide = MakeModule(EKind::WithInt64, 7);
    const std::vector<uint32_t> unsupported = MakeModule(EKind::Unsupported, 7);
    const std::vector<uint32_t> wideRewritten = Rewritten(wide);

    Check(!wideRewritten.empty() && !HasInt64(Bytes(wideRewritten).data(), wideRewritten.size() * 4) && HasInt64(Bytes(wide).data(), wide.size() * 4),
        "the shader of this test is one the rewrite takes the 64-bit integers out of");
    Check(Rewritten(plain).empty() && Rewritten(unsupported).empty(), "and the other two are not");

    // Before Install() nothing is touched.
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    Check(Create(wide.data(), wide.size() * 4, &shaderModule) == VK_SUCCESS && g_calls.size() == 1 && g_calls[0].codePointer == wide.data(),
        "before it is installed, the driver gets what the game has");
    Check(CountsText() == "0 rewritten, 0 unchanged, 0 failed", "and nothing is counted", CountsText());
    g_calls.clear();

    VulkanInt64Lowering::Install();
    Check(vkCreateShaderModule != FakeCreateShaderModule, "installing replaces the entry point");

    const PFN_vkCreateShaderModule installed = vkCreateShaderModule;
    VulkanInt64Lowering::Install();
    Check(vkCreateShaderModule == installed, "installing twice changes nothing more");

    // A shader with 64-bit integers reaches the driver rewritten; everything else about
    // the call stays as it was.
    int nextStructure = 0;
    shaderModule = VK_NULL_HANDLE;
    VkResult result = Create(wide.data(), wide.size() * 4, &shaderModule, &nextStructure);
    Check(result == VK_SUCCESS && shaderModule != VK_NULL_HANDLE, "a shader with 64-bit integers is created");
    Check(g_calls.size() == 1, "by one call of the driver", std::to_string(g_calls.size()));
    if (g_calls.size() == 1)
    {
        const Call& call = g_calls[0];
        Check(call.code == Bytes(wideRewritten), "which gets the rewritten shader");
        Check(call.codePointer != wide.data(), "and not the game's");
        Check(call.device == g_device && call.allocator == &g_allocator && call.sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
            call.next == &nextStructure && call.flags == 0, "everything else about the call is passed on");
    }
    Check(CountsText() == "1 rewritten, 0 unchanged, 0 failed", "it is counted as rewritten", CountsText());
    g_calls.clear();

    // The game keeps shaders in byte arrays, which may start anywhere.
    std::vector<uint8_t> shifted(wide.size() * 4 + 1);
    memcpy(shifted.data() + 1, wide.data(), wide.size() * 4);
    result = Create(shifted.data() + 1, wide.size() * 4, &shaderModule);
    Check(result == VK_SUCCESS && g_calls.size() == 1 && g_calls[0].code == Bytes(wideRewritten) && uintptr_t(g_calls[0].codePointer) % 4 == 0,
        "a shader at an odd address is rewritten all the same, and the driver gets it at an even one");
    g_calls.clear();

    // A shader without 64-bit integers is not copied, let alone changed.
    result = Create(plain.data(), plain.size() * 4, &shaderModule);
    Check(result == VK_SUCCESS && g_calls.size() == 1 && g_calls[0].codePointer == plain.data() && g_calls[0].code == Bytes(plain),
        "a shader without 64-bit integers is handed on as it is");
    Check(CountsText() == "2 rewritten, 1 unchanged, 0 failed", "and counted as unchanged", CountsText());
    Check(g_errors.empty() && g_otherMessages == 0, "nothing of this is worth a line in the log");
    g_calls.clear();

    // What the driver answers is what the game hears. If it is no to a rewritten shader,
    // the log hears of it too: plume does not tell.
    g_driverResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    shaderModule = VK_NULL_HANDLE;
    result = Create(plain.data(), plain.size() * 4, &shaderModule);
    Check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY && shaderModule == VK_NULL_HANDLE && g_errors.empty(),
        "an error of the driver is passed back");

    result = Create(wide.data(), wide.size() * 4, &shaderModule);
    Check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY && shaderModule == VK_NULL_HANDLE, "also for a rewritten shader");
    Check(g_errors.size() == 1 && g_errors[0].find("refused a shader that was rewritten") != std::string::npos &&
        g_errors[0].find("(error -2, " + std::to_string(wideRewritten.size()) + " words)") != std::string::npos,
        "which the log is told, with the error", g_errors.empty() ? "" : g_errors[0]);

    for (int i = 0; i < 10; i++)
        Create(wide.data(), wide.size() * 4, &shaderModule);
    Check(g_errors.size() == 4 && g_errors[3].find("only counted") != std::string::npos, "four times and no more", std::to_string(g_errors.size()));
    Check(VulkanInt64Lowering::GetCounts().refused == 11 && VulkanInt64Lowering::GetCounts().rewritten == 13, "all of them are counted", CountsText());

    g_driverResult = VK_SUCCESS;
    g_errors.clear();
    g_calls.clear();

    // A shader that cannot be rewritten goes to the driver unchanged, and the log says why.
    result = Create(unsupported.data(), unsupported.size() * 4, &shaderModule);
    Check(result == VK_SUCCESS && g_calls.size() == 1 && g_calls[0].codePointer == unsupported.data() && g_calls[0].code == Bytes(unsupported),
        "a shader that cannot be rewritten is handed on as it is");
    Check(g_errors.size() == 1 && g_errors[0].find("vectors of 64-bit integers") != std::string::npos &&
        g_errors[0].find("could not be rewritten") != std::string::npos, "and the log says why", g_errors.empty() ? "" : g_errors[0]);
    Check(CountsText() == "13 rewritten, 2 unchanged, 1 failed", "it is counted as failed", CountsText());
    g_calls.clear();

    // So does one that is no shader at all.
    const std::vector<uint8_t> odd = { 3, 2, 35, 7, 0, 0, 1 };
    result = Create(odd.data(), odd.size(), &shaderModule);
    Check(result == VK_SUCCESS && g_calls.size() == 1 && g_calls[0].code == odd, "code that is not made of whole words is handed on as it is");
    Check(g_errors.size() == 2 && g_errors[1].find("whole number of words") != std::string::npos, "with a line in the log", g_errors.size() > 1 ? g_errors[1] : "");

    const std::vector<uint32_t> noise = { 1, 2, 3, 4, 5, 6 };
    result = Create(noise.data(), noise.size() * 4, &shaderModule);
    Check(result == VK_SUCCESS && g_calls.size() == 2 && g_calls[1].codePointer == noise.data(), "as is code that is no SPIR-V");
    Check(g_errors.size() == 3 && g_errors[2].find("not a SPIR-V module") != std::string::npos, "with its line", g_errors.size() > 2 ? g_errors[2] : "");
    g_calls.clear();

    // The log is told of the first few failures only.
    for (int i = 0; i < 40; i++)
        Create(unsupported.data(), unsupported.size() * 4, &shaderModule);
    Check(g_errors.size() == 4, "the log hears of four failures and no more", std::to_string(g_errors.size()));
    Check(g_errors.size() == 4 && g_errors[3].find("only counted") != std::string::npos && g_errors[2].find("only counted") == std::string::npos,
        "the last of them says that it is the last");
    Check(CountsText() == "13 rewritten, 2 unchanged, 43 failed", "all of them are counted", CountsText());
    Check(g_calls.size() == 40, "and all of them reach the driver");
    g_calls.clear();

    // Shaders are created by several threads at once. Each thread has shaders of its own
    // and checks that the driver got the rewrite of the very shader it handed in.
    g_record = false;
    g_callCount = 0;
    g_callsWithInt64 = 0;
    const VulkanInt64Lowering::Counts before = VulkanInt64Lowering::GetCounts();

    constexpr int ThreadCount = 8;
    constexpr int PerThread = 400;
    std::atomic<int> wrong{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < ThreadCount; t++)
    {
        threads.emplace_back([t, &wrong]
        {
            for (int i = 0; i < PerThread; i++)
            {
                const bool isWide = (i % 4) != 3;
                const std::vector<uint32_t> code = MakeModule(isWide ? EKind::WithInt64 : EKind::Plain, uint32_t(t * 100000 + i));

                VkShaderModule created = VK_NULL_HANDLE;
                if (Create(code.data(), code.size() * 4, &created) != VK_SUCCESS || created == VK_NULL_HANDLE)
                    wrong++;
            }
        });
    }

    for (std::thread& thread : threads)
        thread.join();

    const VulkanInt64Lowering::Counts after = VulkanInt64Lowering::GetCounts();
    Check(wrong == 0 && g_callCount == uint64_t(ThreadCount * PerThread), "eight threads create their shaders at once",
        std::to_string(g_callCount.load()));
    Check(g_callsWithInt64 == 0, "none of them reaches the driver with 64-bit integers", std::to_string(g_callsWithInt64.load()));
    Check(after.rewritten - before.rewritten == uint64_t(ThreadCount * PerThread * 3 / 4) &&
        after.unchanged - before.unchanged == uint64_t(ThreadCount * PerThread / 4) && after.failed == before.failed,
        "and every one of them is counted", CountsText());

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
