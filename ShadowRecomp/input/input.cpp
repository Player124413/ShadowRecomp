#include <stdafx.h>
#include <array>
#include <SDL3/SDL.h>
#include "input.h"
#include <kernel/function.h>
#include <string>
#include <app.h>
#include <sdl_events.h>
#include <os/logger.h>

#ifdef __ANDROID__
#include <ui/touch_controls.h>
#endif

input::EInputDevice input::g_inputDevice;
input::EInputDevice input::g_inputDeviceController;
input::EInputDeviceExplicit input::g_inputDeviceExplicit;

#define VIBRATION_TIMEOUT_MS 5000

// XINPUT_GAMEPAD_TRIGGER_THRESHOLD
#define DIGITAL_BUTTON_THRESHOLD 30

using input::EXboxButton;

static const std::array<input::PadBindings, 4> g_padBindings =
{{
#define PAD_BINDINGS(player) \
	{ \
		&Config::Pad##player##P_A, &Config::Pad##player##P_B, &Config::Pad##player##P_X, &Config::Pad##player##P_Y, \
		&Config::Pad##player##P_LeftTrigger, &Config::Pad##player##P_RightTrigger, \
		&Config::Pad##player##P_Start, &Config::Pad##player##P_Back, \
		&Config::Pad##player##P_DPadUp, &Config::Pad##player##P_DPadDown, \
		&Config::Pad##player##P_DPadLeft, &Config::Pad##player##P_DPadRight, \
		&Config::Pad##player##P_White, &Config::Pad##player##P_Black, \
		&Config::Pad##player##P_LeftThumb, &Config::Pad##player##P_RightThumb \
	}
	PAD_BINDINGS(1),
	PAD_BINDINGS(2),
	PAD_BINDINGS(3),
	PAD_BINDINGS(4)
#undef PAD_BINDINGS
}};

const input::PadBindings& input::GetPadBindings(uint32_t playerIndex)
{
	return g_padBindings[std::min(playerIndex, 3u)];
}

static void ApplyStickResponse(int16_t& x, int16_t& y, float deadzone, float sensitivity)
{
	if (deadzone <= 0.0f && sensitivity == 1.0f)
	{
		return;
	}

	auto fx = x / 32767.0f;
	auto fy = y / 32767.0f;
	auto magnitude = sqrtf(fx * fx + fy * fy);

	if (magnitude <= deadzone || magnitude == 0.0f)
	{
		x = 0;
		y = 0;
		return;
	}

	auto scale = ((magnitude - deadzone) / (1.0f - deadzone)) / magnitude * sensitivity;

	auto peak = std::max(fabsf(fx), fabsf(fy)) * scale;
	if (peak > 1.0f)
	{
		scale /= peak;
	}

	x = (int16_t)(fx * scale * 32767.0f);
	y = (int16_t)(fy * scale * 32767.0f);
}

bool IsInputAllowed()
{
	return GameWindow::s_isFocused || Config::AllowBackgroundInput;
}

bool IsInputDeviceController()
{
	return input::g_inputDevice != input::EInputDevice::Keyboard &&
		input::g_inputDevice != input::EInputDevice::Mouse;
}

struct PadSnapshot
{
	bool connected{};
	input::EInputDevice device{};
	input::PadButtonValues buttons{};
	int16_t leftX{};
	int16_t leftY{};
	int16_t rightX{};
	int16_t rightY{};
};

struct PadFrame
{
	std::array<PadSnapshot, 4> pads;
	bool hasActiveController{};
};

static std::mutex s_padFrameMutex;
static PadFrame s_padFrame;

static PadFrame GetPadFrame()
{
	std::lock_guard lock(s_padFrameMutex);
	return s_padFrame;
}

class Controller
{
public:
	SDL_Gamepad* controller{};
	SDL_Joystick* joystick{};
	SDL_JoystickID id{};

	Controller() = default;

	explicit Controller(SDL_JoystickID instanceID) : Controller(SDL_OpenGamepad(instanceID))
	{
	}

	Controller(SDL_Gamepad* controller) : controller(controller)
	{
		if (!controller)
			return;

		joystick = SDL_GetGamepadJoystick(controller);
		id = SDL_GetJoystickID(joystick);
	}

	SDL_GamepadType GetControllerType() const
	{
		return SDL_GetGamepadType(controller);
	}

	input::EInputDevice GetInputDevice() const
	{
		switch (GetControllerType())
		{
		case SDL_GAMEPAD_TYPE_PS3:
		case SDL_GAMEPAD_TYPE_PS4:
		case SDL_GAMEPAD_TYPE_PS5:
			return input::EInputDevice::PlayStation;
		case SDL_GAMEPAD_TYPE_XBOX360:
		case SDL_GAMEPAD_TYPE_XBOXONE:
			return input::EInputDevice::Xbox;
		default:
			return input::EInputDevice::Unknown;
		}
	}

	const char* GetControllerName() const
	{
		auto result = SDL_GetGamepadName(controller);

		if (!result)
		{
			return "Unknown Device";
		}

		return result;
	}

	void Close()
	{
		if (!controller)
			return;

		SDL_CloseGamepad(controller);

		controller = nullptr;
		joystick = nullptr;
		id = 0;
	}

	bool CanPoll()
	{
		return controller;
	}

	uint8_t GetButtonValue(EPadButton button) const
	{
		switch (button)
		{
		case EPadButton::LeftTrigger:
			return (uint8_t)std::clamp(SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, 0, 255);
		case EPadButton::RightTrigger:
			return (uint8_t)std::clamp(SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128, 0, 255);
		case EPadButton::None:
			return 0;
		default:
			return SDL_GetGamepadButton(controller, (SDL_GamepadButton)button) ? 0xFF : 0;
		}
	}

	void SetVibration(const XboxVibrationPacket& vibration)
	{
		if (!CanPoll())
			return;

		SDL_RumbleGamepad(controller, vibration.wLeftMotorSpeed, vibration.wRightMotorSpeed, VIBRATION_TIMEOUT_MS);
	}

	void SetLED(const uint8_t r, const uint8_t g, const uint8_t b) const
	{
		const float brightness = std::clamp(Config::ControllerLEDBrightness.Value, 0.0f, 1.0f);
		const auto scale = [brightness](uint8_t c) { return (uint8_t)std::lround(c * brightness); };
		SDL_SetGamepadLED(controller, scale(r), scale(g), scale(b));
	}
};

static void MapPadAxis(const PadSnapshot& snapshot, uint32_t playerIndex, XINPUT_GAMEPAD_OG& pad)
{
	pad.sThumbLX = snapshot.leftX;
	pad.sThumbLY = (int16_t)std::clamp(-(int32_t)snapshot.leftY,
		(int32_t)-32768, (int32_t)INT16_MAX); //clamp here, fix the int16 overflow with negative angle, this fix "press up" not working

	pad.sThumbRX = snapshot.rightX;
	pad.sThumbRY = (int16_t)std::clamp(-(int32_t)snapshot.rightY,
		(int32_t)-32768, (int32_t)INT16_MAX);

	auto horizontalCamera = ECameraRotationMode::Normal;
	switch (playerIndex)
	{
	case 1:  horizontalCamera = Config::HorizontalCamera2P; break;
	case 2:  horizontalCamera = Config::HorizontalCamera3P; break;
	case 3:  horizontalCamera = Config::HorizontalCamera4P; break;
	default: horizontalCamera = Config::HorizontalCamera1P; break;
	}

	if (horizontalCamera == ECameraRotationMode::Reverse)
		pad.sThumbRX = (int16_t)std::clamp(-(int32_t)pad.sThumbRX, (int32_t)-32768, (int32_t)INT16_MAX);

	float deadzone;
	float sensitivity;
	switch (playerIndex)
	{
	case 1:  deadzone = Config::StickDeadzone2P; sensitivity = Config::StickSensitivity2P; break;
	case 2:  deadzone = Config::StickDeadzone3P; sensitivity = Config::StickSensitivity3P; break;
	case 3:  deadzone = Config::StickDeadzone4P; sensitivity = Config::StickSensitivity4P; break;
	default: deadzone = Config::StickDeadzone1P; sensitivity = Config::StickSensitivity1P; break;
	}

	ApplyStickResponse(pad.sThumbLX, pad.sThumbLY, deadzone, sensitivity);
	ApplyStickResponse(pad.sThumbRX, pad.sThumbRY, deadzone, sensitivity);
}

static void MapPadButtons(const PadSnapshot& snapshot, uint32_t playerIndex, XINPUT_GAMEPAD_OG& pad)
{
	auto& bindings = input::GetPadBindings(playerIndex);

	auto getValue = [&](EXboxButton button) { return snapshot.buttons[(size_t)bindings[(size_t)button]->Value]; };
	auto isPressed = [&](EXboxButton button) { return getValue(button) > DIGITAL_BUTTON_THRESHOLD; };
	auto getDigitalValue = [&](EXboxButton button) -> uint8_t { return isPressed(button) ? 0xFF : 0; };

	pad.wButtons = 0;

	static constexpr std::pair<EXboxButton, uint16_t> digitalButtons[] =
	{
		{ EXboxButton::DPadUp, XINPUT_GAMEPAD_DPAD_UP },
		{ EXboxButton::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN },
		{ EXboxButton::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT },
		{ EXboxButton::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT },
		{ EXboxButton::Start, XINPUT_GAMEPAD_START },
		{ EXboxButton::Back, XINPUT_GAMEPAD_BACK },
		{ EXboxButton::LeftThumb, XINPUT_GAMEPAD_LEFT_THUMB },
		{ EXboxButton::RightThumb, XINPUT_GAMEPAD_RIGHT_THUMB }
	};

	for (auto& [button, mask] : digitalButtons)
	{
		if (isPressed(button))
		{
			pad.wButtons |= mask;
		}
	}

	// touchpad is back if unbound
	auto isTouchpadBound = std::any_of(bindings.begin(), bindings.end(), [](auto* binding) { return binding->Value == EPadButton::Touchpad; });

	if (!isTouchpadBound && snapshot.buttons[(size_t)EPadButton::Touchpad])
	{
		pad.wButtons |= XINPUT_GAMEPAD_BACK;
	}

	pad.bAnalogButtons[Buttons_A] = getDigitalValue(EXboxButton::A);
	pad.bAnalogButtons[Buttons_B] = getDigitalValue(EXboxButton::B);
	pad.bAnalogButtons[Buttons_X] = getDigitalValue(EXboxButton::X);
	pad.bAnalogButtons[Buttons_Y] = getDigitalValue(EXboxButton::Y);
	pad.bAnalogButtons[Buttons_White] = getDigitalValue(EXboxButton::White);
	pad.bAnalogButtons[Buttons_Black] = getDigitalValue(EXboxButton::Black);
	pad.bAnalogButtons[Buttons_LT] = getValue(EXboxButton::LeftTrigger);
	pad.bAnalogButtons[Buttons_RT] = getValue(EXboxButton::RightTrigger);
}

#ifdef __ANDROID__
static void MergeTouchControls(XINPUT_GAMEPAD_OG& pad)
{
	TouchControls::PadState touch;
	if (!TouchControls::GetPadState(touch))
		return;

	pad.wButtons |= touch.buttons;

	for (size_t i = 0; i < std::size(pad.bAnalogButtons); i++)
		pad.bAnalogButtons[i] = std::max(pad.bAnalogButtons[i], touch.analogButtons[i]);

	// A stick that is being touched wins over the physical one.
	if (touch.thumbLX != 0 || touch.thumbLY != 0)
	{
		pad.sThumbLX = touch.thumbLX;
		pad.sThumbLY = touch.thumbLY;
	}

	if (touch.thumbRX != 0 || touch.thumbRY != 0)
	{
		pad.sThumbRX = touch.thumbRX;
		pad.sThumbRY = touch.thumbRY;

		// Same meaning as for a physical stick, see MapPadAxis().
		if (Config::HorizontalCamera1P == ECameraRotationMode::Reverse)
			pad.sThumbRX = (int16_t)std::clamp(-(int32_t)pad.sThumbRX, (int32_t)-32768, (int32_t)INT16_MAX);
	}
}
#endif

std::array<Controller, 4> g_controllers;
Controller* g_activeController;
uint32_t g_keyboardPort = 1;
std::atomic<bool> g_chaosControlHoldCheat = false;
std::atomic<bool> g_chaosBlastHoldCheat = false;

class KeyboardController
{
public:
	_XINPUT_GAMEPAD_OG state{};

	static constexpr int16_t STICK_MAX = 32767;
	static constexpr int16_t STICK_MIN = -32768;

	void Poll()
	{
		const bool* keystate = SDL_GetKeyboardState(nullptr);

		state.wButtons = 0;

		if (keystate[Config::Key_LeftStickUp])
			state.sThumbLY = STICK_MAX;
		else if (keystate[Config::Key_LeftStickDown])
			state.sThumbLY = STICK_MIN;
		else
			state.sThumbLY = 0;

		if (keystate[Config::Key_LeftStickLeft])
			state.sThumbLX = STICK_MIN;
		else if (keystate[Config::Key_LeftStickRight])
			state.sThumbLX = STICK_MAX;
		else
			state.sThumbLX = 0;

		if (keystate[Config::Key_RightStickUp])
			state.sThumbRY = STICK_MAX;
		else if (keystate[Config::Key_RightStickDown])
			state.sThumbRY = STICK_MIN;
		else
			state.sThumbRY = 0;

		if (keystate[Config::Key_RightStickLeft])
			state.sThumbRX = STICK_MIN;
		else if (keystate[Config::Key_RightStickRight])
			state.sThumbRX = STICK_MAX;
		else
			state.sThumbRX = 0;

		if (keystate[Config::Key_Start])
			state.wButtons |= XINPUT_GAMEPAD_START;
		if (keystate[Config::Key_Back])
			state.wButtons |= XINPUT_GAMEPAD_BACK;
		if (keystate[Config::Key_LeftThumb])
			state.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
		if (keystate[Config::Key_RightThumb])
			state.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

		if (keystate[Config::Key_DPadUp])
			state.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
		if (keystate[Config::Key_DPadDown])
			state.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
		if (keystate[Config::Key_DPadLeft])
			state.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
		if (keystate[Config::Key_DPadRight])
			state.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

		if (keystate[Config::Key_Y])
			state.bAnalogButtons[Buttons_Y] = 0xFF;
		else
			state.bAnalogButtons[Buttons_Y] = 0;

		if (keystate[Config::Key_B])
			state.bAnalogButtons[Buttons_B] = 0xFF;
		else
			state.bAnalogButtons[Buttons_B] = 0;

		if (keystate[Config::Key_A])
			state.bAnalogButtons[Buttons_A] = 0xFF;
		else
			state.bAnalogButtons[Buttons_A] = 0;

		if (keystate[Config::Key_X])
			state.bAnalogButtons[Buttons_X] = 0xFF;
		else
			state.bAnalogButtons[Buttons_X] = 0;

		if (keystate[Config::Key_LeftBumper])
			state.bAnalogButtons[Buttons_White] = 0xFF;
		else
			state.bAnalogButtons[Buttons_White] = 0;

		if (keystate[Config::Key_RightBumper])
			state.bAnalogButtons[Buttons_Black] = 0xFF;
		else
			state.bAnalogButtons[Buttons_Black] = 0;

		if (keystate[Config::Key_LeftTrigger])
			state.bAnalogButtons[Buttons_LT] = 0xFF;
		else
			state.bAnalogButtons[Buttons_LT] = 0;

		if (keystate[Config::Key_RightTrigger])
			state.bAnalogButtons[Buttons_RT] = 0xFF;
		else
			state.bAnalogButtons[Buttons_RT] = 0;
	}
};

KeyboardController g_keyboardController;

inline Controller* EnsureController(uint32_t dwUserIndex)
{
	if (!g_controllers[dwUserIndex].controller)
		return nullptr;

	return &g_controllers[dwUserIndex];
}

inline size_t FindFreeController()
{
	for (size_t i = 0; i < g_controllers.size(); i++)
	{
		if (!g_controllers[i].controller)
			return i;
	}

	return -1;
}

inline Controller* FindController(SDL_JoystickID which)
{
	for (auto& controller : g_controllers)
	{
		if (controller.id == which)
			return &controller;
	}

	return nullptr;
}

static void SetControllerInputDevice(Controller* controller)
{
	g_activeController = controller;

	input::g_inputDevice = controller->GetInputDevice();
	input::g_inputDeviceController = input::g_inputDevice;

	auto controllerType = (input::EInputDeviceExplicit)controller->GetControllerType();
	auto controllerName = controller->GetControllerName();


	if (input::g_inputDeviceExplicit != controllerType)
	{
		input::g_inputDeviceExplicit = controllerType;

		if (controllerType == input::EInputDeviceExplicit::Unknown)
		{
			LOGFN("Detected controller: {} (Unknown Controller Type)", controllerName);
		}
		else
		{
			LOGFN("Detected controller: {}", controllerName);
		}
	}
}


static void SetControllerChaosModeLED(Controller& controller, ELedAura mode)
{
	if (!controller.controller)
		return;

	switch (mode)
	{
	case LED_AURA_NEUTRAL:
	default:
	{
		controller.SetLED(24, 24, 24);
	}
	break;
	case LED_AURA_HERO:
	{
		controller.SetLED(0, 94, 255);
	}
	break;
	case LED_AURA_VILLAIN:
	{
		controller.SetLED(237, 0, 0);
	}
	break;
	}
}

static void SetControllerPlayerColor(Controller& controller, size_t playerIndex)
{
	if (!controller.controller)
	{
		return;
	}

	static constexpr uint8_t s_playerColors[4][3] =
	{
		{   0,   0, 255 },
		{ 255,   0,   0 },
		{   0, 255,   0 },
		{ 128,   0, 128 },
	};

	const auto& color = s_playerColors[playerIndex % std::size(s_playerColors)];
	controller.SetLED(color[0], color[1], color[2]);
}

static bool s_auraDirty = true;

static void SyncControllers()
{
	for (auto& controller : g_controllers)
	{
		if (!controller.controller || SDL_GamepadConnected(controller.controller))
		{
			continue;
		}
		if (g_activeController == &controller)
		{
			g_activeController = nullptr;
		}
		controller.Close();
	}

	auto gamepadCount = 0;
	auto gamepads = SDL_GetGamepads(&gamepadCount);

	for (int i = 0; i < gamepadCount; i++)
	{
		if (FindController(gamepads[i]))
		{
			continue;
		}

		const auto freeIndex = FindFreeController();

		if (freeIndex == -1)
		{
			break;
		}

		auto controller = Controller(gamepads[i]);
		if (controller.controller)
		{
			g_controllers[freeIndex] = controller;
		}
	}

	SDL_free(gamepads);

	// a new pad was added which has no aura yet
	s_auraDirty = true;
}

// ds4 / ds5 led color aura fixes
// when touching goal ring, the async / event thread start to be used which can easily deadlock with SDLs stuff
static void ApplyControllerLEDs()
{
	static ELedAura s_appliedAura = LED_AURA_NEUTRAL;
	static float s_appliedBrightness = -1.0f;
	static bool s_appliedSync = false;

	const bool sync = Config::ChaosPowerLEDSync.Value;
	const auto aura = App::s_chaosMode.load(std::memory_order_relaxed);
	const auto brightness = Config::ControllerLEDBrightness.Value;
	const bool forced = std::exchange(s_auraDirty, false);
	if (!forced && sync == s_appliedSync && (!sync || aura == s_appliedAura) && brightness == s_appliedBrightness)
	{
		return;
	}

	s_appliedSync = sync;
	s_appliedAura = aura;
	s_appliedBrightness = brightness;
	for (size_t i = 0; i < g_controllers.size(); i++)
	{
		if (sync)
		{
			SetControllerChaosModeLED(g_controllers[i], aura);
		}
		else
		{
			SetControllerPlayerColor(g_controllers[i], i);
		}
	}
}

static void PublishPadFrame()
{
	PadFrame frame{};
	frame.hasActiveController = g_activeController != nullptr;

	for (size_t i = 0; i < g_controllers.size(); i++)
	{
		auto& controller = g_controllers[i];

		if (!controller.CanPoll())
		{
			continue;
		}

		auto& snapshot = frame.pads[i];
		snapshot.connected = true;
		snapshot.device = controller.GetInputDevice();

		for (size_t button = 0; button < snapshot.buttons.size(); button++)
		{
			snapshot.buttons[button] = controller.GetButtonValue((EPadButton)button);
		}

		snapshot.leftX = SDL_GetGamepadAxis(controller.controller, SDL_GAMEPAD_AXIS_LEFTX);
		snapshot.leftY = SDL_GetGamepadAxis(controller.controller, SDL_GAMEPAD_AXIS_LEFTY);
		snapshot.rightX = SDL_GetGamepadAxis(controller.controller, SDL_GAMEPAD_AXIS_RIGHTX);
		snapshot.rightY = SDL_GetGamepadAxis(controller.controller, SDL_GAMEPAD_AXIS_RIGHTY);
	}

	std::lock_guard lock(s_padFrameMutex);
	s_padFrame = frame;
}

void input::SetChaosAura(ELedAura mode)
{
	if ((int)mode < LED_AURA_NEUTRAL || (int)mode > LED_AURA_VILLAIN)
		mode = LED_AURA_NEUTRAL;

	App::s_chaosMode.store(mode, std::memory_order_relaxed);
}

void input::Update()
{
	MainThreadDispatcher::Get().Flush();
	ApplyControllerLEDs();
	PublishPadFrame();
}

bool Input_OnSDLEvent(void*, SDL_Event* event)
{
	switch (event->type)
	{
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
	{
		// opening or closing a pad could deadlock in specific situation so best to play safe
		MainThreadDispatcher::Get().Post(SyncControllers);

		break;
	}
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
	{
		auto* controller = FindController(event->gdevice.which);

		if (!controller)
			break;

		if (g_activeController != controller)
		{
			if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(event->gaxis.value) <= 8000)
				break;

			MainThreadDispatcher::Get().Post([id = controller->id]()
			{
				if (!GameWindow::s_isFullscreenCursorVisible)
				{
					SDL_HideCursor();
				}

				if (auto* controller = FindController(id))
				{
					SetControllerInputDevice(controller);
				}
			});
		}

		break;
	}

	case SDL_EVENT_KEY_DOWN:
		if ((event->key.mod & SDL_KMOD_CTRL) && !event->key.repeat &&
			event->key.scancode >= SDL_SCANCODE_1 && event->key.scancode <= SDL_SCANCODE_4)
		{
			g_keyboardPort = event->key.scancode - SDL_SCANCODE_1 + 1;
			LOGFN("Keyboard now controlling PORT {}", g_keyboardPort);
		}
		if (event->key.scancode == SDL_SCANCODE_F7 && !event->key.repeat && Config::FillChaosControlGaugeOnInput)
		{
			g_chaosControlHoldCheat = !g_chaosControlHoldCheat;
		}
		if (event->key.scancode == SDL_SCANCODE_F6 && !event->key.repeat && Config::FillChaosBlastGaugeOnInput)
		{
			g_chaosBlastHoldCheat = !g_chaosBlastHoldCheat;
		}
		input::g_inputDevice = input::EInputDevice::Keyboard;
		break;
	case SDL_EVENT_KEY_UP:
		input::g_inputDevice = input::EInputDevice::Keyboard;
		break;

	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
#ifdef __ANDROID__
		// SDL mirrors every finger as a mouse so that ImGui can be tapped. Those touches
		// drive the on-screen gamepad; they do not mean the player switched to a mouse.
		if ((event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which : event->button.which) == SDL_TOUCH_MOUSEID)
			break;
#endif

		if (!GameWindow::IsFullscreen() || GameWindow::s_isFullscreenCursorVisible)
			SDL_ShowCursor();

		input::g_inputDevice = input::EInputDevice::Mouse;

		break;
	}

	case SDL_EVENT_WINDOW_FOCUS_LOST:
	{
		// Stop vibrating controllers on focus lost.
		MainThreadDispatcher::Get().Post([]()
		{
			for (auto& controller : g_controllers)
			{
				controller.SetVibration({ 0, 0 });
			}
		});

		break;
	}
	}

	return false;
}

void input::Init()
{

	// needed if installer was used before playing.
	static bool s_isInit = false;
	if (s_isInit)
		return;

	s_isInit = true;

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
	SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");

	SDL_InitSubSystem(SDL_INIT_EVENTS);
	SDL_AddEventWatch(Input_OnSDLEvent, nullptr);
	SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);

	// just to ensure controller is available frame 1
	SyncControllers();
	input::Update();
}

input::EInputDevice input::GetPadDevice(uint32_t playerIndex)
{
	const auto frame = GetPadFrame();
	return playerIndex < frame.pads.size() ? frame.pads[playerIndex].device : EInputDevice::Unknown;
}

void input::GetRawPadButtons(uint32_t playerIndex, PadButtonValues& values)
{
	const auto frame = GetPadFrame();

	if (playerIndex < frame.pads.size() && frame.pads[playerIndex].connected)
	{
		values = frame.pads[playerIndex].buttons;
		return;
	}

	values.fill(0);

	for (auto& snapshot : frame.pads)
	{
		if (!snapshot.connected)
		{
			continue;
		}

		for (size_t button = 0; button < values.size(); button++)
		{
			values[button] = std::max(values[button], snapshot.buttons[button]);
		}
	}
}

uint32_t input::GetState(uint32_t dwUserIndex, XINPUT_STATE* pState)
{
	static uint32_t packet;

	if (!pState)
		return ERROR_BAD_ARGUMENTS;

	memset(pState, 0, sizeof(*pState));

	pState->dwPacketNumber = packet++;

	const auto frame = GetPadFrame();
	const auto snapshot = dwUserIndex >= 1 && dwUserIndex <= frame.pads.size() ? frame.pads[dwUserIndex - 1] : PadSnapshot{};
	bool hasController = false;
	bool isConnected = dwUserIndex == 1
		|| dwUserIndex == g_keyboardPort
		|| snapshot.connected
		|| frame.hasActiveController;

	if (IsInputAllowed())
	{
		if (snapshot.connected)
		{
			MapPadButtons(snapshot, dwUserIndex - 1, pState->Gamepad);
			MapPadAxis(snapshot, dwUserIndex - 1, pState->Gamepad);
			hasController = true;
		}

#ifdef __ANDROID__
		// The on-screen gamepad always plays as player 1, on top of a physical controller.
		if (dwUserIndex == 1)
			MergeTouchControls(pState->Gamepad);
#endif

		if (dwUserIndex == g_keyboardPort)
		{
			g_keyboardController.Poll();
			auto& kbPad = g_keyboardController.state;
			auto& pad = pState->Gamepad;

			if (kbPad.sThumbLX != 0) pad.sThumbLX = kbPad.sThumbLX;
			if (kbPad.sThumbLY != 0) pad.sThumbLY = kbPad.sThumbLY;
			if (kbPad.sThumbRX != 0) pad.sThumbRX = kbPad.sThumbRX;
			if (kbPad.sThumbRY != 0) pad.sThumbRY = kbPad.sThumbRY;

			pad.wButtons |= kbPad.wButtons;

			for (int i = 0; i < 8; i++)
			{
				if (kbPad.bAnalogButtons[i] != 0)
					pad.bAnalogButtons[i] = kbPad.bAnalogButtons[i];
			}

			return ERROR_SUCCESS;
		}

		if (hasController || dwUserIndex == 1)
			return ERROR_SUCCESS;
	}

	return isConnected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}


uint32_t input::SetState(uint32_t dwUserIndex, XboxVibrationPacket* outPacket)
{
	if (!outPacket)
		return ERROR_SUCCESS;

	bool vibrationEnabled = true;
	switch (dwUserIndex)
	{
	case 2:  vibrationEnabled = Config::Vibration2P.Value; break;
	case 3:  vibrationEnabled = Config::Vibration3P.Value; break;
	case 4:  vibrationEnabled = Config::Vibration4P.Value; break;
	default: vibrationEnabled = Config::Vibration1P.Value; break;
	}

	if (!vibrationEnabled)
		return ERROR_SUCCESS;

	const auto frame = GetPadFrame();

	if (!frame.hasActiveController)
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	if (dwUserIndex >= 1 && dwUserIndex <= frame.pads.size())
	{
		MainThreadDispatcher::Get().Post([index = dwUserIndex - 1, left = outPacket->wLeftMotorSpeed, right = outPacket->wRightMotorSpeed]()
		{
			XboxVibrationPacket vibration{};
			vibration.wLeftMotorSpeed = left;
			vibration.wRightMotorSpeed = right;
			g_controllers[index].SetVibration(vibration);
		});
	}

	return ERROR_SUCCESS;
}


HANDLE XInputOpen(int* a1, int a2)
{
	auto base = g_memory.base;

	MainThreadDispatcher::Get().Post([]()
	{
		for (auto& controller : g_controllers)
		{
			if (controller.controller)
			{
				SetControllerInputDevice(&controller);
			}
		}
	});

	for (int i = 0; i < 4; i++)
	{
		X86_STORE_U32(0x559984 + (4 * i), i + 1);
	}

	return 0;
}


GUEST_FUNCTION_HOOK_STDCALL(sub_4248A6, input::GetState);
GUEST_FUNCTION_HOOK_STDCALL(sub_424919, input::SetState);
GUEST_FUNCTION_HOOK_STDCALL(sub_2AAC20, XInputOpen);
