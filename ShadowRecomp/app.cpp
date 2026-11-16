#include "app.h"
#include <gpu/video.h>
#include <kernel/function.h>
#include <ui/game_window.h>
#include <os/process.h>
#include <user/config.h>
#include <audio/audio.h>
#include <volk.h>
#include <user/paths.h>
#include <user/registry.h>
#include <audio/driver/sdl3_driver.h>
#include <cpu/guest_thread.h>
#include <gpu/frame_pacing/timer_resolution.h>
#include <input/input.h>
#include <patches/multiplayer_mod_patches.h>
#include <misc/rich_presence/shadow_rpc.h>

#ifdef __ANDROID__
#include <os/android/lifecycle_android.h>
#include <ui/touch_controls.h>
#endif

// NOTE: on Android this is the thread that loaded the library, not the one running main();
// UpdateSDLEventsMidAsmHook() corrects it.
static std::thread::id g_mainThreadId = std::this_thread::get_id();

void App::Restart(std::vector<std::string> restartArgs)
{
	os::process::StartProcess(os::process::GetExecutablePath(), restartArgs, os::process::GetWorkingDirectory());
	Exit();
}

void App::Exit()
{
	RichPresence::ShadowRpc::Stop();
	Video::Shutdown();
	AudioShutDown();
	Config::Save();

	TimerResolution::Release();

	if (m_TLS)
	{
		delete(m_TLS);
		m_TLS = nullptr;
	}
	std::_Exit(0);

}

bool App::isInMainMenu() // 7 is library
{
	return s_gamemode == -1 || s_gamemode == 7;
}

bool Window_OnSDLEvent(void*, SDL_Event* event);

//main game loop
X86_FUNC_IMPL(__imp__sub_57630);
X86_OVERRIDE_FUNC(sub_57630)
{
#ifdef __ANDROID__
	// Park the game while the app is in the background: its window surface is gone and
	// SDL has paused audio. See os/android/lifecycle_android.h.
	os::android::WaitWhileAppPaused();

	// Leaving a layout editor session started from the launcher returns to the launcher.
	if (TouchControls::ConsumeExitRequest())
		App::Exit();
#endif

	double targetFps = GetTargetFPS();
	if (targetFps >= FPS_MIN && targetFps < GetUnlockedFPS())
	{
		double targetDeltaTime = 1.0 / targetFps;

		auto frameTime_HHC = X86_LOAD_F32(0x541FA8);
		if (abs(frameTime_HHC - targetDeltaTime) < 0.00001)
			X86_STORE_F32(0x541FA8, targetDeltaTime);
	}

	App::s_deltaTime = X86_LOAD_F32(0x541FA8);
	App::s_time += App::s_deltaTime;
	App::s_gamemode = X86_LOAD_U32(0x5022A4);
	Video::BeginGameThreadProfiler();
	MultiplayerPatchesUpdate(ctx, base);
	__imp__sub_57630(ctx, base);
	Video::EndGameThreadProfiler();
	//std::this_thread::sleep_for(std::chrono::milliseconds(16));

}

//init
X86_FUNC_IMPL(__imp__sub_11AD0);
X86_OVERRIDE_FUNC(sub_11AD0)
{
	App::s_isInit = true;
	App::s_language = Config::Language;
	App::s_voiceLanguage = Config::VoiceLanguage;

	Registry::Save();

	__imp__sub_11AD0(ctx, base);
}

//Set Voice Language
X86_FUNC_IMPL(__imp__sub_55BE0);
X86_OVERRIDE_FUNC(sub_55BE0)
{
	__imp__sub_55BE0(ctx, base);

	X86_STORE_U32(0x54DD8C, (uint32_t)Config::VoiceLanguage.Value);
}

std::atomic<bool> shouldQuit = false; 

// We need to call SDL_PumpEvents and SDL_FlushEvents in the Main Thread of our program in a function that run every frames.
// If we don't, the game window will act unresponsive, preventing resizing, closing, etc to work.
// unfortunately Shadow immediately create a new thread in the entry point function and move on
// So we do a mid asm hook in the entry point function and put our loop there instead
void UpdateSDLEventsMidAsmHook(X86Context& __restrict ctx, uint8_t* base)
{
#ifdef __ANDROID__
	// Static initializers run on the Java thread that loaded the library, while main() and
	// this hook run on SDL's own thread, which is the one that has to pump events.
	g_mainThreadId = std::this_thread::get_id();
#endif

	SDL_Event event;
	while (!shouldQuit)
	{
		// fix unresponsive window
		if (std::this_thread::get_id() == g_mainThreadId)
		{
			MainThreadDispatcher::Get().Flush(); // This runs the callbacks needed from user's config
			SDL_PumpEvents();
			SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
			GameWindow::Update();
			input::Update();
		}

		SDL_WaitEventTimeout(&event, 8);
	}
}
