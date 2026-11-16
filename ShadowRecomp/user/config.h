#pragma once

#include <locale/locale.h>

#ifdef __ANDROID__
// EAndroidVulkanDriver, EAndroidRenderMode, EAndroidAudioPreset, ETouchControls, ETouchCamera.
#include <os/android/android_settings.h>
#endif


static constexpr int32_t FPS_MIN = 15;
static constexpr int32_t FPS_MAX = 240;

class IConfigDef
{
public:
    virtual ~IConfigDef() = default;
    virtual bool IsHidden() = 0;

    virtual void ReadValue(toml::v3::ex::parse_result& toml) = 0;
    virtual void MakeDefault() = 0;
    virtual std::string_view GetSection() const = 0;
    virtual std::string_view GetName() const = 0;
    virtual std::string GetNameLocalised(ELanguage language) const = 0;
    virtual std::string GetDescription(ELanguage language) const = 0;
    virtual bool IsDefaultValue() const = 0;
    virtual const void* GetValue() const = 0;
    virtual std::string GetValueLocalised(ELanguage language) const = 0;
    virtual std::string GetValueDescription(ELanguage language) const = 0;
    virtual std::string GetDefinition(bool withSection = false) const = 0;
    virtual std::string ToString(bool strWithQuotes = true) const = 0;
    virtual void GetLocaleStrings(std::vector<std::string_view>& localeStrings) const = 0;
    virtual void SnapToNearestAccessibleValue(bool searchUp) = 0;
    virtual bool RequiresRestart() = 0;
    virtual void UpdateStore() = 0;
    virtual bool IsValueChanged() = 0;
};

#define CONFIG_LOCALE            std::unordered_map<ELanguage, std::tuple<std::string, std::string>>
#define CONFIG_ENUM_LOCALE(type) std::unordered_map<ELanguage, std::unordered_map<type, std::tuple<std::string, std::string>>>

#define CONFIG_CALLBACK(name)       if (name.Callback) name.Callback(&name)
#define CONFIG_LOCK_CALLBACK(name)  if (name.LockCallback) name.LockCallback(&name)
#define CONFIG_APPLY_CALLBACK(name) if (name.ApplyCallback) name.ApplyCallback(&name)

#define WINDOWPOS_CENTRED        0x2FFF0000

extern std::vector<IConfigDef*> g_configDefinitions;


enum class ECameraRotationMode : uint32_t
{
    Normal,
    Reverse
};


enum class EControllerIcons : uint32_t
{
    Auto,
    Xbox,
    PlayStation2,
    Gamecube
};

// matches SDL_GamepadButton up to Touchpad
enum class EPadButton : uint32_t
{
    A,
    B,
    X,
    Y,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    Misc1,
    Paddle1,
    Paddle2,
    Paddle3,
    Paddle4,
    Touchpad,
    LeftTrigger,
    RightTrigger,
    None,
    Count
};

enum class EGraphicsAPI : uint32_t
{
    Auto,
#ifdef SHADOW_RECOMP_D3D12
    D3D12,
#endif
    Vulkan
};

enum class EAspectRatio : uint32_t
{
    Auto,
    Original,
    Wide16x9,
    Wide16x10,
    Narrow5x4,
    Wide21x9
};

enum class EWindowState : uint32_t
{
    Normal,
    Maximised
};

enum class ETripleBuffering : uint32_t
{
    Auto,
    On,
    Off
};

enum class EFMVAspectRatio : uint32_t
{
    Stretched,
    Fit,
    Fill
};

enum class ECutsceneFramerate : uint32_t
{
    Original,
    Unlocked
};

template<typename T, bool isHidden = false>
class ConfigDef final : public IConfigDef
{
    T m_storedValue{};

public:
    std::string Section{};
    std::string Name{};
    CONFIG_LOCALE* Locale{};
    T DefaultValue{};
    T Value{ DefaultValue };
    std::set<T> InaccessibleValues{};
    std::unordered_map<std::string, T>* EnumTemplate{};
    std::map<T, std::string> EnumTemplateReverse{};
    CONFIG_ENUM_LOCALE(T)* EnumLocale {};
    std::function<void(ConfigDef<T, isHidden>*)> Callback;
    std::function<void(ConfigDef<T, isHidden>*)> LockCallback;
    std::function<void(ConfigDef<T, isHidden>*)> ApplyCallback;
    bool IsLoadedFromConfig{};
    bool IsRestartRequired{};
    bool IsOptional{};

    // CONFIG_DEFINE
    ConfigDef(std::string section, std::string name, T defaultValue, bool requiresRestart);

    // CONFIG_DEFINE_LOCALISED
    ConfigDef(std::string section, std::string name, CONFIG_LOCALE* nameLocale, T defaultValue, bool requiresRestart);

    // CONFIG_DEFINE_ENUM
    ConfigDef(std::string section, std::string name, T defaultValue, bool requiresRestart, std::unordered_map<std::string, T>* enumTemplate);

    // CONFIG_DEFINE_ENUM_LOCALISED
    ConfigDef(std::string section, std::string name, CONFIG_LOCALE* nameLocale, T defaultValue, bool requiresRestart, std::unordered_map<std::string, T>* enumTemplate, CONFIG_ENUM_LOCALE(T)* enumLocale);

    ConfigDef(const ConfigDef&) = delete;
    ConfigDef(ConfigDef&&) = delete;
    ~ConfigDef();

    bool IsHidden() override;

    void ReadValue(toml::v3::ex::parse_result& toml) override;
    void MakeDefault() override;
    std::string_view GetSection() const override;
    std::string_view GetName() const override;
    std::string GetNameLocalised(ELanguage language) const override;
    std::string GetDescription(ELanguage language) const override;
    bool IsDefaultValue() const override;
    const void* GetValue() const override;
    std::string GetValueLocalised(ELanguage language) const override;
    std::string GetValueDescription(ELanguage language) const override;
    std::string GetDefinition(bool withSection = false) const override;
    std::string ToString(bool strWithQuotes = true) const override;
    void GetLocaleStrings(std::vector<std::string_view>& localeStrings) const override;
    void SnapToNearestAccessibleValue(bool searchUp) override;
    bool RequiresRestart() override;
    void UpdateStore() override;
    bool IsValueChanged() override;

    operator T() const
    {
        return Value;
    }

    void operator=(const T& other)
    {
        Value = other;
    }
};

#define CONFIG_DECLARE(type, name)                                                       static ConfigDef<type>       name;
#define CONFIG_DECLARE_HIDDEN(type, name)                                                static ConfigDef<type, true> name;

#define CONFIG_DEFINE(section, type, name, defaultValue, requiresRestart)                CONFIG_DECLARE(type, name)
#define CONFIG_DEFINE_HIDDEN(section, type, name, defaultValue, requiresRestart)         CONFIG_DECLARE_HIDDEN(type, name)
#define CONFIG_DEFINE_LOCALISED(section, type, name, defaultValue, requiresRestart)      CONFIG_DECLARE(type, name)
#define CONFIG_DEFINE_ENUM(section, type, name, defaultValue, requiresRestart)           CONFIG_DECLARE(type, name)
#define CONFIG_DEFINE_ENUM_LOCALISED(section, type, name, defaultValue, requiresRestart) CONFIG_DECLARE(type, name)

#define CONFIG_DEFINE_LOCALISED_OPTIONAL(section, type, name, defaultValue, requiresRestart)      CONFIG_DECLARE(type, name)
#define CONFIG_DEFINE_ENUM_OPTIONAL(section, type, name, defaultValue, requiresRestart)           CONFIG_DECLARE(type, name)
#define CONFIG_DEFINE_ENUM_LOCALISED_OPTIONAL(section, type, name, defaultValue, requiresRestart) CONFIG_DECLARE(type, name)


class Config
{
public:
#include "config_def.h"


    static inline bool s_isCallbacksCreated;

    static std::filesystem::path GetConfigPath();

    static void CreateCallbacks();
    static void Load();
    static void Save();
};

