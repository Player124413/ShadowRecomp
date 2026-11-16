#include "game_window.h"
#include <gpu/video.h>

#include <app.h>
#include <sdl_listener.h>

#include <cmath>

#if _WIN32
#include <dwmapi.h>
#include <shellscalingapi.h>
#endif
#include <os/logger.h>
#include "imgui_utils.h"

#ifdef __ANDROID__
#include <os/android/lifecycle_android.h>
#include <ui/touch_controls.h>
#endif

//Original code from Unleashed Recomp

bool m_isFullscreenKeyReleased = true;
bool m_isResizing = false;

// SDL dispatches event watches on whichever thread pumps or pushes, this ends not being thread safe.
// We need to queue events and use mutex to ensure thread safety.
static std::mutex s_imguiEventMutex;
static std::vector<SDL_Event> s_imguiEvents;


static constexpr size_t IMGUI_EVENT_QUEUE_LIMIT = 4096; // arbitrary limits


static void SetCursorVisible(bool isVisible)
{
    if (isVisible)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

static SDL_DisplayID GetDisplayID(int displayIndex)
{
    SDL_DisplayID result = 0;
    auto displayCount = 0;
    auto displays = SDL_GetDisplays(&displayCount);

    if (displays && displayIndex >= 0 && displayIndex < displayCount)
        result = displays[displayIndex];

    SDL_free(displays);
    return result;
}

// ensure that we only bounds events that can potentially crash
// like if DrawImGui stops running (loading screen, stalled swap chain) while input continue to be processed
static bool IsImGuiBackendEvent(const SDL_Event& event)
{
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
        return true;

    switch (event.type)
    {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        return true;
    default:
        return false;
    }
}

void GameWindow::DrainImGuiEvents()
{
    std::vector<SDL_Event> eventsToProcess;
    {
        std::lock_guard<std::mutex> lock(s_imguiEventMutex);
        eventsToProcess.swap(s_imguiEvents);
    }

    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().BackendPlatformUserData == nullptr)
        return;

    for (auto& event : eventsToProcess)
    {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
}

bool Window_OnSDLEvent(void*, SDL_Event* event)
{
#ifdef __ANDROID__
    // Android removes the window surface while the app is in the background. SDL parks the
    // event thread and pauses audio by itself; the game loop parks on this flag.
    switch (event->type)
    {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        os::android::SetAppPaused(true);

        // Android gives no notice before it ends a process that is in the background, and
        // that is how nearly every session ends: what is to be kept is written now.
        Video::SaveCaches();
        break;

    case SDL_EVENT_DID_ENTER_BACKGROUND:
        os::android::SetAppPaused(true);
        break;

    case SDL_EVENT_DID_ENTER_FOREGROUND:
        os::android::SetAppPaused(false);
        break;
    }

    TouchControls::OnEvent(*event);
#endif

    // Queue ImGui backend events
    if (IsImGuiBackendEvent(*event))
    {
        std::lock_guard<std::mutex> lock(s_imguiEventMutex);

        if (s_imguiEvents.size() < IMGUI_EVENT_QUEUE_LIMIT)
            s_imguiEvents.push_back(*event);
    }

    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
        ImGui_ImplSDL3_ProcessEvent(event);

    for (auto listener : GetEventListeners())
    {
        if (listener->OnSDLEvent(event))
        {
            return false;
        }
    }

    switch (event->type)
    {
    case SDL_EVENT_QUIT:
    {
        if (App::s_isSaving)
            break;

        App::Exit();
    }
    break;
    case SDL_EVENT_KEY_DOWN:
    {
        switch (event->key.key)
        {
            // Toggle fullscreen on ALT+ENTER.
        case SDLK_RETURN:
        {
            if (!(event->key.mod & SDL_KMOD_ALT) || !m_isFullscreenKeyReleased)
                break;

            Config::Fullscreen = GameWindow::SetFullscreen(!GameWindow::IsFullscreen());

            if (Config::Fullscreen)
            {
                if (GameWindow::IsAllDisplays(Config::Monitor))
                    GameWindow::SetDisplay(Config::Monitor);
                else
                    Config::Monitor = GameWindow::GetDisplay();
            }
            else
            {
                Config::WindowState = GameWindow::SetMaximised(Config::WindowState == EWindowState::Maximised);
            }

            // Block holding ALT+ENTER spamming window changes.
            m_isFullscreenKeyReleased = false;
        }
        break;
        }
    }
    break;
    case SDL_EVENT_KEY_UP:
    {
        switch (event->key.key)
        {
            // Allow user to input ALT+ENTER again.
        case SDLK_RETURN:
            m_isFullscreenKeyReleased = true;
            break;
        }
    }
    break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        GameWindow::s_isFocused = false;
        SDL_ShowCursor();
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    {
        GameWindow::s_isFocused = true;

        if (GameWindow::IsFullscreen())
            SetCursorVisible(GameWindow::s_isFullscreenCursorVisible);

        break;
    }

    case SDL_EVENT_WINDOW_RESTORED:
        Config::WindowState = EWindowState::Normal;
        break;

    case SDL_EVENT_WINDOW_MAXIMIZED:
        Config::WindowState = EWindowState::Maximised;
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        m_isResizing = true;

        if (Config::WindowSize >= 0 &&
            (event->window.data1 != GameWindow::s_width ||
                event->window.data2 != GameWindow::s_height))
        {
            Config::WindowSize = -1;
        }

        GameWindow::s_width = event->window.data1;
        GameWindow::s_height = event->window.data2;
        GameWindow::SetTitle(fmt::format("{} - [{}x{}]", GameWindow::GetTitle(), GameWindow::s_width, GameWindow::s_height).c_str());
        break;

    case SDL_EVENT_WINDOW_MOVED:
        GameWindow::s_x = event->window.data1;
        GameWindow::s_y = event->window.data2;
        break;

    }

    return false;
}

void GameWindow::Init(const char* sdlVideoDriver)
{
#ifdef __linux__
    SDL_SetHint("SDL_APP_ID", "io.github.foxworkshop.shadowrecomp");
#endif

#ifdef __ANDROID__
    // The window is resizable and SDL would otherwise hand the orientation back to the
    // device's rotation lock, overriding the manifest: a phone held upright would then
    // show the game sideways.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif

    if (sdlVideoDriver)
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, sdlVideoDriver, SDL_HINT_OVERRIDE);

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO) && sdlVideoDriver)
    {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        SDL_InitSubSystem(SDL_INIT_VIDEO);
    }

    auto videoDriverName = SDL_GetCurrentVideoDriver();

    if (videoDriverName)
        LOGFN("SDL video driver: \"{}\"", videoDriverName);

    SDL_AddEventWatch(Window_OnSDLEvent, s_pWindow);

#ifdef _WIN32
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif

    s_x = Config::WindowX;
    s_y = Config::WindowY;
    s_width = Config::WindowWidth.Value;
    s_height = Config::WindowHeight.Value;

    if (s_x == -1 && s_y == -1)
        s_x = s_y = SDL_WINDOWPOS_CENTERED;

    if (!IsPositionValid())
        GameWindow::ResetDimensions();

    auto props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Shadow Recompiled");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, s_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, s_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, s_width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, s_height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, GetWindowFlags());
    s_pWindow = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

#ifdef __ANDROID__
    // The window always covers the screen, whatever size was requested.
    if (s_pWindow != nullptr)
        SDL_GetWindowSize(s_pWindow, &s_width, &s_height);
#endif

    if (IsFullscreen())
        SDL_HideCursor();

    SetDisplay(Config::Monitor);
    SetIcon((ELedAura)0);
    SetTitle();

    SDL_SetWindowMinimumSize(s_pWindow, MIN_WIDTH, MIN_HEIGHT);

    auto windowProps = SDL_GetWindowProperties(s_pWindow);

#if defined(_WIN32)
    s_renderWindow = (HWND)SDL_GetPointerProperty(windowProps, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

    /**if (Config::DisableDWMRoundedCorners)
    {
        DWM_WINDOW_CORNER_PREFERENCE wcp = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(s_renderWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &wcp, sizeof(wcp));
    }*/
#elif defined(__ANDROID__)
    // plume renders to the ANativeWindow directly. Android replaces it whenever the app
    // returns from the background; gpu/android_video.cpp follows it from there on.
    s_renderWindow = (ANativeWindow*)SDL_GetPointerProperty(windowProps, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#elif defined(PLUME_SDL_VULKAN_ENABLED)
    s_renderWindow = s_pWindow;
#elif defined(__linux__)
    s_renderWindow = { (Display*)SDL_GetPointerProperty(windowProps, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr),
        (Window)SDL_GetNumberProperty(windowProps, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0) };
#else
    static_assert(false, "Unknown platform.");
#endif

    SetTitleBarColour();

    SDL_ShowWindow(s_pWindow);
}

void GameWindow::Update()
{
    if (!GameWindow::IsFullscreen() && !GameWindow::IsMaximised() && !s_isChangingDisplay)
    {
        Config::WindowX = GameWindow::s_x;
        Config::WindowY = GameWindow::s_y;
        Config::WindowWidth = GameWindow::s_width;
        Config::WindowHeight = GameWindow::s_height;
    }

    if (m_isResizing)
    {
        SetTitle();
        g_pendingSurfaceResizeFlag = true;
        m_isResizing = false;
    }

    if (g_needsResize)
        s_isChangingDisplay = false;
}

SDL_Surface* GameWindow::GetIconSurface(void* pIconBmp, size_t iconSize)
{
    auto io = SDL_IOFromMem(pIconBmp, iconSize);
    auto surface = SDL_LoadBMP_IO(io, true);

    //if (!surface)
      //  LOGF_ERROR("Failed to load icon: {}", SDL_GetError());

    return surface;
}

void GameWindow::SetIcon(void* pIconBmp, size_t iconSize)
{
    if (auto icon = GetIconSurface(pIconBmp, iconSize))
    {
        SDL_SetWindowIcon(s_pWindow, icon);
        SDL_DestroySurface(icon);
    }
}

void GameWindow::SetIcon(ELedAura mode)
{
    switch (mode)
    {
    default:
    case LED_AURA_NEUTRAL:
        //SetIcon(g_game_icon, sizeof(g_game_icon));
        break;
    case LED_AURA_HERO:
        //SetIcon(g_game_icon_hero, sizeof(g_game_icon_hero));
        break;
    case LED_AURA_VILLAIN:
        // SetIcon(g_game_icon_vilain, sizeof(g_game_icon_vilain));
        break;
    }
}

const char* GameWindow::GetTitle()
{
    if (Config::UseOfficialTitleOnTitleBar)
    {
        return "SHADOW THE HEDGEHOG(TM)";
    }

    return "Shadow Recompiled";
}

void GameWindow::SetTitle(const char* title)
{
    SDL_SetWindowTitle(s_pWindow, title ? title : GetTitle());
}

void GameWindow::SetTitleBarColour()
{
#if _WIN32
    /**if (os::user::IsDarkTheme())
    {
        auto version = os::version::GetOSVersion();

        if (version.Major < 10 || version.Build <= 17763)
            return;

        auto flag = version.Build >= 18985
            ? DWMWA_USE_IMMERSIVE_DARK_MODE
            : 19; // DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1

        const DWORD useImmersiveDarkMode = 1;
        DwmSetWindowAttribute(s_renderWindow, flag, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode));
    }*/
#endif
}

bool GameWindow::IsFullscreen()
{
    return s_isAllScreensOption || (SDL_GetWindowFlags(s_pWindow) & SDL_WINDOW_FULLSCREEN);
}

bool GameWindow::SetFullscreen(bool isEnabled)
{
#ifdef __ANDROID__
    // There are no windows on Android: the game always covers the screen. Leaving
    // fullscreen would only bring back the system bars and desync the window size.
    return true;
#endif

    // undo borderless window before any fullscreen change
    if (s_isAllScreensOption)
    {
        s_isAllScreensOption = false;
        SDL_SetWindowBordered(s_pWindow, true);
        SDL_SetWindowResizable(s_pWindow, true);
    }
    if (isEnabled)
    {
        SDL_SetWindowFullscreen(s_pWindow, true);
        SetCursorVisible(s_isFullscreenCursorVisible);
    }
    else
    {
        SDL_SetWindowFullscreen(s_pWindow, false);
        SDL_ShowCursor();
        SetIcon((ELedAura)GameWindow::s_Icon);
        SetDimensions(Config::WindowWidth, Config::WindowHeight, Config::WindowX, Config::WindowY);
    }
    SDL_SyncWindow(s_pWindow);
    return isEnabled;
}

void GameWindow::SetFullscreenCursorVisibility(bool isVisible)
{
    s_isFullscreenCursorVisible = isVisible;

    if (IsFullscreen())
    {
        SetCursorVisible(s_isFullscreenCursorVisible);
    }
    else
    {
        SDL_ShowCursor();
    }
}

bool GameWindow::IsMaximised()
{
    return SDL_GetWindowFlags(s_pWindow) & SDL_WINDOW_MAXIMIZED;
}

EWindowState GameWindow::SetMaximised(bool isEnabled)
{
    if (isEnabled)
    {
        SDL_MaximizeWindow(s_pWindow);
    }
    else
    {
        SDL_RestoreWindow(s_pWindow);
    }

    return isEnabled
        ? EWindowState::Maximised
        : EWindowState::Normal;
}

SDL_Rect GameWindow::GetDimensions()
{
    SDL_Rect rect{};

    SDL_GetWindowPosition(s_pWindow, &rect.x, &rect.y);
    SDL_GetWindowSize(s_pWindow, &rect.w, &rect.h);

    return rect;
}

void GameWindow::GetSizeInPixels(int* w, int* h)
{
    SDL_GetWindowSizeInPixels(s_pWindow, w, h);
}

void GameWindow::SetDimensions(int w, int h, int x, int y)
{
    s_width = w;
    s_height = h;
    s_x = x;
    s_y = y;
    SDL_SetWindowSize(s_pWindow, w, h);
    SDL_ResizeEvent(s_pWindow, w, h);
    SDL_SetWindowPosition(s_pWindow, x, y);
    SDL_MoveEvent(s_pWindow, x, y);
    SDL_SyncWindow(s_pWindow);
}

void GameWindow::ResetDimensions()
{
    s_x = SDL_WINDOWPOS_CENTERED;
    s_y = SDL_WINDOWPOS_CENTERED;
    s_width = DEFAULT_WIDTH;
    s_height = DEFAULT_HEIGHT;

    Config::WindowX = s_x;
    Config::WindowY = s_y;
    Config::WindowWidth = s_width;
    Config::WindowHeight = s_height;
}

SDL_WindowFlags GameWindow::GetWindowFlags()
{
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (Config::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;

#ifdef __ANDROID__
    // There are no windows on Android; fullscreen is what hides the system bars.
    flags |= SDL_WINDOW_FULLSCREEN;
#endif

    // On Android the flag also keeps SDL from attaching an OpenGL ES surface to the window,
    // which would make it unusable for Vulkan.
#if defined(SDL_VULKAN_ENABLED) || defined(PLUME_SDL_VULKAN_ENABLED) || defined(__ANDROID__)
    flags |= SDL_WINDOW_VULKAN;
#endif

    return flags;
}

int GameWindow::GetDisplayCount()
{
    auto result = 0;
    auto displays = SDL_GetDisplays(&result);

    if (!displays)
    {
        LOGF_ERROR("Failed to get display count: {}", SDL_GetError());
        return 1;
    }

    SDL_free(displays);

    return result;
}

int GameWindow::GetDisplay()
{
    auto result = -1;
    auto displayID = SDL_GetDisplayForWindow(s_pWindow);
    auto displayCount = 0;
    auto displays = SDL_GetDisplays(&displayCount);

    for (int i = 0; i < displayCount; i++)
    {
        if (displays[i] == displayID)
        {
            result = i;
            break;
        }
    }

    SDL_free(displays);

    return result;
}

void GameWindow::SetDisplay(int displayIndex)
{
#ifdef __ANDROID__
    // Android decides which display the activity is shown on.
    return;
#endif

    if (!IsFullscreen())
        return;

    // SDL fullscreen is originally locked to one display, so use a borderless window
    // that cover every monitor instead for all screens options.
    if (IsAllDisplays(displayIndex))
    {
        if (s_isAllScreensOption)
            return;

        s_isChangingDisplay = true;

        auto bounds = GetAllDisplaysBounds();

        SDL_SetWindowFullscreen(s_pWindow, false);
        SDL_SetWindowResizable(s_pWindow, false);
        SDL_SetWindowBordered(s_pWindow, false);
        SetDimensions(bounds.w, bounds.h, bounds.x, bounds.y);

        s_isAllScreensOption = true;
        SetCursorVisible(s_isFullscreenCursorVisible);
        return;
    }

    if (displayIndex >= GetDisplayCount())
        displayIndex = 0;

    if (!s_isAllScreensOption && GetDisplay() == displayIndex)
        return;

    s_isChangingDisplay = true;

    SDL_Rect bounds;

    if (SDL_GetDisplayBounds(GetDisplayID(displayIndex), &bounds))
    {
        SetFullscreen(false);
        SetDimensions(bounds.w, bounds.h, bounds.x, bounds.y);
        SetFullscreen(true);
    }
    else
    {
        ResetDimensions();
    }
}

bool GameWindow::CanSpanDisplays()
{
    if (GetDisplayCount() < 2)
        return false;

    // likely won't work with wayland, so prevent it
    auto videoDriverName = SDL_GetCurrentVideoDriver();

    return !videoDriverName || strcmp(videoDriverName, "wayland") != 0;
}

// any index higher than the last monitor means all screens, so unplugging one still works
bool GameWindow::IsAllDisplays(int displayIndex)
{
    return displayIndex >= GetDisplayCount() && CanSpanDisplays();
}

SDL_Rect GameWindow::GetAllDisplaysBounds()
{
    SDL_Rect result{};
    auto displayCount = 0;
    auto displays = SDL_GetDisplays(&displayCount);

    for (int i = 0; i < displayCount; i++)
    {
        SDL_Rect bounds;

        if (!SDL_GetDisplayBounds(displays[i], &bounds))
            continue;

        if (result.w == 0 || result.h == 0)
            result = bounds;
        else
            SDL_GetRectUnion(&result, &bounds, &result);
    }

    SDL_free(displays);

    return result;
}

float GameWindow::GetDisplayScale()
{
    if (s_pWindow == nullptr)
        return 1.0f;

    int w, h, wInPixels, hInPixels;
    SDL_GetWindowSize(s_pWindow, &w, &h);
    SDL_GetWindowSizeInPixels(s_pWindow, &wInPixels, &hInPixels);

    if (w <= 0 || h <= 0)
        return 1.0f;

    return (float(wInPixels) / float(w) + float(hInPixels) / float(h)) * 0.5f;
}

std::vector<SDL_DisplayMode> GameWindow::GetDisplayModes(bool ignoreInvalidModes, bool ignoreRefreshRates)
{
    auto result = std::vector<SDL_DisplayMode>();
    auto uniqueResolutions = std::set<std::pair<int, int>>();
    auto displayID = SDL_GetDisplayForWindow(s_pWindow);
    auto modeCount = 0;
    auto modes = SDL_GetFullscreenDisplayModes(displayID, &modeCount);

    if (!modes)
        return result;

    for (int i = modeCount - 1; i >= 0; i--)
    {
        auto mode = *modes[i];
        mode.w = (int)lround(mode.w * mode.pixel_density);
        mode.h = (int)lround(mode.h * mode.pixel_density);

        if (ignoreInvalidModes)
        {
            if (mode.w < MIN_WIDTH || mode.h < MIN_HEIGHT)
                continue;

            if (auto desktopMode = SDL_GetDesktopDisplayMode(displayID))
            {
                auto desktopW = (int)lround(desktopMode->w * desktopMode->pixel_density);
                auto desktopH = (int)lround(desktopMode->h * desktopMode->pixel_density);

                if (mode.w >= desktopW && mode.h >= desktopH)
                    continue;
            }
        }

        if (ignoreRefreshRates)
        {
            auto res = std::make_pair(mode.w, mode.h);

            if (uniqueResolutions.find(res) == uniqueResolutions.end())
            {
                uniqueResolutions.insert(res);
                result.push_back(mode);
            }
        }
        else
        {
            result.push_back(mode);
        }
    }

    SDL_free(modes);

    return result;
}

int GameWindow::FindNearestDisplayMode()
{
    auto result = -1;
    auto displayModes = GetDisplayModes();
    auto scale = GetDisplayScale();
    auto refWidth = (int)lround(s_width * scale);
    auto refHeight = (int)lround(s_height * scale);
    auto currentDiff = std::numeric_limits<int>::max();

    for (int i = 0; i < displayModes.size(); i++)
    {
        auto& mode = displayModes[i];

        auto widthDiff = abs(mode.w - refWidth);
        auto heightDiff = abs(mode.h - refHeight);
        auto totalDiff = widthDiff + heightDiff;

        if (totalDiff < currentDiff)
        {
            currentDiff = totalDiff;
            result = i;
        }
    }

    return result;
}

bool GameWindow::IsPositionValid()
{
    auto result = false;
    auto displayCount = 0;
    auto displays = SDL_GetDisplays(&displayCount);

    for (int i = 0; i < displayCount; i++)
    {
        SDL_Rect bounds;

        if (SDL_GetDisplayBounds(displays[i], &bounds))
        {
            auto x = s_x;
            auto y = s_y;

            // Window spans across the entire display in windowed mode, which is invalid.
            if (!Config::Fullscreen && s_width == bounds.w && s_height == bounds.h)
                break;

            auto center = SDL_WINDOWPOS_CENTERED_DISPLAY(displays[i]);
            auto isPrimary = displays[i] == SDL_GetPrimaryDisplay();

            if (x == center || (isPrimary && x == SDL_WINDOWPOS_CENTERED))
                x = bounds.x + bounds.w / 2 - s_width / 2;

            if (y == center || (isPrimary && y == SDL_WINDOWPOS_CENTERED))
                y = bounds.y + bounds.h / 2 - s_height / 2;

            if (x >= bounds.x && x < bounds.x + bounds.w &&
                y >= bounds.y && y < bounds.y + bounds.h)
            {
                result = true;
                break;
            }
        }
    }

    SDL_free(displays);

    return result;
}
