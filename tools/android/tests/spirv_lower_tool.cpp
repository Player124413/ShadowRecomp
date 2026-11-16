// Runs gpu/spirv_int64_lowering.cpp on a file, for spirv_lowering_test.py.
//
//   spirv_lower_tool <in.spv> <out.spv>
//
// Prints "lowered", "unchanged" or "failed: <why>" and exits with 0, 0 or 1. The output
// file is only written when the module was rewritten.
//
//   spirv_lower_tool --damage <in.spv> <every>
//
// Runs the pass on damaged copies of the module: cut short after every <every>th word, and
// with every <every>th word replaced by other values in turn (1 for all of them). Prints
// "survived <number of copies>". What the pass makes of each copy is up to it; that it
// stays within its input and output is for the sanitizers this tool is built with to see.
#include <gpu/spirv_int64_lowering.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool ReadWords(const char* path, std::vector<uint32_t>& words)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
    {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }

    // Whatever is in the file is handed over, whole words of it.
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    fclose(file);

    words.resize(bytes.size() / 4);
    if (!words.empty())
        memcpy(words.data(), bytes.data(), words.size() * 4);

    return true;
}

static bool RunOnDamagedCopy(const std::vector<uint32_t>& words, size_t count)
{
    // A copy of exactly the size handed over, so that a read past it is noticed.
    std::vector<uint32_t> copy(words.begin(), words.begin() + count);
    copy.shrink_to_fit();

    std::vector<uint32_t> output;
    std::string error;
    const auto result = SpirvInt64Lowering::Lower(copy.data(), copy.size(), output, error);

    if (result == SpirvInt64Lowering::EResult::Lowered)
        return output.size() >= 5 && output[0] == 0x07230203;

    return output.empty() && (result == SpirvInt64Lowering::EResult::Failed) == !error.empty();
}

static int Damage(const char* path, size_t every)
{
    std::vector<uint32_t> words;
    if (!ReadWords(path, words) || every == 0)
        return 2;

    size_t runs = 0;
    bool consistent = true;

    for (size_t count = 0; count < words.size(); count += every, runs++)
        consistent &= RunOnDamagedCopy(words, count);

    const uint32_t bound = words.size() > 3 ? words[3] : 0;
    uint32_t random = 0x2545F491;

    for (size_t at = 0; at < words.size(); at += every)
    {
        const uint32_t original = words[at];
        random = random * 1664525u + 1013904223u;

        // Zero, small and huge numbers, ids at and beyond the edge, an instruction that
        // claims another length or is another one, and noise.
        const uint32_t replacements[] = { 0, 1, 0xFFFFFFFF, bound, bound - 1, original ^ 0x00010000, original + 0x00010000,
            original ^ 1, original + 1, random, random >> 20 };

        for (uint32_t replacement : replacements)
        {
            words[at] = replacement;
            consistent &= RunOnDamagedCopy(words, words.size());
            runs++;
        }

        words[at] = original;
    }

    if (!consistent)
    {
        printf("failed: a result did not match what was left in the output\n");
        return 3;
    }

    printf("survived %zu\n", runs);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && strcmp(argv[1], "--damage") == 0)
        return Damage(argv[2], strtoul(argv[3], nullptr, 10));

    if (argc != 3)
    {
        fprintf(stderr, "usage: spirv_lower_tool <in.spv> <out.spv> | --damage <in.spv> <every>\n");
        return 2;
    }

    std::vector<uint32_t> words;
    if (!ReadWords(argv[1], words))
        return 2;

    std::vector<uint32_t> output = { 0xDEADBEEF };
    std::string error;

    switch (SpirvInt64Lowering::Lower(words.data(), words.size(), output, error))
    {
    case SpirvInt64Lowering::EResult::Unchanged:
        if (output.size() != 1 || output[0] != 0xDEADBEEF || !error.empty())
        {
            printf("failed: an unchanged module left traces in the output\n");
            return 3;
        }
        printf("unchanged\n");
        return 0;

    case SpirvInt64Lowering::EResult::Failed:
        if (output.size() != 1 || output[0] != 0xDEADBEEF || error.empty())
        {
            printf("failed: a refused module left traces in the output or no reason\n");
            return 3;
        }
        printf("failed: %s\n", error.c_str());
        return 1;

    case SpirvInt64Lowering::EResult::Lowered:
        break;
    }

    FILE* file = fopen(argv[2], "wb");
    if (file == nullptr || fwrite(output.data(), 4, output.size(), file) != output.size() || fclose(file) != 0)
    {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }

    printf("lowered\n");
    return 0;
}
