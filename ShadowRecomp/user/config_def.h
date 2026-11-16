// This file gets included in both config.h and config.cpp, with their own macros changing
// the preprocessed output. The header is only going to have the declarations this way.

CONFIG_DEFINE_ENUM_LOCALISED("System", ELanguage, Language, ELanguage::English, true);
CONFIG_DEFINE_ENUM_LOCALISED("System", EVoiceLanguage, VoiceLanguage, EVoiceLanguage::English, false);
CONFIG_DEFINE_LOCALISED("System", bool, Subtitles, true, false);
CONFIG_DEFINE_LOCALISED("System", bool, Hints, true, false);
CONFIG_DEFINE_LOCALISED("System", bool, AchievementNotifications, true, false);
CONFIG_DEFINE_LOCALISED("System", bool, ConfirmationPrompt, true, false);
CONFIG_DEFINE_LOCALISED("System", bool, EnablePartnerIntro, true, false);
CONFIG_DEFINE("System", bool, ShowConsole, false, false);

CONFIG_DEFINE_ENUM_LOCALISED("Input", ECameraRotationMode, HorizontalCamera1P, ECameraRotationMode::Normal, false);
CONFIG_DEFINE_ENUM_LOCALISED("Input", ECameraRotationMode, HorizontalCamera2P, ECameraRotationMode::Normal, false);
CONFIG_DEFINE_ENUM_LOCALISED_OPTIONAL("Input", ECameraRotationMode, HorizontalCamera3P, ECameraRotationMode::Normal, false);
CONFIG_DEFINE_ENUM_LOCALISED_OPTIONAL("Input", ECameraRotationMode, HorizontalCamera4P, ECameraRotationMode::Normal, false);
CONFIG_DEFINE_LOCALISED("Input", bool, Vibration1P, true, false);
CONFIG_DEFINE_LOCALISED("Input", bool, Vibration2P, true, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", bool, Vibration3P, true, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", bool, Vibration4P, true, false);
CONFIG_DEFINE_LOCALISED("Input", bool, AllowBackgroundInput, false, false);
CONFIG_DEFINE_ENUM_LOCALISED("Input", EControllerIcons, ControllerIcons, EControllerIcons::Auto, true);
CONFIG_DEFINE_LOCALISED("Input", bool, ChaosPowerLEDSync, true, false);
CONFIG_DEFINE_LOCALISED("Input", float, ControllerLEDBrightness, 0.25f, false);
CONFIG_DEFINE_LOCALISED("Input", float, StickDeadzone1P, 0.0f, false);
CONFIG_DEFINE_LOCALISED("Input", float, StickDeadzone2P, 0.0f, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", float, StickDeadzone3P, 0.0f, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", float, StickDeadzone4P, 0.0f, false);
CONFIG_DEFINE_LOCALISED("Input", float, StickSensitivity1P, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Input", float, StickSensitivity2P, 1.0f, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", float, StickSensitivity3P, 1.0f, false);
CONFIG_DEFINE_LOCALISED_OPTIONAL("Input", float, StickSensitivity4P, 1.0f, false);
#ifdef __ANDROID__
// Android-only options are set in the launcher, not in the in-game menu. They must stay
// inside the block of their section: Config::Save() opens a new TOML table every time
// the section name changes, and a repeated table makes the file unparseable.
CONFIG_DEFINE_ENUM("Input", ETouchControls, TouchControls, ETouchControls::Auto, false);
CONFIG_DEFINE_ENUM("Input", ETouchCamera, TouchCamera, ETouchCamera::Swipe, false);
#endif

CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_A, SDL_SCANCODE_X, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_B, SDL_SCANCODE_C, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_X, SDL_SCANCODE_Z, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_Y, SDL_SCANCODE_S, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_DPadUp, SDL_SCANCODE_T, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_DPadDown, SDL_SCANCODE_G, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_DPadLeft, SDL_SCANCODE_F, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_DPadRight, SDL_SCANCODE_H, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_Start, SDL_SCANCODE_RETURN, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_Back, SDL_SCANCODE_BACKSPACE, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftTrigger, SDL_SCANCODE_Q, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightTrigger, SDL_SCANCODE_W, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftBumper, SDL_SCANCODE_E, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightBumper, SDL_SCANCODE_D, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftThumb, SDL_SCANCODE_COMMA, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightThumb, SDL_SCANCODE_PERIOD, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftStickUp, SDL_SCANCODE_UP, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftStickDown, SDL_SCANCODE_DOWN, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftStickLeft, SDL_SCANCODE_LEFT, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_LeftStickRight, SDL_SCANCODE_RIGHT, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightStickUp, SDL_SCANCODE_I, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightStickDown, SDL_SCANCODE_K, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightStickLeft, SDL_SCANCODE_J, false);
CONFIG_DEFINE_ENUM("Bindings", SDL_Scancode, Key_RightStickRight, SDL_SCANCODE_L, false);

#define CONFIG_DEFINE_PAD_BINDINGS(player, define) \
    define("Bindings" #player "P", EPadButton, Pad##player##P_A, EPadButton::A, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_B, EPadButton::B, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_X, EPadButton::X, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_Y, EPadButton::Y, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_LeftTrigger, EPadButton::LeftTrigger, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_RightTrigger, EPadButton::RightTrigger, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_Start, EPadButton::Start, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_Back, EPadButton::Back, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_DPadUp, EPadButton::DPadUp, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_DPadDown, EPadButton::DPadDown, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_DPadLeft, EPadButton::DPadLeft, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_DPadRight, EPadButton::DPadRight, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_White, EPadButton::LeftShoulder, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_Black, EPadButton::RightShoulder, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_LeftThumb, EPadButton::LeftStick, false); \
    define("Bindings" #player "P", EPadButton, Pad##player##P_RightThumb, EPadButton::RightStick, false);

CONFIG_DEFINE_PAD_BINDINGS(1, CONFIG_DEFINE_ENUM)
CONFIG_DEFINE_PAD_BINDINGS(2, CONFIG_DEFINE_ENUM)
CONFIG_DEFINE_PAD_BINDINGS(3, CONFIG_DEFINE_ENUM_OPTIONAL)
CONFIG_DEFINE_PAD_BINDINGS(4, CONFIG_DEFINE_ENUM_OPTIONAL)

#undef CONFIG_DEFINE_PAD_BINDINGS

CONFIG_DEFINE_LOCALISED("Audio", float, MasterVolume, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Audio", float, MusicVolume, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Audio", float, EffectsVolume, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Audio", float, VoicesVolume, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Audio", float, CutsceneVolume, 1.0f, false);
CONFIG_DEFINE_LOCALISED("Audio", bool, MuteOnFocusLost, true, false);
CONFIG_DEFINE_LOCALISED("Audio", bool, IdleChaosPowersMusic, true, false);

CONFIG_DEFINE_LOCALISED("Audio", bool, MusicAttenuation, false, false);
#ifdef __ANDROID__
CONFIG_DEFINE_ENUM("Audio", EAndroidAudioPreset, AndroidAudioPreset, EAndroidAudioPreset::Default, true);
#endif

CONFIG_DEFINE("Video", std::string, GraphicsDevice, "", true);
CONFIG_DEFINE("Video", int32_t, WindowX, WINDOWPOS_CENTRED, false);
CONFIG_DEFINE("Video", int32_t, WindowY, WINDOWPOS_CENTRED, false);
CONFIG_DEFINE("Video", int32_t, WindowWidth, 640, false);
CONFIG_DEFINE("Video", int32_t, WindowHeight, 480, false);
CONFIG_DEFINE_ENUM("Video", EWindowState, WindowState, EWindowState::Normal, false);
CONFIG_DEFINE_LOCALISED("Video", int32_t, Monitor, 0, false);
CONFIG_DEFINE_LOCALISED("Video", bool, Fullscreen, true, false);
CONFIG_DEFINE_ENUM_LOCALISED("Video", EAspectRatio, AspectRatio, EAspectRatio::Auto, false);
CONFIG_DEFINE_LOCALISED("Video", int32_t, WindowSize, -1, false);
CONFIG_DEFINE_LOCALISED("Video", float, ResolutionScale, 1.0f, false);
CONFIG_DEFINE_ENUM_LOCALISED("Video", EFMVAspectRatio, FMVAspectRatio, EFMVAspectRatio::Fit, false);
CONFIG_DEFINE_LOCALISED("Video", float, MaxHUDWidth, 0.0f, false);
CONFIG_DEFINE_LOCALISED("Video", bool, VSync, true, false);
CONFIG_DEFINE_ENUM("Video", ETripleBuffering, TripleBuffering, ETripleBuffering::Auto, false);
CONFIG_DEFINE_LOCALISED("Video", int32_t, FPS, 60, false);
CONFIG_DEFINE_ENUM_LOCALISED("Video", ECutsceneFramerate, CutsceneFramerate, ECutsceneFramerate::Original, false);
CONFIG_DEFINE_LOCALISED("Video", float, Brightness, 0.5f, false);
CONFIG_DEFINE_LOCALISED("Video", float, BloomIntensity, 0.7f, false);
CONFIG_DEFINE("Video", uint32_t, AnisotropicFiltering, 16, false);
CONFIG_DEFINE_ENUM_LOCALISED("Video", EGraphicsAPI, GraphicsAPI, EGraphicsAPI::Auto, true);
CONFIG_DEFINE_LOCALISED("Video", bool, ShowFPS, false, false);
CONFIG_DEFINE("Video", float, ProfilerFontScale, 0.7f, false);
CONFIG_DEFINE("Video", uint32_t, MaxFrameLatency, 2, false);
#ifdef __ANDROID__
CONFIG_DEFINE_ENUM("Video", EAndroidVulkanDriver, VulkanDriver, EAndroidVulkanDriver::Auto, true);
CONFIG_DEFINE_ENUM("Video", EAndroidRenderMode, RenderMode, EAndroidRenderMode::Auto, true);
// Switches for troubleshooting (docs/ANDROID.md). Hidden: they are only kept in config.toml
// once someone has written them there.
//   BackgroundPipelineCompilation = false  creates all cached pipelines before the game starts
//   PipelineDiskCache = false              does not keep the driver's pipeline cache on disk
//   ShadersWithoutInt64 = true             rewrites the shaders as for a driver without 64-bit
//                                          integers, also on a driver that has them
//   DecodeCompressedTextures = true        unpacks block-compressed textures as for a GPU that
//                                          cannot sample them, also on one that can
CONFIG_DEFINE_HIDDEN("Video", bool, BackgroundPipelineCompilation, true, true);
CONFIG_DEFINE_HIDDEN("Video", bool, PipelineDiskCache, true, true);
CONFIG_DEFINE_HIDDEN("Video", bool, ShadersWithoutInt64, false, true);
CONFIG_DEFINE_HIDDEN("Video", bool, DecodeCompressedTextures, false, true);
#endif

CONFIG_DEFINE_HIDDEN("Codes", bool, QuickBoot, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, InfiniteLives, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, AllStagesUnlocked, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, UseOfficialTitleOnTitleBar, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, EasyLastStoryUnlock, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, InfiniteAmmo, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, FillChaosControlGaugeOnInput, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, FillChaosBlastGaugeOnInput, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, VacuumAnyEnemy, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, VacuumGivePointsOnFireSuck, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, XboxEarlyTargetBehavior, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, HoldLTForceJumpDash, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, ReloadedMod, false, true);
CONFIG_DEFINE_HIDDEN("Codes", bool, ReloadedModOriginalExpertRingDropBehavior, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, ReloadedOriginalElecCodeEnemySpeed, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, ExtendedMultiplayer, false, false);
CONFIG_DEFINE_HIDDEN("Codes", uint32_t, ExtendedMultiplayerPlayerCount, 2, false);
CONFIG_DEFINE_HIDDEN("Codes", int32_t, ExtendedMultiplayerP1CharId, -1, false);
CONFIG_DEFINE_HIDDEN("Codes", int32_t, ExtendedMultiplayerP2CharId, -1, false);
CONFIG_DEFINE_HIDDEN("Codes", int32_t, ExtendedMultiplayerP3CharId, -1, false);
CONFIG_DEFINE_HIDDEN("Codes", int32_t, ExtendedMultiplayerP4CharId, -1, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, PlayAsBattleCharacters, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, ShadowAndroidsNoEmbeddedWeapons, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, SelectModeExpertStages, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, SelectModeEnableLastStoryDevilDoomSlots, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, SkipLoadingAnyEvents, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, SingleFrameInvulnRecovery, false, false);
CONFIG_DEFINE_HIDDEN("Codes", bool, AllPartnersCanHomingAttack, false, false);

CONFIG_DEFINE("Update", bool, CheckForUpdates, true, false);
CONFIG_DEFINE("Update", time_t, LastChecked, 0, false);
