// Runs a compute shader on a real Vulkan driver, for shader_execution_test.py: the one test
// in which shaders rewritten by gpu/spirv_int64_lowering.cpp are executed rather than read.
//
//   vulkan_compute_harness --info
//   vulkan_compute_harness <module.spv> --in <hex>[,<hex>...] --out <words>
//       [--bias0 <hex>] [--bias1 <hex>] [--pointer <index>:<offset>]...
//
// The shader gets no descriptors. As the shaders of the game do, it finds its data through
// addresses in its push constants: two 64-bit numbers, the address of an input buffer filled
// with the words given by --in, and that of an output buffer of --out words, zeroed. Both
// addresses are multiples of 16. What is pushed is each address minus its bias, so that a
// shader has to add the bias back and carries across the middle of the number can be
// forced. --pointer writes the address of the given byte of the input buffer into words
// <index> and <index> + 1 of that buffer.
//
// Prints key=value lines: device, api, int64 (whether the driver has 64-bit integers in
// shaders) and, after a run, output (the words of the output buffer).
//
// Exit codes: 0 done; 77 there is no Vulkan driver here that the test can use (the reason
// is printed); 3 the driver refused the shader; 1 anything else that went wrong.
//
// The driver is the system's Vulkan loader, or the library named by the environment
// variable VULKAN_LIBRARY, a software implementation such as SwiftShader for example.
#include <volk.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr int ExitSkipped = 77;
static constexpr int ExitRefused = 3;

[[noreturn]] static void Skip(const std::string& why)
{
    printf("skipped=%s\n", why.c_str());
    fflush(stdout);
    _Exit(ExitSkipped);
}

[[noreturn]] static void Fail(const std::string& what, VkResult result = VK_SUCCESS, int code = 1)
{
    printf("error=%s (%d)\n", what.c_str(), int(result));
    fflush(stdout);
    _Exit(code);
}

static void Expect(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Fail(what, result);
}

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t* words = nullptr;
    VkDeviceAddress address = 0;
};

static VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;

static Buffer CreateBuffer(size_t wordCount)
{
    Buffer result;

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = wordCount * sizeof(uint32_t);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Expect(vkCreateBuffer(g_device, &bufferInfo, nullptr, &result.buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(g_device, result.buffer, &requirements);

    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &memory);

    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
    {
        if ((requirements.memoryTypeBits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            type = i;
    }

    if (type == UINT32_MAX)
        Skip("the driver has no memory the test can both write and hand to a shader");

    VkMemoryAllocateFlagsInfo flagsInfo = {};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = &flagsInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = type;
    Expect(vkAllocateMemory(g_device, &allocateInfo, nullptr, &result.memory), "vkAllocateMemory");
    Expect(vkBindBufferMemory(g_device, result.buffer, result.memory, 0), "vkBindBufferMemory");

    void* mapped = nullptr;
    Expect(vkMapMemory(g_device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    result.words = static_cast<uint32_t*>(mapped);
    memset(result.words, 0, wordCount * sizeof(uint32_t));

    VkBufferDeviceAddressInfo addressInfo = {};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = result.buffer;
    result.address = vkGetBufferDeviceAddress(g_device, &addressInfo);
    if (result.address == 0)
        Fail("the driver gave a buffer the address 0");

    // The shaders promise the driver this much about where their data lies, as the game's
    // do for its registers of 16 bytes.
    if (result.address % 16 != 0)
        Skip("the driver places buffers at addresses that are not multiples of 16");

    return result;
}

static std::vector<std::string> Split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return parts;
}

int main(int argc, char** argv)
{
    bool infoOnly = false;
    const char* modulePath = nullptr;
    std::vector<uint32_t> input;
    size_t outputWords = 0;
    uint64_t bias[2] = { 0, 0 };
    std::vector<std::pair<size_t, uint64_t>> pointers;

    for (int i = 1; i < argc; i++)
    {
        const std::string argument = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;

        if (argument == "--info")
        {
            infoOnly = true;
        }
        else if (argument == "--in" && value != nullptr)
        {
            for (const std::string& word : Split(value, ','))
                input.push_back(uint32_t(strtoul(word.c_str(), nullptr, 16)));
            i++;
        }
        else if (argument == "--out" && value != nullptr)
        {
            outputWords = strtoul(value, nullptr, 10);
            i++;
        }
        else if ((argument == "--bias0" || argument == "--bias1") && value != nullptr)
        {
            bias[argument == "--bias1"] = strtoull(value, nullptr, 16);
            i++;
        }
        else if (argument == "--pointer" && value != nullptr)
        {
            const std::vector<std::string> parts = Split(value, ':');
            if (parts.size() != 2)
                Fail("--pointer takes <index>:<offset>");
            pointers.emplace_back(strtoul(parts[0].c_str(), nullptr, 10), strtoull(parts[1].c_str(), nullptr, 10));
            i++;
        }
        else if (modulePath == nullptr && argument[0] != '-')
        {
            modulePath = argv[i];
        }
        else
        {
            Fail("unknown argument " + argument);
        }
    }

    if (!infoOnly && (modulePath == nullptr || input.empty() || outputWords == 0))
        Fail("nothing to run; see the top of vulkan_compute_harness.cpp");

    for (const auto& [index, offset] : pointers)
    {
        if (index + 1 >= input.size())
            Fail("--pointer lies outside the input");
    }

    // --- The driver ---------------------------------------------------------------------
    const char* libraryName = getenv("VULKAN_LIBRARY");
    void* library = nullptr;
    if (libraryName != nullptr && libraryName[0] != '\0')
    {
        library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
            Fail(std::string("VULKAN_LIBRARY cannot be loaded: ") + dlerror());
    }
    else
    {
        library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
            library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
            Skip("no Vulkan loader is installed (libvulkan.so.1)");
    }

    const auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (getInstanceProcAddr == nullptr)
        Fail("the Vulkan library has no vkGetInstanceProcAddr");

    volkInitializeCustom(getInstanceProcAddr);

    uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
        vkEnumerateInstanceVersion(&instanceVersion);
    if (instanceVersion < VK_API_VERSION_1_2)
        Skip("the Vulkan loader is older than Vulkan 1.2");

    VkApplicationInfo applicationInfo = {};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "vulkan_compute_harness";
    applicationInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &applicationInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS)
        Skip("no Vulkan instance can be created (error " + std::to_string(int(result)) + "): no driver is installed");

    volkLoadInstanceOnly(instance);

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (deviceCount == 0 || vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) < 0)
        Skip("no Vulkan driver is installed");

    // The first device that can hand addresses of buffers to shaders.
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceFeatures2 features = {};
    VkPhysicalDeviceBufferDeviceAddressFeatures addressFeatures = {};
    uint32_t queueFamily = UINT32_MAX;
    std::string turnedDown;

    for (uint32_t i = 0; i < deviceCount && g_physicalDevice == VK_NULL_HANDLE; i++)
    {
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2)
        {
            turnedDown += std::string(properties.deviceName) + " is older than Vulkan 1.2; ";
            continue;
        }

        features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        addressFeatures = {};
        addressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        features.pNext = &addressFeatures;
        vkGetPhysicalDeviceFeatures2(devices[i], &features);

        if (!addressFeatures.bufferDeviceAddress)
        {
            turnedDown += std::string(properties.deviceName) + " has no bufferDeviceAddress; ";
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, families.data());

        queueFamily = UINT32_MAX;
        for (uint32_t family = 0; family < familyCount && queueFamily == UINT32_MAX; family++)
        {
            if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
                queueFamily = family;
        }

        if (queueFamily != UINT32_MAX)
            g_physicalDevice = devices[i];
    }

    if (g_physicalDevice == VK_NULL_HANDLE)
        Skip("no Vulkan driver here will do: " + (turnedDown.empty() ? std::string("none can run compute shaders") : turnedDown));

    printf("device=%s\n", properties.deviceName);
    printf("api=%u.%u\n", VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
    printf("int64=%d\n", features.features.shaderInt64 ? 1 : 0);
    fflush(stdout);

    if (infoOnly)
        return 0;

    // --- The device: with 64-bit integers where the driver has them, as plume creates it --
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures enabledFeatures = {};
    enabledFeatures.shaderInt64 = features.features.shaderInt64;

    VkPhysicalDeviceBufferDeviceAddressFeatures enabledAddressFeatures = {};
    enabledAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    enabledAddressFeatures.bufferDeviceAddress = VK_TRUE;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabledAddressFeatures;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    Expect(vkCreateDevice(g_physicalDevice, &deviceInfo, nullptr, &g_device), "vkCreateDevice");

    volkLoadDevice(g_device);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(g_device, queueFamily, 0, &queue);

    // --- The data -------------------------------------------------------------------------
    Buffer inputBuffer = CreateBuffer(input.size());
    Buffer outputBuffer = CreateBuffer(outputWords);

    memcpy(inputBuffer.words, input.data(), input.size() * sizeof(uint32_t));
    for (const auto& [index, offset] : pointers)
    {
        const uint64_t address = inputBuffer.address + offset;
        memcpy(inputBuffer.words + index, &address, sizeof(address));
    }

    // --- The shader -----------------------------------------------------------------------
    std::vector<uint32_t> code;
    {
        FILE* file = fopen(modulePath, "rb");
        if (file == nullptr)
            Fail(std::string("cannot read ") + modulePath);

        uint32_t word;
        while (fread(&word, sizeof(word), 1, file) == 1)
            code.push_back(word);
        fclose(file);
    }

    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size() * sizeof(uint32_t);
    moduleInfo.pCode = code.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    result = vkCreateShaderModule(g_device, &moduleInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS)
        Fail("the driver refused the shader module", result, ExitRefused);

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = 2 * sizeof(uint64_t);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    Expect(vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    result = vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE)
        Fail("the driver refused the pipeline", result, ExitRefused);

    // --- The run --------------------------------------------------------------------------
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    Expect(vkCreateCommandPool(g_device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo commandInfo = {};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;

    VkCommandBuffer commands = VK_NULL_HANDLE;
    Expect(vkAllocateCommandBuffers(g_device, &commandInfo, &commands), "vkAllocateCommandBuffers");

    const uint64_t pushed[2] = { inputBuffer.address - bias[0], outputBuffer.address - bias[1] };

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Expect(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushed), pushed);
    vkCmdDispatch(commands, 1, 1, 1);
    Expect(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands;
    Expect(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
    Expect(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    printf("output=");
    for (size_t i = 0; i < outputWords; i++)
        printf("%s%08x", i == 0 ? "" : ",", outputBuffer.words[i]);
    printf("\n");
    fflush(stdout);

    // An ending like that of the game on Android: without taking the driver down, which is
    // not what is tested here.
    _Exit(0);
}
