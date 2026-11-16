// Test double for ShadowRecomp/install/installer.h: same interface, scripted behaviour.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Installer
{
    struct Progress
    {
        float ratio = 0.0f;
        const char* currentFileName = "";
    };

    struct Sources
    {
        std::unique_ptr<int> game;
        uint64_t totalSize = 0;
    };

    using ProgressCallback = std::function<void(const Progress&)>;

    bool ParseSources(const std::filesystem::path& sourcePath, Sources& sources, std::string& errorMessage);
    bool Install(const std::filesystem::path& sourcePath, const std::filesystem::path& outputDir, const ProgressCallback& callback, std::string& errorMessage);
}
