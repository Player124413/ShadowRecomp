#include <ui/options_menu.h>
#include <locale/locale.h>
#include <kernel/memory.h>
#include <kernel/function.h>
#include <gpu/video.h>
#include <res/images/common/TitleBg.png.h>
#include <res/images/common/Options.png.h>
#include <res/images/common/options_icons.png.h>
#include <res/images/common/Options2.png.h>
#include <res/images/common/arrow.png.h>
#include <res/images/common/HPgauge.png.h>
#include <decompressor.h>
#include <patches/aspect_ratio_patches.h>
#include <patches/bloom_patches.h>
#include <patches/fps_patches.h>

#include "imgui_utils.h"
#include <gpu/imgui/imgui_snapshot.h>
#include <ui/fader.h>

#include <input/input.h>
#include <input/padstate.h>
#include <ui/message_window.h>
#include <ui/game_window.h>
#include <user/config.h>
#include <app.h>
#include <exports.h>
#include <ui/menu_common.h>
#include <ui/button_guide.h>
#include <patches/pause_patches.h>
#include <os/media.h>


#define OPTIONS_CATEGORY_STYLE_ICON      0
#define OPTIONS_CATEGORY_STYLE_ICON_TEXT 1

#ifndef OPTIONS_CATEGORY_STYLE
#define OPTIONS_CATEGORY_STYLE OPTIONS_CATEGORY_STYLE_ICON_TEXT
#endif

static constexpr int MAX_VISIBLE_ROWS = 5;
static constexpr ImU32 HIGHLIGHT_COLOUR = IM_COL32(255, 255, 98, 255);

// mapping based on 640x480
static constexpr float OPTION_ROW_HEIGHT = 48.0f;
static constexpr float OPTIONS_LOGO_WIDTH = 136.5f;
static constexpr float OPTIONS_LOGO_HEIGHT = 461.0f;
static constexpr float BUTTON_GUIDE_LEFT = -229.5f;
static constexpr float BUTTON_GUIDE_RIGHT = 201.5f;
static constexpr float BUTTON_GUIDE_TOP = 404.5f;

static IConfigDef* g_optionCurrent{};
static bool g_optionCanReset{};
static std::unique_ptr<GuestTexture> g_upTexOptionsBG{};
static std::unique_ptr<GuestTexture> g_upTexOptionsBG2{};

static std::unique_ptr<GuestTexture> g_upTexOptionsIcons{};
static std::unique_ptr<GuestTexture> g_upTexOptionsText{}; // flame, options logo, hud etc.
static std::unique_ptr<GuestTexture> g_upTexArrow{};
static std::unique_ptr<GuestTexture> g_upTexHpGauge{};
static ImFont* g_fntSeurat;

static int g_categoryIndex{ -1 };
static int g_optionIndex{};
static int g_optionCount{};

static bool g_livePreviewHudHidden{};
static float g_optionPreviewFade{}; // 0 shows the background, 1 hides it
static constexpr float g_options_preview_fade_frames = 24.0f;

static IConfigDef* g_previewConfig{};
static std::string g_previewValueText{};
static float g_sliderPreviewValue{};

static constexpr ConfigDef<float>* const g_stickDeadzones[] = { &Config::StickDeadzone1P, &Config::StickDeadzone2P, &Config::StickDeadzone3P, &Config::StickDeadzone4P };
static constexpr ConfigDef<float>* const g_stickSensitivities[] = { &Config::StickSensitivity1P, &Config::StickSensitivity2P, &Config::StickSensitivity3P, &Config::StickSensitivity4P };

// customize input sub menu
enum class ERemapSection
{
	Keyboard,
	Gamepad1P,
	Gamepad2P,
	Gamepad3P,
	Gamepad4P
};

enum class EBindingCaptureState
{
	Idle,
	Listening,
	WaitRelease
};

static constexpr double BINDING_CAPTURE_TIMEOUT = 5.0;
static constexpr double BINDING_RELEASE_MIN_TIME = 0.25;
static constexpr uint8_t BINDING_CAPTURE_THRESHOLD = 0x80;

static bool g_isRemapMenu{};
static int g_remapSectionIndex{};
static int g_remapReturnOptionIndex{};

static EBindingCaptureState g_captureState{};
static double g_captureTime{};
static std::array<uint8_t, SDL_SCANCODE_COUNT> g_capturePrevKeys{};
static input::PadButtonValues g_capturePrevPadButtons{};

void AudioConfigValueChangedCallback(X86Context* ctx, IConfigDef* config);

static bool IsLivePreviewActive()
{
	return OptionsMenu::s_isPause && OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected
		&& (g_optionCurrent == &Config::Brightness || g_optionCurrent == &Config::BloomIntensity
			|| g_optionCurrent == &Config::AspectRatio || g_optionCurrent == &Config::MaxHUDWidth);
}

static ConfigDef<float>* GetPreviewGaugeConfig(IConfigDef* config)
{
	if (config == &Config::Brightness)
	{
		return &Config::Brightness;
	}
	if (config == &Config::BloomIntensity)
	{
		return &Config::BloomIntensity;
	}
	return nullptr; // anything that should be values (non boss csd style sliders)
}

void ToggleHud(bool toggle);
static void BeginPauseHudHide()
{
	auto base = g_memory.base;

	// skip this for MaxHUDWidth so the preview still renders the hud
	if (!g_livePreviewHudHidden && IsLivePreviewActive() && g_optionCurrent != &Config::MaxHUDWidth && X86_LOAD_U8(0x54DE78) != 0)
	{
		ToggleHud(false);
		g_livePreviewHudHidden = true;
	}
}

// small transition for options that hide the background
static void UpdateLivePreviewFade()
{
	auto step = ImGui::GetIO().DeltaTime * 60.0f / g_options_preview_fade_frames;

	if (IsLivePreviewActive())
		g_optionPreviewFade = std::min(g_optionPreviewFade + step, 1.0f);
	else
		g_optionPreviewFade = std::max(g_optionPreviewFade - step, 0.0f);


	if (IsLivePreviewActive())
	{
		auto gaugeConfig = GetPreviewGaugeConfig(g_optionCurrent);

		if (gaugeConfig && g_previewConfig != g_optionCurrent)
			g_sliderPreviewValue = gaugeConfig->Value;

		g_previewConfig = g_optionCurrent;
	}
	else if (g_optionPreviewFade <= 0.0f)
	{
		g_previewConfig = nullptr;
	}
}

static float GetOptionPreviewFade()
{
	auto t = g_optionPreviewFade;
	return t * t * (3.0f - 2.0f * t);
}

// Used when the whole options menu fades out while a preview is shown alone
static float GetMenuAlpha()
{
	return g_previewConfig ? 1.0f - GetOptionPreviewFade() : 1.0f;
}

static void EndPauseHudHide()
{
	if (g_livePreviewHudHidden)
	{
		ToggleHud(true);
		g_livePreviewHudHidden = false;
	}
}

static double g_stateTime{};
static double g_categoryTime{};
static double g_cursorArrowsTime{};
static double g_flowStateTime{};
static double g_lastIncrementTime{};
static double g_lastTappedTime{};
static ScrollArrows g_scrollArrows{};
static double g_containerAnimTime = 0.0;
static double g_containerClosedAnimTime = 0.0;
static double g_transitionTime{};
static double g_logoTime{};

// Open and close timings at 60 fps
// (Can be adjusted to make the menu feel smoother or slower)
static constexpr double g_options_container_frames = 15.0;
static constexpr double g_options_logo_close_delay = g_options_container_frames;
static constexpr float g_options_logo_alpha = 152.0f;

static double GetFramesSince(double time)
{
	return (ImGui::GetTime() - time) * 60.0;
}

static bool g_up{};
static bool g_upWasHeld{};
static bool g_down{};
static bool g_downWasHeld{};
static bool g_left{};
static bool g_leftWasHeld{};
static bool g_right{};
static bool g_rightWasHeld{};
static bool g_isAccepted{};
static bool g_isDeclined{};
static bool g_isLeaving{};
static bool g_isReset{};
static bool g_isControlsVisible = false;

bool m_isDescScrolling{};
bool m_isDescManualScrolling{};
float m_descScrollOffset{};
float m_descScrollTimer{};
float m_descScrollDirection{ 1.0f };

static std::string optionDescription;
static void UpdateOptionDescription(const IConfigDef* config, const std::string description)
{
	optionDescription = description;
	if (OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected)
	{
		const auto valueDescription = config->GetValueDescription(Config::Language);
		if (!valueDescription.empty())
		{
			optionDescription = valueDescription;
		}
	}
}

static std::string& GetCategoryName(OptionsMenuCategory category)
{
	switch (category)
	{
	case OptionsMenuCategory::System: return Localise("Options_Category_System");
	case OptionsMenuCategory::Input: return Localise("Options_Category_Input");
	case OptionsMenuCategory::Audio: return Localise("Options_Category_Audio");
	case OptionsMenuCategory::Video: return Localise("Options_Category_Video");
	}

	return g_localeMissing;
}

static std::string& GetCategoryDescription(OptionsMenuCategory category)
{
	switch (category)
	{
	case OptionsMenuCategory::System: return Localise("Options_Desc_Category_System");
	case OptionsMenuCategory::Input: return Localise("Options_Desc_Category_Input");
	case OptionsMenuCategory::Audio: return Localise("Options_Desc_Category_Audio");
	case OptionsMenuCategory::Video: return Localise("Options_Desc_Category_Video");
	}

	return g_localeMissing;
}

static constexpr int ICON_ATLAS_WIDTH = 165;
static constexpr int ICON_ATLAS_HEIGHT = 494;
static constexpr int ICON_CELL_SIZE = 82;
static constexpr int ICON_CELL_PITCH = 83;

enum
{
	ICON_ROW_INPUT = 1,
	ICON_ROW_KEYBOARD = 5
};

static constexpr float LEGACY_ICON_SIZE = 79.0f;
static constexpr float LEGACY_ICON_PITCH = 92.0f;
static constexpr float LEGACY_ICON_SELECTED_SCALE = 1.05f;

static constexpr float INLINE_ICON_SIZE = 36.0f;
static constexpr float INLINE_ICON_GAP = 10.0f;

static constexpr float CATEGORY_COLUMN_CENTRE_X = 171.0f;
static constexpr float CATEGORY_TOP = 137.5f;

static constexpr float OPTIONS_PANEL_WIDTH_RATIO = 0.55f;

static constexpr float CATEGORY_EDGE_MARGIN = 12.0f;


static constexpr float CATEGORY_STACK_CENTRE_Y = 250.0f;
static constexpr float CATEGORY_STACK_HEIGHT = 320.0f;

static int GetCategoryIconRow(OptionsMenuCategory category)
{
	switch (category)
	{
	case OptionsMenuCategory::System: return 0;
	case OptionsMenuCategory::Input:  return 1;
	case OptionsMenuCategory::Audio:  return 2;
	case OptionsMenuCategory::Video:  return 3;
		//case OptionsMenuCategory::Misc:   return 4; 
	case OptionsMenuCategory::Debug:  return 0;
	}

	return 0;
}

static std::tuple<ImVec2, ImVec2> GetCategoryIconUVs(int row, bool isCurrent)
{
	const int cellX = isCurrent ? ICON_CELL_PITCH : 0;
	const int cellY = row * ICON_CELL_PITCH;
	const int cellHeight = std::min(ICON_CELL_SIZE, ICON_ATLAS_HEIGHT - cellY);

	return PIXELS_TO_UV_COORDS(ICON_ATLAS_WIDTH, ICON_ATLAS_HEIGHT, cellX + 0.5f, cellY + 0.5f, ICON_CELL_SIZE - 1.0f, cellHeight - 1.0f);
}

static bool IsCategoryVisible(OptionsMenuCategory category)
{
	// Don't show debug option if locked
	if (!OptionsMenu::s_isDebugUnlocked && category == OptionsMenuCategory::Debug)
		return false;


	/**if (category == OptionsMenuCategory::Misc)
		return false;*/

	return true;
}


static float ClampBand(float left, float width, float bandLeft, float bandRight)
{
	if (left + width > bandRight)
		left = bandRight - width;

	if (left < bandLeft)
		left = bandLeft;

	return left;
}

static bool CheckAndDiscard(bool& value)
{
	if (value)
	{
		value = false;
		return true;
	}

	return false;
}



void resetDescScroll()
{
	m_isDescScrolling = false;
	m_descScrollOffset = 0.0f;
	m_descScrollTimer = 0.0f;
	m_descScrollDirection = 1.0f;
}


void DrawDescription(std::string s, const double scrollDelay)
{
	static std::string prevText;
	static double scrollStartTime = 0.0;
	double elapsed = ImGui::GetTime() - scrollStartTime;

	std::replace(s.begin(), s.end(), '\n', ' ');

	if (prevText != s)
	{
		prevText = s;
		scrollStartTime = ImGui::GetTime() + scrollDelay;
	}

	auto drawList = ImGui::GetBackgroundDrawList();
	auto& res = ImGui::GetIO().DisplaySize;

	// First we calculate the position of the black bar of the menu BG
	float barTexX = 74.0f;
	float barTexY = 38.f;
	float barTexH = 48.f;
	float texHeight = 512.0f;

	// Then we map to screen space proportionally
	float boxLeft = GetMenuBoxLeft();
	float boxWidth = GetMenuBoxWidth();
	float barScreenX = boxLeft + (barTexX / texHeight) * boxWidth;
	float barScreenY = (barTexY / texHeight) * res.y;
	float barScreenH = (barTexH / texHeight) * res.y;

	ImVec2 barMin = { barScreenX + Scale(30), barScreenY };
	ImVec2 barMax = { boxLeft + boxWidth, barScreenY + barScreenH };

	drawList->PushClipRect(barMin, barMax);


	float fontSize = g_fntSeuratSize - 2.f;
	auto textSize = g_fntSeurat->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s.c_str());

	float barWidth = barMax.x - barScreenX;


	ImVec2 textPos = {
		barScreenX + barWidth * 0.5f - textSize.x * 0.5f + Scale(15.f),  // centered within bar bounds
		barScreenY + barScreenH * 0.6f - textSize.y * 0.5f
	};

	float offsetScroll = 0.0f;
	float totalWidth = 0.0f;
	bool isScrolling = textSize.x > barWidth - Scale(55.f);

	if (isScrolling)
	{
		float gap = Scale(80.0f);
		totalWidth = textSize.x + gap;
		float scrollSpeed = Scale(80.f);

		// Anchor to left edge of the bar so scroll always starts at beginning of text
		textPos.x = barMin.x + Scale(15.f);
		offsetScroll = elapsed > 0.0 ? fmod(elapsed * scrollSpeed, totalWidth) : 0.0f;
	}

	textPos.x -= offsetScroll;

	// The black bar vanishes during option preview,
	// so add an outline to keep the text readable.
	auto outlineAlpha = (int)(255 * GetOptionPreviewFade());

	auto drawText = [&](ImVec2 pos)
		{
			if (outlineAlpha > 0)
				DrawTextWithOutline(g_fntSeurat, fontSize, pos, IM_COL32(255, 255, 255, 255), s.c_str(), 2.f, IM_COL32(0, 0, 0, outlineAlpha));
			else
				drawList->AddText(g_fntSeurat, fontSize, pos, IM_COL32(255, 255, 255, 255), s.c_str());
		};

	drawText(textPos);

	// Draw the looping copy trailing behind
	if (isScrolling)
		drawText({ textPos.x + totalWidth, textPos.y });

	drawList->PopClipRect();
}

struct ScrollTextWorker
{
	int Row = -1;
	std::string Text{};
	int LastFrame = -2;
	double StartTime{};
};

static float GetTextScrollOffset(int row, const std::string& text, float loopWidth, float speed, double pauseTime)
{
	static ScrollTextWorker textWK{};
	auto frame = ImGui::GetFrameCount();
	auto time = ImGui::GetTime();

	if (textWK.Row != row || textWK.Text != text || textWK.LastFrame < frame - 1)
	{
		textWK.Row = row;
		textWK.Text = text;
		textWK.StartTime = time;
	}

	textWK.LastFrame = frame;

	if (loopWidth <= 0.0f || speed <= 0.0f)
		return 0.0f;

	// wait once when the cursor arrive, then loop scrolling.
	double elapsed = time - textWK.StartTime - pauseTime;

	if (elapsed <= 0.0)
		return 0.0f;

	return (float)std::fmod(elapsed * speed, (double)loopWidth);
}

//Returns truncated string with ellipsis if it exceeds maxWidth
static std::string TruncateTextWithEllipsis(ImFont* font, float fontSize, const std::string& text, float maxWidth)
{
	auto fullSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
	if (fullSize.x <= maxWidth)
		return text;

	const std::string ellipsis = "...";
	auto ellipsisSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ellipsis.c_str());
	float budget = maxWidth - ellipsisSize.x;

	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); )
	{
		// Advance by one UTF-8 codepoint
		unsigned char c = (unsigned char)text[i];
		size_t charLen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
		std::string candidate = result + text.substr(i, charLen);
		auto candidateSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, candidate.c_str());
		if (candidateSize.x > budget)
			break;
		result = std::move(candidate);
		i += charLen;
	}
	return result + ellipsis;
}

static void DrawMenuText(float fontSize, ImVec2 pos, ImU32 colour, const char* text, bool isHighlighted = false, float shadowOffset = 2.0f)
{
	auto shadowColour = isHighlighted ? IM_COL32(0, 0, 93, 255) : IM_COL32(0, 0, 0, 255);
	DrawTextWithShadow(g_fntSeurat, fontSize, pos, colour, text, shadowOffset, 1.0f, shadowColour);
}

void DrawSelectionArrows(ImVec2 min, ImVec2 max, bool isSelected)
{
	if (!isSelected)
		return;

	auto drawList = ImGui::GetBackgroundDrawList();

	static bool s_isLeftArrowMotion = false;
	static bool s_isRightArrowMotion = false;

	auto motionTime = (s_isLeftArrowMotion || s_isRightArrowMotion)
		? ComputeLinearMotion(g_lastTappedTime, 0, 15)
		: 0;

	if (g_left)
	{
		s_isLeftArrowMotion = true;
		s_isRightArrowMotion = false;
	}

	if (g_right)
	{
		s_isLeftArrowMotion = false;
		s_isRightArrowMotion = true;
	}

	if (motionTime >= 1.0)
	{
		s_isLeftArrowMotion = false;
		s_isRightArrowMotion = false;
	}

	auto arrowUVs = PIXELS_TO_UV_COORDS(48, 48, 0, 0, 24, 24);

	auto arrowWidth = Scale(24);
	auto arrowHeight = Scale(24);

	auto arrowOffsetXMin = Scale(10);
	auto arrowOffsetXMax = Scale(1);

	auto arrowOffsetX = Lerp(arrowOffsetXMin, arrowOffsetXMax, sin(motionTime * M_PI));
	auto arrowOffsetY = Scale(5);

	auto arrowLeftOffsetX = s_isLeftArrowMotion ? arrowOffsetX : arrowOffsetXMin;
	auto arrowRightOffsetX = s_isRightArrowMotion ? arrowOffsetX : arrowOffsetXMin;

	ImVec2 arrowLeftMin = { min.x + arrowLeftOffsetX, min.y + arrowOffsetY };
	ImVec2 arrowLeftMax = { arrowLeftMin.x + arrowWidth, arrowLeftMin.y + arrowHeight };
	ImVec2 arrowRightMin = { max.x - arrowWidth - arrowRightOffsetX, arrowLeftMin.y };
	ImVec2 arrowRightMax = { arrowRightMin.x + arrowWidth, arrowLeftMax.y };

	// Draw left arrow.
	drawList->AddImage(g_upTexArrow.get(), arrowLeftMin, arrowLeftMax, GET_UV_COORDS(arrowUVs), IM_COL32_WHITE);

	arrowUVs = PIXELS_TO_UV_COORDS(48, 48, 24, 0, 24, 24);
	// Draw right arrow.
	drawList->AddImage(g_upTexArrow.get(), arrowRightMin, arrowRightMax, GET_UV_COORDS(arrowUVs));
};

static int g_optionScrollTop{};
static int GetOptionScrollTop()
{
	if (g_optionIndex > g_optionScrollTop + MAX_VISIBLE_ROWS - 2)
		g_optionScrollTop = g_optionIndex - (MAX_VISIBLE_ROWS - 2);

	if (g_optionIndex < g_optionScrollTop + 1)
		g_optionScrollTop = g_optionIndex - 1;

	g_optionScrollTop = std::clamp(g_optionScrollTop, 0, std::max(g_optionCount - MAX_VISIBLE_ROWS, 0));

	return g_optionScrollTop;
}

static float GetRowScrollOffset(float optionHeight)
{
	return -GetOptionScrollTop() * optionHeight;
}

static void StepMaxHudWidth(X86Context* ctx, bool increment, bool decrement)
{
	if (!increment && !decrement)
	{
		return;
	}

	constexpr int count = int(std::size(g_hudWidthPresets));
	int preset = FindHudWidthPreset(Config::MaxHUDWidth.Value);
	if (decrement)
	{
		preset = (preset <= 0 ? count : preset) - 1;
	}
	else
	{
		preset = (preset + 1) % count;
	}

	Config::MaxHUDWidth.Value = g_hudWidthPresets[preset].AspectRatio;
	Game_PlaySound(ctx, "cursor");
}

template <typename T, bool isHidden = false>
static void DrawOption
(
	X86Context* ctx,
	int rowIndex,
	ConfigDef<T, isHidden>* config,
	bool isAccessible,
	ImVec2 min,
	ImVec2 max,
	std::string* inaccessibleReason = nullptr,
	T valueMin = T(0),
	T valueCentre = T(0.5),
	T valueMax = T(1),
	bool isSlider = true,
	bool isInterpolatedString = false
)
{
	auto drawList = ImGui::GetBackgroundDrawList();
	auto clipRectMin = drawList->GetClipRectMin();
	auto clipRectMax = drawList->GetClipRectMax();

	auto optionHeight = Scale(OPTION_ROW_HEIGHT);
	// original offset from the container
	float optionsOffsetX = Scale(170);
	float optionsOffsetY = Scale(23);

	// removing offset will make option draw right when the container starts, so we add an extra scale to make it fit inside.
	min.x -= optionsOffsetX - Scale(10);
	max.x -= Scale(10);

	auto optionDescription = isAccessible || !inaccessibleReason ? config->GetDescription(Config::Language) : *inaccessibleReason;
	auto offsetY = optionHeight * rowIndex + GetRowScrollOffset(optionHeight);
	auto isCurrent = g_optionIndex == rowIndex;
	auto isSelected = isCurrent && OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected;
	auto isHover = isCurrent && OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor;
	bool isHoverOrSelected = isSelected || isHover;

	constexpr bool isSliderType = std::is_same_v<T, float> || std::is_same_v<T, int>;

	auto bgEdgeUVs = PIXELS_TO_UV_COORDS(256, 256, 1, 0, 51, 45);
	auto bgStretchUVs = PIXELS_TO_UV_COORDS(256, 256, 51, 0, 51, 45);
	auto bgWidth = Scale(50);
	auto bgHeight = Scale(45);

	ImVec2 titleBgEdgeMin = { min.x, min.y + offsetY };
	ImVec2 titleBgEdgeMax = { titleBgEdgeMin.x + bgWidth, titleBgEdgeMin.y + bgHeight };
	ImVec2 titleBgStretchMin = { titleBgEdgeMax.x, titleBgEdgeMin.y };
	ImVec2 titleBgStretchMax = { max.x, max.y };
	ImVec2 titleBgFadeMin = { titleBgStretchMax.x - Scale(300), titleBgStretchMax.y };
	ImVec2 titleBgFadeMax = titleBgStretchMax;

	// value variables
	float valueAreaWidth = Scale(105);
	float valueAreaEnd = max.x - Scale(15);
	float valueAreaStart = valueAreaEnd - valueAreaWidth;
	float valueAreaCenter = valueAreaStart + (valueAreaWidth * 0.5f);

	// options name variables
	const float fontOptionSize = g_fntSeuratSize - 4.f;
	auto titleText = config->GetNameLocalised(Config::Language);
	auto titleTextSize = g_fntSeurat->CalcTextSizeA(fontOptionSize, FLT_MAX, 0, titleText.c_str());
	ImVec2 titlePos = { titleBgEdgeMin.x, titleBgEdgeMin.y + Scale(8) };
	auto titleAreaStart = titlePos.x;
	auto titleAreaEnd = valueAreaStart - Scale(10); // leave a gap before the value column
	auto titleAreaWidth = titleAreaEnd - titleAreaStart;
	bool titleOverflows = titleTextSize.x > titleAreaWidth;

	auto TexColor = isHoverOrSelected ? HIGHLIGHT_COLOUR : IM_COL32(255, 255, 255, 255);

	if (titleOverflows && isHover)
	{
		const float loopWidth = titleTextSize.x + Scale(50);
		float scrollX = GetTextScrollOffset(rowIndex, titleText, loopWidth, Scale(80), 1.0);

		drawList->PushClipRect({ titleAreaStart, clipRectMin.y }, { titleAreaEnd, clipRectMax.y }, true);
		ImVec2 scrolledTitlePos = { titlePos.x - scrollX, titlePos.y };

		DrawMenuText(fontOptionSize, scrolledTitlePos, TexColor, titleText.c_str(), isHoverOrSelected);
		DrawMenuText(fontOptionSize, { scrolledTitlePos.x + loopWidth, scrolledTitlePos.y }, TexColor, titleText.c_str(), isHoverOrSelected);

		drawList->PopClipRect();
	}
	else
	{
		// not hovered or fits fine: truncate with ellipsis as before
		std::string displayTitle = titleOverflows
			? TruncateTextWithEllipsis(g_fntSeurat, g_fntSeuratSize, titleText, titleAreaWidth)
			: titleText;

		DrawMenuText(fontOptionSize, titlePos, TexColor, displayTitle.c_str(), isHoverOrSelected);
	}

	if (isCurrent)
	{
		if (OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected)
		{

			g_optionCurrent = config;

			g_optionCanReset = isAccessible && config->GetName().find("Language") == std::string::npos && (void*)config != (void*)&Config::WindowSize;
		}
		else
		{
			g_optionCurrent = nullptr;
			g_optionCanReset = false;
		}


		if (OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor || OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected)
		{
			
			UpdateOptionDescription(config, optionDescription);

			if constexpr (std::is_same_v<T, float>)
			{
				if (config == &Config::ResolutionScale)
				{

					auto resScale = round(*(float*)config->GetValue() * 1000) / 1000;

					auto valueText = fmt::format("({}x{})",
						(int)((float)Video::s_viewportWidth * resScale),
						(int)((float)Video::s_viewportHeight * resScale));

					optionDescription += valueText;
				}
			}
		}


		if (isAccessible)
		{
			static T s_oldValue;
			if (CheckAndDiscard(g_isAccepted))
			{
				Game_PlaySound(ctx, "decide");
				if (OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor)
				{
					g_leftWasHeld = false;
					g_rightWasHeld = false;

					if (config->LockCallback)
					{
						config->LockCallback(config);
					}

					OptionsMenu::SetFlowState(OptionsMenuFlowState::OptionSelected);
					UpdateOptionDescription(config, optionDescription);

					isSelected = true;
					s_oldValue = config->Value;
					g_optionCurrent = config;
					BeginPauseHudHide();
				}
				else
				{
					if (config->Value != s_oldValue)
					{
						VideoConfigValueChangedCallback(config);
						AudioConfigValueChangedCallback(ctx, config);

						if (config->ApplyCallback)
						{
							config->ApplyCallback(config);
						}
					}

					OptionsMenu::SetFlowState(OptionsMenuFlowState::OptionCursor);

					isSelected = false;
					EndPauseHudHide();
				}
			}
			else if (CheckAndDiscard(g_isDeclined))
			{
				Game_PlaySound(ctx, "cancel");

				if (config->Value != s_oldValue)
				{
					// released lock, restore old value
					config->Value = s_oldValue;

					BloomConfigValueChangedCallback(config);
					VideoConfigValueChangedCallback(config);

					if (config->Callback)
						config->Callback(config);

					if (config->ApplyCallback)
						config->ApplyCallback(config);
				}

				OptionsMenu::SetFlowState(OptionsMenuFlowState::OptionCursor);

				isSelected = false;
				EndPauseHudHide();
			}

			if (g_optionCanReset && CheckAndDiscard(g_isReset))
			{
				Game_PlaySound(ctx, "cancel");

				if (!config->IsDefaultValue())
				{
					config->MakeDefault();

					BloomConfigValueChangedCallback(config);
					VideoConfigValueChangedCallback(config);

					if (config->Callback)
						config->Callback(config);

					if (config->ApplyCallback)
						config->ApplyCallback(config);
				}
			}

			if (isSelected)
			{
				auto increment = g_right;
				auto decrement = g_left;

				if (increment || decrement)
					g_lastTappedTime = ImGui::GetTime();

				if constexpr (std::is_same_v<T, int32_t>)
				{
					if (config == &Config::FPS)
					{
						if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_WHITE | PJSPeripheral::BTN_LEFT_TRIG))
						{
							Game_PlaySound(ctx, "cursor");
							config->Value = config->Value >= valueMax ? valueMax - 1 : std::max(config->Value - 30, valueMin);
							FpsConfigValueChangedCallback(config);
						}
						else if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_BLACK | PJSPeripheral::BTN_RIGHT_TRIG))
						{
							Game_PlaySound(ctx, "cursor");
							config->Value = config->Value >= valueMax - 1 ? valueMax : std::min(config->Value + 30, valueMax - 1);
							FpsConfigValueChangedCallback(config);
						}
					}
				}

				if constexpr (std::is_same_v<T, float>)
				{
					if (config == &Config::MasterVolume || config == &Config::MusicVolume || config == &Config::VoicesVolume ||
						config == &Config::CutsceneVolume || config == &Config::EffectsVolume || config == &Config::ResolutionScale ||
						config == &Config::Brightness || config == &Config::BloomIntensity || config == &Config::ControllerLEDBrightness ||
						std::ranges::find(g_stickDeadzones, config) != std::end(g_stickDeadzones) ||
						std::ranges::find(g_stickSensitivities, config) != std::end(g_stickSensitivities))
					{
						if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_WHITE | PJSPeripheral::BTN_LEFT_TRIG))
						{
							Game_PlaySound(ctx, "cursor");
							config->Value = std::clamp(std::round((config->Value - 0.1f) * 100.0f) / 100.0f, valueMin, valueMax);
							BloomConfigValueChangedCallback(config);
						}
						else if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_BLACK | PJSPeripheral::BTN_RIGHT_TRIG))
						{
							Game_PlaySound(ctx, "cursor");
							config->Value = std::clamp(std::round((config->Value + 0.1f) * 100.0f) / 100.0f, valueMin, valueMax);
							BloomConfigValueChangedCallback(config);
						}
					}
				}

				if constexpr (std::is_enum_v<T>)
				{
					auto it = config->EnumTemplateReverse.find(config->Value);

					if (decrement)
					{
						if (it == config->EnumTemplateReverse.begin())
							it = config->EnumTemplateReverse.end();

						--it;
					}
					else if (increment)
					{
						++it;

						if (it == config->EnumTemplateReverse.end())
							it = config->EnumTemplateReverse.begin();
					}

					config->Value = it->first;
					config->SnapToNearestAccessibleValue(increment);

					if (increment || decrement)
					{
						UpdateOptionDescription(config, optionDescription);

						Game_PlaySound(ctx, "cursor");

						if (config->Callback)
							config->Callback(config);
					}
				}
				else if ((void*)config == (void*)&Config::MaxHUDWidth)
				{
					StepMaxHudWidth(ctx, increment, decrement);
				}
				else if constexpr (isSliderType)
				{
					auto time = ImGui::GetTime();
					auto fastIncrement = isSlider && (g_leftWasHeld || g_rightWasHeld) && (time - g_lastTappedTime) > 0.5;
					auto playIncrementSound = true;

					static auto s_fastIncrementHoldTime = 0.0;
					static auto s_lastIncrementSoundTime = 0.0;

					static constexpr auto INCREMENT_TIME = 1.0 / 60.0;
					static constexpr auto INCREMENT_SOUND_TIME = 1.0 / 20.0;

					if (fastIncrement)
					{
						s_fastIncrementHoldTime += App::s_deltaTime;
					}
					else
					{
						s_fastIncrementHoldTime = 0.0;
					}

					if (fastIncrement)
					{
						playIncrementSound = (time - s_lastIncrementSoundTime) > INCREMENT_SOUND_TIME;

						if (s_fastIncrementHoldTime < INCREMENT_TIME)
						{
							fastIncrement = false;
						}
						else
						{
							g_lastIncrementTime = time;
						}
					}

					if (fastIncrement)
					{
						increment = g_rightWasHeld;
						decrement = g_leftWasHeld;
					}

					do
					{
						if constexpr (std::is_integral_v<T>)
						{
							if (decrement)
								config->Value -= 1;
							else if (increment)
								config->Value += 1;
						}
						else
						{
							if (decrement)
								config->Value -= 0.01f;
							else if (increment)
								config->Value += 0.01f;
						}

						if (fastIncrement)
							s_fastIncrementHoldTime -= INCREMENT_TIME;
					} while (fastIncrement && s_fastIncrementHoldTime >= INCREMENT_TIME);

					auto isValueInBounds = config->Value >= valueMin && config->Value <= valueMax;

					if ((increment || decrement) && isValueInBounds && playIncrementSound)
					{
						Game_PlaySound(ctx, "cursor");

						s_lastIncrementSoundTime = time;
					}

					config->Value = std::clamp(config->Value, valueMin, valueMax);
				}
				else if constexpr (std::is_same_v<T, bool>)
				{
					if (increment || decrement)
					{
						Game_PlaySound(ctx, "cursor");

						config->Value = !config->Value;
					}
				}

				if (increment || decrement)
				{
					BloomConfigValueChangedCallback(config);
					FpsConfigValueChangedCallback(config);
				}

				// Run standard callback if there's no callback on apply.
				if (!config->ApplyCallback)
				{
					if ((increment || decrement) && config->Callback)
						config->Callback(config);
				}

			}


		}
		else
		{
			if (CheckAndDiscard(g_isAccepted))
				Game_PlaySound(ctx, "cannot_decide");
		}
	}


	std::string valueText;
	bool isValueCentred = false;

	if constexpr (std::is_same_v<T, float>)
	{
		if (config == &Config::ResolutionScale)
		{
			auto resScale = round(*(float*)config->GetValue() * 1000) / 1000;
			valueText = fmt::format("{}%", round(resScale * 100));
		}
		else if (config == &Config::MaxHUDWidth)
		{
			const int preset = FindHudWidthPreset(config->Value);

			if (preset < 0)
			{
				valueText = Localise("Options_Value_Custom");
			}
			else if (!g_hudWidthPresets[preset].Name)
			{
				valueText = Localise("Options_Value_Max");
			}
			else
			{
				valueText = g_hudWidthPresets[preset].Name;
			}
		}
		else
		{
			valueText = fmt::format("{}%", int(round(config->Value * 100.0f)));
		}

	}
	else if constexpr (std::is_same_v<T, int32_t>)
	{
		if (config == &Config::WindowSize)
		{
			if (Config::Fullscreen)
			{
				// Show the real rendered resolution, like the profiler does.
				valueText = fmt::format("{}x{}", Video::s_viewportWidth, Video::s_viewportHeight);
			}
			else
			{
				auto displayModes = GameWindow::GetDisplayModes();

				if (config->Value >= 0 && config->Value < displayModes.size())
				{
					auto& displayMode = displayModes[config->Value];

					valueText = fmt::format("{}x{}", displayMode.w, displayMode.h);
				}
				else
				{
					// Show the real rendered resolution, like the profiler does.
					valueText = fmt::format("{}x{}", Video::s_viewportWidth, Video::s_viewportHeight);
				}
			}

			isValueCentred = true;
		}
		else if (config == &Config::Monitor)
		{
			if (GameWindow::IsAllDisplays(config->Value))
				valueText = Localise("Options_Value_AllScreens");
			else
				valueText = fmt::format("{}", config->Value + 1);

			isValueCentred = true;
		}
		else
		{
			valueText = fmt::format("{}", config->Value);

			if (isSlider && config->Value >= valueMax)
				valueText = Localise("Options_Value_Max");
		}
	}
	else
	{
		valueText = config->GetValueLocalised(Config::Language);
		isValueCentred = true;
	}

	if ((void*)config == (void*)g_previewConfig)
	{
		g_previewValueText = valueText;
	}

	// Draw value

	const float fontValueSize = g_fntSeuratSize - Scale(5.33f);
	auto rawValueSize = g_fntSeurat->CalcTextSizeA(fontValueSize, FLT_MAX, 0.0f, valueText.data());
	bool overflows = rawValueSize.x > valueAreaWidth;

	ImVec2 valuePos = { valueAreaEnd - rawValueSize.x, titlePos.y };

	if (valueAreaWidth > 0.0f)
	{
		if (overflows && isHover)
		{
			const float loopWidth = rawValueSize.x + Scale(30);
			float scrollX = GetTextScrollOffset(rowIndex, valueText, loopWidth, Scale(80), 0.5);

			valuePos.x = valueAreaStart - scrollX;

			auto valueColor = IM_COL32(255, 255, 255, isAccessible ? 255 : 127);

			drawList->PushClipRect({ valueAreaStart, clipRectMin.y }, { valueAreaEnd, clipRectMax.y }, true);

			DrawMenuText(fontValueSize, valuePos, valueColor, valueText.data());
			DrawMenuText(fontValueSize, { valuePos.x + loopWidth, valuePos.y }, valueColor, valueText.data());

			drawList->PopClipRect();
		}
		else
		{
			// selected or doesn't fit fine: truncate with ellipsis if needed
			std::string displayText = overflows
				? TruncateTextWithEllipsis(g_fntSeurat, fontValueSize, valueText, valueAreaWidth)
				: valueText;

			// Recalculate position for truncated text (centered)
			auto displaySize = g_fntSeurat->CalcTextSizeA(fontValueSize, FLT_MAX, 0.0f, displayText.data());
			valuePos.x = valueAreaCenter - (displaySize.x * 0.5f);

			DrawMenuText(fontValueSize, valuePos, IM_COL32(255, 255, 255, isAccessible ? 255 : 127), displayText.data());

		}
	}


	float arrowAreaPadding = Scale(35);
	ImVec2 arrowAreaMin = { valueAreaStart - arrowAreaPadding, min.y + offsetY };
	ImVec2 arrowAreaMax = { valueAreaEnd + arrowAreaPadding, min.y + offsetY + optionHeight };
	DrawSelectionArrows(arrowAreaMin, arrowAreaMax, isSelected);

}


static void DrawActionOption
(
	X86Context* ctx,
	int rowIndex,
	ImVec2 min,
	const std::string& name,
	const std::string& description,
	bool isAccessible,
	const std::string* inaccessibleReason,
	const std::function<void()>& onAccept
)
{
	auto optionHeight = Scale(OPTION_ROW_HEIGHT);
	const float fontOptionSize = g_fntSeuratSize - 4.f;

	// removing offset will make option draw right when the container starts, so we add an extra scale to make it fit inside.
	min.x -= Scale(170) - Scale(10);

	auto offsetY = optionHeight * rowIndex + GetRowScrollOffset(optionHeight);
	auto isCurrent = g_optionIndex == rowIndex;
	auto isHover = isCurrent && OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor;

	ImVec2 titlePos = { min.x, min.y + offsetY + Scale(8) };

	auto TexColor = isHover ? HIGHLIGHT_COLOUR : IM_COL32(255, 255, 255, 255);
	if (isAccessible == false)
	{
		TexColor = IM_COL32(127, 127, 127, 255);
	}
	DrawMenuText(fontOptionSize, titlePos, TexColor, name.c_str(), isHover);

	if (!isCurrent)
		return;

	optionDescription = isAccessible || !inaccessibleReason ? description : *inaccessibleReason;

	if (CheckAndDiscard(g_isAccepted))
	{
		if (isAccessible)
		{
			Game_PlaySound(ctx, "decide");
			onAccept();
		}
		else
		{
			Game_PlaySound(ctx, "cannot_decide");
		}
	}
}

static void GoToLegacyMenu()
{
	OptionsMenu::SetFlowState(OptionsMenuFlowState::CategoryCursor);
	GameMenuCommon::g_LegacyOptions = true;
	OptionsMenu::s_GoToLegacyMenu = true;
}

/* --- SOUND TEST REIMPLEMENTATION --- */

// Reimplement the game's Sound Test in the custom options
// Song only shows up when it has been played at least once in the game.
static constexpr int g_SoundTestTrackCount = 55;
static constexpr uint32_t g_soundTestFilenamesAddr = 0x4EB278;
static constexpr int32_t g_SoundTestFirstMSGID = 6100;
static constexpr uint32_t g_saveDataAddr = 0x549640;
static constexpr uint32_t g_saveDataHeadTracks = 0x1714;
static constexpr uint32_t g_menuTextAddr = 0x54BA14;
static constexpr uint32_t g_fontMessageTable = 0x5C;


enum BgmSlot
{
	bgmSlotStop = 0,
	bgmSlotOptions = 3 // options menu music
};

enum SoundTestRequest
{
	soundTestRequestNone = -3,
	soundTestRequestRestore = -2,
	soundTestRequestPlayTrack = -1
};


static bool g_isSoundTest{};
static int g_soundTestReturnIndex{};
static int g_soundTestPlaying = -1;
static bool g_soundTestChangedMusic{};
static std::vector<int> g_soundTestTracks;
static std::vector<std::string> g_soundTestNames;
static std::atomic<int> g_soundTestRequest{ soundTestRequestNone };

static bool IsSongUnlocked(int track)
{
	auto base = g_memory.base;

	auto saveData = X86_LOAD_U32(g_saveDataAddr);
	if (saveData == 0)
		return false;

	auto bits = X86_LOAD_U32(saveData + g_saveDataHeadTracks + 4 * (track / 32));
	return (bits >> (track % 32)) & 1;
}


static std::string GetTrackName(int track)
{
	auto base = g_memory.base;

	auto menuText = X86_LOAD_U32(g_menuTextAddr);
	auto font = menuText ? X86_LOAD_U32(menuText) : 0;
	auto table = font ? X86_LOAD_U32(font + g_fontMessageTable) : 0;

	if (table)
	{
		auto count = (int32_t)X86_LOAD_U32(table);
		auto textID = (g_SoundTestFirstMSGID + track) * 1000 + 100;

		for (int32_t i = 0; i < count; i++)
		{
			auto record = table + 4 + i * 0x14;
			if ((int32_t)X86_LOAD_U32(record + 4) != textID)
				continue;

			std::string name;
			auto text = X86_LOAD_U32(record);

			for (int j = 0; j < 128; j++)
			{
				uint32_t c = X86_LOAD_U16(text + j * 2);
				if (c == 0)
					break;

				if (c >= 0xD800 && c < 0xDC00)
				{
					uint32_t low = X86_LOAD_U16(text + (j + 1) * 2);
					c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
					j++;
				}

				if (c >= 0x20)
					AppendUtf8(name, c);
			}

			if (!name.empty())
				return name;
		}
	}

	std::string fileName = (const char*)g_memory.Translate(X86_LOAD_U32(g_soundTestFilenamesAddr + 4 * track));
	return fileName.substr(0, fileName.find('.'));
}

static void OpenSoundTest()
{
	g_soundTestTracks.clear();
	g_soundTestNames.clear();

	for (int i = 0; i < g_SoundTestTrackCount; i++)
	{
		if (!IsSongUnlocked(i))
			continue;

		g_soundTestTracks.push_back(i);
		g_soundTestNames.push_back(GetTrackName(i));
	}

	g_isSoundTest = true;
	g_soundTestReturnIndex = g_optionIndex;
	g_optionIndex = 0;
	g_optionCount = (int)g_soundTestTracks.size();
	g_cursorArrowsTime = ImGui::GetTime();
	g_scrollArrows = {};
}

static void CloseSoundTest()
{
	if (!g_isSoundTest)
		return;

	// put the options music back 
	if (g_soundTestChangedMusic)
		g_soundTestRequest = soundTestRequestRestore;

	g_soundTestChangedMusic = false;
	g_soundTestPlaying = -1;
	g_isSoundTest = false;
	g_optionIndex = g_soundTestReturnIndex;
	g_cursorArrowsTime = ImGui::GetTime();
	g_scrollArrows = {};
}

void OptionsMenu::UpdateSoundTest(X86Context& ctx)
{
	auto request = g_soundTestRequest.exchange(soundTestRequestNone);

	if (request == soundTestRequestNone)
		return;

	auto base = g_memory.base;

	// stop the current song, then play the requested track
	auto bgmSlots = GuestToHostFunction<uint32_t>(ctx, sub_5CD00);
	auto slot = request == soundTestRequestRestore ? bgmSlotOptions : bgmSlotStop;
	GuestToHostFunction<void, CallConvention::Thiscall>(ctx, sub_28D8B0, bgmSlots, slot);

	if (request >= 0)
	{
		auto fileName = X86_LOAD_U32(g_soundTestFilenamesAddr + 4 * request);
		auto soundEffects = GuestToHostFunction<uint32_t>(ctx, sub_55B40);
		GuestToHostFunction<void, CallConvention::Thiscall>(ctx, sub_25BE10, soundEffects, fileName);
	}
}

static void DrawSoundTestTrack(int rowIndex, ImVec2 min, ImVec2 max)
{
	auto optionHeight = Scale(OPTION_ROW_HEIGHT);
	const float fontOptionSize = g_fntSeuratSize - 4.f;
	const float fontValueSize = g_fntSeuratSize - Scale(5.33f);

	min.x -= Scale(170) - Scale(10);
	max.x -= Scale(10);

	auto track = g_soundTestTracks[rowIndex];
	auto isPlaying = g_soundTestPlaying == track;
	auto offsetY = optionHeight * rowIndex + GetRowScrollOffset(optionHeight);
	auto isCurrent = g_optionIndex == rowIndex;
	auto isHover = isCurrent && OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor;

	float valueAreaWidth = Scale(105);
	float valueAreaEnd = max.x - Scale(15);
	float valueAreaStart = valueAreaEnd - valueAreaWidth;
	float valueAreaCenter = valueAreaStart + (valueAreaWidth * 0.5f);

	ImVec2 titlePos = { min.x, min.y + offsetY + Scale(8) };
	auto titleAreaWidth = valueAreaStart - Scale(10) - titlePos.x;

	auto TexColor = isHover ? HIGHLIGHT_COLOUR : IM_COL32(255, 255, 255, 255);

	auto title = TruncateTextWithEllipsis(g_fntSeurat, fontOptionSize, g_soundTestNames[rowIndex], titleAreaWidth);
	DrawMenuText(fontOptionSize, titlePos, TexColor, title.c_str(), isHover);

	if (isPlaying)
	{
		auto valueText = TruncateTextWithEllipsis(g_fntSeurat, fontValueSize, Localise("Options_Value_Playing"), valueAreaWidth);
		auto valueSize = g_fntSeurat->CalcTextSizeA(fontValueSize, FLT_MAX, 0.0f, valueText.c_str());
		ImVec2 valuePos = { valueAreaCenter - valueSize.x * 0.5f, titlePos.y };

		DrawMenuText(fontValueSize, valuePos, HIGHLIGHT_COLOUR, valueText.c_str(), true);
	}

	if (!isCurrent)
		return;

	optionDescription = Localise("Options_Desc_SoundTest_Track");

	// Restart the track if player press A while it's already playing
	if (CheckAndDiscard(g_isAccepted))
	{
		g_soundTestPlaying = track;
		g_soundTestRequest = track;
		g_soundTestChangedMusic = true;
	}
}

/* --- SOUND TEST REIMPLEMENTATION END --- */

static void ResetOptionSelection();

static int GetRemapSectionCount()
{
	return Config::ExtendedMultiplayer ? 5 : 3;
}

static bool IsKeyboardRemapSection()
{
	return (ERemapSection)g_remapSectionIndex == ERemapSection::Keyboard;
}

static std::string GetRemapSectionName(int sectionIndex)
{
	if ((ERemapSection)sectionIndex == ERemapSection::Keyboard)
	{
		return Localise("Options_Category_Keyboard");
	}

	return fmt::format(fmt::runtime(Localise("Options_Category_Gamepad")), sectionIndex);
}

static std::string GetRemapSectionDescription(int sectionIndex)
{
	if ((ERemapSection)sectionIndex == ERemapSection::Keyboard)
	{
		return Localise("Options_Desc_Category_Keyboard");
	}

	return fmt::format(fmt::runtime(Localise("Options_Desc_Category_Gamepad")), sectionIndex);
}

static const char* g_xboxButtonNames[] =
{
	"A",
	"B",
	"X",
	"Y",
	"Input_Button_LeftTrigger",
	"Input_Button_RightTrigger",
	"START",
	"BACK",
	"Input_Button_DPadUp",
	"Input_Button_DPadDown",
	"Input_Button_DPadLeft",
	"Input_Button_DPadRight",
	"Input_Button_White",
	"Input_Button_Black",
	"Input_Button_LeftStickClick",
	"Input_Button_RightStickClick"
};

static_assert(std::size(g_xboxButtonNames) == (size_t)input::EXboxButton::Count);

static const std::pair<const char*, ConfigDef<SDL_Scancode>*> g_keyboardBindings[] =
{
	{ "A", &Config::Key_A },
	{ "B", &Config::Key_B },
	{ "X", &Config::Key_X },
	{ "Y", &Config::Key_Y },
	{ "Input_Button_LeftTrigger", &Config::Key_LeftTrigger },
	{ "Input_Button_RightTrigger", &Config::Key_RightTrigger },
	{ "START", &Config::Key_Start },
	{ "BACK", &Config::Key_Back },
	{ "Input_Button_DPadUp", &Config::Key_DPadUp },
	{ "Input_Button_DPadDown", &Config::Key_DPadDown },
	{ "Input_Button_DPadLeft", &Config::Key_DPadLeft },
	{ "Input_Button_DPadRight", &Config::Key_DPadRight },
	{ "Input_Button_LeftStickUp", &Config::Key_LeftStickUp },
	{ "Input_Button_LeftStickDown", &Config::Key_LeftStickDown },
	{ "Input_Button_LeftStickLeft", &Config::Key_LeftStickLeft },
	{ "Input_Button_LeftStickRight", &Config::Key_LeftStickRight },
	{ "Input_Button_RightStickUp", &Config::Key_RightStickUp },
	{ "Input_Button_RightStickDown", &Config::Key_RightStickDown },
	{ "Input_Button_RightStickLeft", &Config::Key_RightStickLeft },
	{ "Input_Button_RightStickRight", &Config::Key_RightStickRight },
	{ "Input_Button_White", &Config::Key_LeftBumper },
	{ "Input_Button_Black", &Config::Key_RightBumper },
	{ "Input_Button_LeftStickClick", &Config::Key_LeftThumb },
	{ "Input_Button_RightStickClick", &Config::Key_RightThumb }
};

// we can use universal name for some buttons, the rest should use localisation.
static std::string GetButtonName(const char* name)
{
	if (std::string_view(name).starts_with("Input_Button_"))
	{
		return Localise(name);
	}

	return name;
}

// Some keys are named differently depending on the language too 
static const std::pair<SDL_Scancode, const char*> g_keyNames[] =
{
	{ SDL_SCANCODE_RETURN, "Input_Key_Return" },
	{ SDL_SCANCODE_ESCAPE, "Input_Key_Escape" },
	{ SDL_SCANCODE_BACKSPACE, "Input_Key_Backspace" },
	{ SDL_SCANCODE_SPACE, "Input_Key_Space" },
	{ SDL_SCANCODE_UP, "Input_Key_Up" },
	{ SDL_SCANCODE_DOWN, "Input_Key_Down" },
	{ SDL_SCANCODE_LEFT, "Input_Key_Left" },
	{ SDL_SCANCODE_RIGHT, "Input_Key_Right" },
	{ SDL_SCANCODE_LCTRL, "Input_Key_LeftCtrl" },
	{ SDL_SCANCODE_RCTRL, "Input_Key_RightCtrl" },
	{ SDL_SCANCODE_LSHIFT, "Input_Key_LeftShift" },
	{ SDL_SCANCODE_RSHIFT, "Input_Key_RightShift" },
	{ SDL_SCANCODE_LALT, "Input_Key_LeftAlt" },
	{ SDL_SCANCODE_RALT, "Input_Key_RightAlt" },
};

static bool IsTextInFont(ImFont* font, const char* text)
{
	while (*text)
	{
		unsigned int c = 0;
		text += ImTextCharFromUtf8(&c, text, nullptr);

		if (c > IM_UNICODE_CODEPOINT_MAX || !font->FindGlyphNoFallback((ImWchar)c))
		{
			return false;
		}
	}

	return true;
}

// Since keys are saved by position, we can add support for Azerty by using SDL's scancode.
static std::string GetKeyLabel(SDL_Scancode key, const std::string& fallback)
{
	for (auto& [scancode, name] : g_keyNames)
	{
		if (scancode == key)
		{
			return Localise(name);
		}
	}

	auto keycode = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, true);

	if (keycode > SDLK_SPACE && keycode != SDLK_DELETE && !(keycode & SDLK_SCANCODE_MASK))
	{
		auto name = SDL_GetKeyName(keycode);

		if (name && *name && IsTextInFont(g_fntSeurat, name))
		{
			return name;
		}
	}

	return fallback;
}

static std::string GetPadButtonLabel(EPadButton button, bool isPlayStation, const std::string& fallback)
{
	if (isPlayStation)
	{
		switch (button)
		{
		case EPadButton::A:             return Localise("Input_Button_PS_Cross");
		case EPadButton::B:             return Localise("Input_Button_PS_Circle");
		case EPadButton::X:             return Localise("Input_Button_PS_Square");
		case EPadButton::Y:             return Localise("Input_Button_PS_Triangle");
		case EPadButton::Back:          return "SHARE";
		case EPadButton::Guide:         return "PS";
		case EPadButton::Start:         return "OPTIONS";
		case EPadButton::LeftStick:     return "L3";
		case EPadButton::RightStick:    return "R3";
		case EPadButton::LeftShoulder:  return "L1";
		case EPadButton::RightShoulder: return "R1";
		case EPadButton::LeftTrigger:   return "L2";
		case EPadButton::RightTrigger:  return "R2";
		default:                        break;
		}
	}

	switch (button)
	{
	case EPadButton::DPadUp:    return Localise("Input_Button_DPadUp");
	case EPadButton::DPadDown:  return Localise("Input_Button_DPadDown");
	case EPadButton::DPadLeft:  return Localise("Input_Button_DPadLeft");
	case EPadButton::DPadRight: return Localise("Input_Button_DPadRight");
	default: break;
	}

	return fallback;
}

// DrawOption layout for non config rows
static void DrawCustomOption(int rowIndex, ImVec2 min, ImVec2 max, const std::string& title, const std::string& value, bool isSelected)
{
	auto optionHeight = Scale(OPTION_ROW_HEIGHT);

	min.x -= Scale(170) - Scale(10);
	max.x -= Scale(10);

	auto offsetY = optionHeight * rowIndex + GetRowScrollOffset(optionHeight);
	auto isHoverOrSelected = g_optionIndex == rowIndex && OptionsMenu::s_flowState != OptionsMenuFlowState::CategoryCursor;

	auto textColor = isHoverOrSelected ? HIGHLIGHT_COLOUR : IM_COL32(255, 255, 255, 255);

	ImVec2 titlePos = { min.x, min.y + offsetY + Scale(8) };

	DrawMenuText(g_fntSeuratSize - 4.f, titlePos, textColor, title.c_str(), isHoverOrSelected);

	if (value.empty())
		return;

	// allow long names
	float valueAreaWidth = Scale(105);
	float valueAreaEnd = max.x - Scale(15);
	float valueAreaStart = valueAreaEnd - valueAreaWidth;
	float valueAreaCenter = valueAreaStart + (valueAreaWidth * 0.5f);

	const float fontValueSize = g_fntSeuratSize - Scale(5.33f);
	auto displayText = TruncateTextWithEllipsis(g_fntSeurat, fontValueSize, value, Scale(150));
	auto displaySize = g_fntSeurat->CalcTextSizeA(fontValueSize, FLT_MAX, 0.0f, displayText.c_str());

	DrawMenuText(fontValueSize, { valueAreaCenter - (displaySize.x * 0.5f), titlePos.y }, IM_COL32(255, 255, 255, 255), displayText.c_str());

	float arrowAreaPadding = Scale(35);
	ImVec2 arrowAreaMin = { valueAreaStart - arrowAreaPadding, min.y + offsetY };
	ImVec2 arrowAreaMax = { valueAreaEnd + arrowAreaPadding, min.y + offsetY + optionHeight };
	DrawSelectionArrows(arrowAreaMin, arrowAreaMax, isSelected);
}

static void DrawCustomizeInputOption(X86Context* ctx, int rowIndex, ImVec2 min, ImVec2 max)
{
	DrawCustomOption(rowIndex, min, max, Localise("Options_Name_CustomizeInput"), "", false);

	if (g_optionIndex != rowIndex)
	{
		return;
	}

	g_optionCurrent = nullptr;
	g_optionCanReset = false;

	if (OptionsMenu::s_flowState == OptionsMenuFlowState::CategoryCursor)
	{
		return;
	}

	optionDescription = Localise("Options_Desc_CustomizeInput");

	if (CheckAndDiscard(g_isAccepted))
	{
		Game_PlaySound(ctx, "decide");

		g_isRemapMenu = true;
		g_remapSectionIndex = 0;
		g_remapReturnOptionIndex = rowIndex;

		ResetOptionSelection();
		OptionsMenu::SetFlowState(OptionsMenuFlowState::CategoryCursor);

		g_categoryTime = ImGui::GetTime();
		g_cursorArrowsTime = g_categoryTime;
	}
}

static void DiscardMenuInput()
{
	g_isAccepted = false;
	g_isDeclined = false;
	g_isLeaving = false;
	g_isReset = false;
	g_up = false;
	g_down = false;
	g_left = false;
	g_right = false;
}

static void BeginBindingCapture(int playerIndex)
{
	int keyCount = 0;
	auto keys = SDL_GetKeyboardState(&keyCount);

	// ignore held input on select
	g_capturePrevKeys.fill(0);
	std::copy_n(keys, std::min(keyCount, (int)g_capturePrevKeys.size()), g_capturePrevKeys.begin());

	if (playerIndex >= 0)
	{
		input::GetRawPadButtons(playerIndex, g_capturePrevPadButtons);
	}

	g_captureState = EBindingCaptureState::Listening;
	g_captureTime = ImGui::GetTime();
}

static SDL_Scancode GetNewlyPressedKey()
{
	int keyCount = 0;
	auto keys = SDL_GetKeyboardState(&keyCount);
	auto result = SDL_SCANCODE_UNKNOWN;

	for (int i = 0; i < std::min(keyCount, (int)g_capturePrevKeys.size()); i++)
	{
		if (keys[i] && !g_capturePrevKeys[i] && result == SDL_SCANCODE_UNKNOWN)
		{
			result = (SDL_Scancode)i;
		}

		g_capturePrevKeys[i] = keys[i];
	}

	return result;
}

// Count if none
static EPadButton GetNewlyPressedPadButton(int playerIndex)
{
	input::PadButtonValues values;
	input::GetRawPadButtons(playerIndex, values);

	auto result = EPadButton::Count;

	for (size_t i = 0; i < values.size(); i++)
	{
		auto isPressed = values[i] >= BINDING_CAPTURE_THRESHOLD;
		auto wasPressed = g_capturePrevPadButtons[i] >= BINDING_CAPTURE_THRESHOLD;

		if (isPressed && !wasPressed && result == EPadButton::Count)
		{
			result = (EPadButton)i;
		}
	}

	g_capturePrevPadButtons = values;

	return result;
}

static bool IsAnyInputHeld(int playerIndex)
{
	int keyCount = 0;
	auto keys = SDL_GetKeyboardState(&keyCount);

	if (std::any_of(keys, keys + keyCount, [](uint8_t key) { return key != 0; }))
	{
		return true;
	}

	if (playerIndex < 0)
	{
		return false;
	}

	input::PadButtonValues values;
	input::GetRawPadButtons(playerIndex, values);

	return std::any_of(values.begin(), values.end(), [](uint8_t value) { return value >= BINDING_CAPTURE_THRESHOLD; });
}

template <typename T>
static constexpr T GetUnboundValue()
{
	if constexpr (std::is_same_v<T, SDL_Scancode>)
	{
		return SDL_SCANCODE_UNKNOWN;
	}
	else
	{
		return EPadButton::None;
	}
}

template <typename T>
static void ClearDuplicateBindings(ConfigDef<T>* config, int playerIndex)
{
	if (config->Value == GetUnboundValue<T>())
	{
		return;
	}

	auto clear = [&](ConfigDef<T>* other)
		{
			if (other != config && other->Value == config->Value)
			{
				other->Value = GetUnboundValue<T>();
			}
		};

	if constexpr (std::is_same_v<T, SDL_Scancode>)
	{
		for (auto& [title, other] : g_keyboardBindings)
		{
			clear(other);
		}
	}
	else
	{
		for (auto* other : input::GetPadBindings(playerIndex))
		{
			clear(other);
		}
	}
}

// playerIndex -1 for keyboard
template <typename T>
static void DrawBindingOption(X86Context* ctx, int rowIndex, ImVec2 min, ImVec2 max, const std::string& title, ConfigDef<T>* config, int playerIndex)
{
	constexpr bool isKeyboard = std::is_same_v<T, SDL_Scancode>;

	auto isCurrent = g_optionIndex == rowIndex && OptionsMenu::s_flowState != OptionsMenuFlowState::CategoryCursor;
	auto isListening = isCurrent && g_captureState == EBindingCaptureState::Listening;
	auto time = ImGui::GetTime();

	std::string valueText;

	if (isListening)
	{
		valueText = fmt::format("{}", (int)ceil(BINDING_CAPTURE_TIMEOUT - (time - g_captureTime)));
	}
	else if (config->Value == GetUnboundValue<T>())
	{
		valueText = "---";
	}
	else if constexpr (isKeyboard)
	{
		valueText = GetKeyLabel(config->Value, config->ToString(false));
	}
	else
	{
		auto isPlayStation = input::GetPadDevice(playerIndex) == input::EInputDevice::PlayStation;
		valueText = GetPadButtonLabel(config->Value, isPlayStation, config->ToString(false));
	}

	DrawCustomOption(rowIndex, min, max, title, valueText, isListening);

	if (g_optionIndex != rowIndex)
	{
		return;
	}

	g_optionCurrent = nullptr;
	g_optionCanReset = isCurrent && g_captureState == EBindingCaptureState::Idle;

	if (!isCurrent)
	{
		return;
	}

	switch (g_captureState)
	{
	case EBindingCaptureState::Idle:
	{
		optionDescription = Localise(isKeyboard ? "Options_Desc_BindKey" : "Options_Desc_BindButton");

		if (CheckAndDiscard(g_isAccepted))
		{
			Game_PlaySound(ctx, "decide");

			BeginBindingCapture(playerIndex);
			OptionsMenu::SetFlowState(OptionsMenuFlowState::OptionSelected);
		}
		else if (CheckAndDiscard(g_isReset))
		{
			Game_PlaySound(ctx, "cancel");

			config->MakeDefault();
			ClearDuplicateBindings(config, playerIndex);
		}

		break;
	}

	case EBindingCaptureState::Listening:
	{
		optionDescription = Localise(isKeyboard ? "Options_Desc_BindKeyListening" : "Options_Desc_BindButtonListening");

		DiscardMenuInput();

		auto key = GetNewlyPressedKey();
		auto isCaptured = false;

		if constexpr (isKeyboard)
		{
			// only saveable keys
			if (key != SDL_SCANCODE_UNKNOWN && key != SDL_SCANCODE_ESCAPE && config->EnumTemplateReverse.contains(key))
			{
				config->Value = key;
				isCaptured = true;
			}
		}
		else
		{
			auto button = GetNewlyPressedPadButton(playerIndex);

			if (button != EPadButton::Count)
			{
				config->Value = button;
				isCaptured = true;
			}
		}

		auto isCancelled = key == SDL_SCANCODE_ESCAPE || (time - g_captureTime) >= BINDING_CAPTURE_TIMEOUT;

		if (isCaptured)
		{
			ClearDuplicateBindings(config, playerIndex);
		}

		if (isCaptured || isCancelled)
		{
			Game_PlaySound(ctx, isCaptured ? "decide" : "cancel");

			g_captureState = EBindingCaptureState::WaitRelease;
			g_captureTime = time;
		}

		break;
	}

	case EBindingCaptureState::WaitRelease:
	{
		DiscardMenuInput();

		if ((time - g_captureTime) >= BINDING_RELEASE_MIN_TIME && !IsAnyInputHeld(playerIndex))
		{
			g_captureState = EBindingCaptureState::Idle;
			OptionsMenu::SetFlowState(OptionsMenuFlowState::OptionCursor);
		}

		break;
	}
	}
}

static void DrawRemapOptions(X86Context* ctx, int& rowCount, ImVec2 min, ImVec2 max)
{
	if (IsKeyboardRemapSection())
	{
		for (auto& [title, config] : g_keyboardBindings)
		{
			DrawBindingOption(ctx, rowCount++, min, max, GetButtonName(title), config, -1);
		}

		return;
	}

	auto playerIndex = g_remapSectionIndex - (int)ERemapSection::Gamepad1P;

	DrawOption(ctx, rowCount++, g_stickDeadzones[playerIndex], true, min, max, nullptr, 0.0f, 0.0f, 0.5f);
	DrawOption(ctx, rowCount++, g_stickSensitivities[playerIndex], true, min, max, nullptr, 0.5f, 1.0f, 2.0f);

	auto& bindings = input::GetPadBindings(playerIndex);

	for (size_t i = 0; i < bindings.size(); i++)
	{
		DrawBindingOption(ctx, rowCount++, min, max, GetButtonName(g_xboxButtonNames[i]), bindings[i], playerIndex);
	}
}

static void DrawOptions(X86Context* ctx, ImVec2 minClipRect, ImVec2 maxClipRect, ImVec2 min, ImVec2 max)
{
	auto drawList = ImGui::GetBackgroundDrawList();
	drawList->PushClipRect(minClipRect, maxClipRect);

	auto cmnReason = &Localise("Options_Desc_NotAvailable");
	auto devReason = &Localise("Options_Desc_NotImplemented");
	auto rowCount = 0;

#define ENUM_VALUE(type) (type)0, (type)0, (type)0

	switch ((OptionsMenuCategory)g_categoryIndex)
	{
	case OptionsMenuCategory::System:
		DrawOption(ctx, rowCount++, &Config::Language, !OptionsMenu::s_isPause, min, max, cmnReason);
		DrawOption(ctx, rowCount++, &Config::VoiceLanguage, !OptionsMenu::s_isPause, min, max, cmnReason);
		DrawOption(ctx, rowCount++, &Config::Subtitles, true, min, max);
		DrawOption(ctx, rowCount++, &Config::EnablePartnerIntro, true, min, max);
		DrawOption(ctx, rowCount++, &Config::ConfirmationPrompt, true, min, max);
		DrawOption(ctx, rowCount++, &Config::Hints, true, min, max);
		DrawOption(ctx, rowCount++, &Config::AchievementNotifications, true, min, max);

		break;

	case OptionsMenuCategory::Input:
		if (g_isRemapMenu)
		{
			DrawRemapOptions(ctx, rowCount, min, max);
			break;
		}

		DrawOption(ctx, rowCount++, &Config::HorizontalCamera1P, true, min, max);
		DrawOption(ctx, rowCount++, &Config::HorizontalCamera2P, true, min, max);
		if (Config::ExtendedMultiplayer.Value) {
			DrawOption(ctx, rowCount++, &Config::HorizontalCamera3P, true, min, max);
			DrawOption(ctx, rowCount++, &Config::HorizontalCamera4P, true, min, max);
		}
		DrawOption(ctx, rowCount++, &Config::Vibration1P, true, min, max);
		DrawOption(ctx, rowCount++, &Config::Vibration2P, true, min, max);
		if (Config::ExtendedMultiplayer.Value) {
			DrawOption(ctx, rowCount++, &Config::Vibration3P, true, min, max);
			DrawOption(ctx, rowCount++, &Config::Vibration4P, true, min, max);
		}
		DrawOption(ctx, rowCount++, &Config::ControllerIcons, !OptionsMenu::s_isPause, min, max, cmnReason);
		DrawOption(ctx, rowCount++, &Config::ChaosPowerLEDSync, true, min, max);
		DrawOption(ctx, rowCount++, &Config::ControllerLEDBrightness, true, min, max);
		DrawOption(ctx, rowCount++, &Config::AllowBackgroundInput, true, min, max);
		DrawCustomizeInputOption(ctx, rowCount++, min, max);
		break;

	case OptionsMenuCategory::Audio:

		if (g_isSoundTest) // sound test uses its own page for obvious reasons
		{
			for (size_t i = 0; i < g_soundTestTracks.size(); i++)
				DrawSoundTestTrack(rowCount++, min, max);

			break;
		}

		DrawOption(ctx, rowCount++, &Config::MasterVolume, true, min, max);
		DrawOption(ctx, rowCount++, &Config::MusicVolume, true, min, max, nullptr, 0.0f, 1.0f, 1.0f);
		DrawOption(ctx, rowCount++, &Config::EffectsVolume, true, min, max, nullptr, 0.0f, 1.0f, 1.0f);
		DrawOption(ctx, rowCount++, &Config::VoicesVolume, true, min, max, nullptr, 0.0f, 1.0f, 1.0f);
		DrawOption(ctx, rowCount++, &Config::CutsceneVolume, true, min, max, nullptr, 0.0f, 1.0f, 1.0f);
		DrawOption(ctx, rowCount++, &Config::MusicAttenuation, os::media::IsSupported(), min, max, &Localise("Options_Desc_OSNotSupported"));
		DrawOption(ctx, rowCount++, &Config::MuteOnFocusLost, true, min, max);
		DrawOption(ctx, rowCount++, &Config::IdleChaosPowersMusic, true, min, max);
		DrawActionOption(ctx, rowCount++, min, Localise("Options_Name_SoundTest"), Localise("Options_Desc_SoundTest"), !OptionsMenu::s_isPause, cmnReason, OpenSoundTest);
		break;

	case OptionsMenuCategory::Video:
	{
		bool canChangeResolution = Config::Fullscreen == false;

		auto displayCount = GameWindow::GetDisplayCount();
		auto canChangeMonitor = Config::Fullscreen && displayCount > 1;
		auto monitorReason = &Localise("Options_Desc_NotAvailableWindowed");

		if (Config::Fullscreen && displayCount <= 1)
			monitorReason = &Localise("Options_Desc_NotAvailableHardware");

		auto monitorMax = GameWindow::CanSpanDisplays() ? displayCount : displayCount - 1;
		DrawOption(ctx, rowCount++, &Config::Monitor, canChangeMonitor, min, max, monitorReason, 0, 0, std::max(monitorMax, 0), false);
#ifdef __ANDROID__
		// The game always covers the screen on Android.
		DrawOption(ctx, rowCount++, &Config::Fullscreen, false, min, max, &Localise("Options_Desc_OSNotSupported"));
#else
		DrawOption(ctx, rowCount++, &Config::Fullscreen, true, min, max, devReason);
#endif
		DrawOption(ctx, rowCount++, &Config::AspectRatio, true, min, max, devReason);
		DrawOption(ctx, rowCount++, &Config::WindowSize, canChangeResolution, min, max, &Localise("Options_Desc_NotAvailableFullscreen"), 0, 0, (int)GameWindow::GetDisplayModes().size() - 1, false);
		DrawOption(ctx, rowCount++, &Config::ResolutionScale, true, min, max, nullptr, 0.25f, 1.0f, 2.0f);
		DrawOption(ctx, rowCount++, &Config::FMVAspectRatio, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::MaxHUDWidth, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::VSync, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::FPS, true, min, max, nullptr, FPS_MIN, 120, GetUnlockedFPS());
		DrawOption(ctx, rowCount++, &Config::CutsceneFramerate, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::ShowFPS, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::Brightness, true, min, max, nullptr);
		DrawOption(ctx, rowCount++, &Config::BloomIntensity, true, min, max, nullptr);

#ifdef SHADOW_RECOMP_D3D12
		DrawOption(ctx, rowCount++, &Config::GraphicsAPI, !OptionsMenu::s_isPause, min, max, cmnReason);
#endif

		break;
	}

	case OptionsMenuCategory::Debug:
	{
		for (auto def : g_configDefinitions)
		{
			if (def->GetSection() != "Codes")
				continue;

			//  def->SetHidden(false);

			DrawOption(ctx, rowCount++, (ConfigDef<bool, true>*)def, true, min, max);
		}

		break;
	}
	}

#undef ENUM_VALUE

	// in case this is open mid draw
	g_optionCount = g_isSoundTest ? (int)g_soundTestTracks.size() : rowCount;

	drawList->PopClipRect();

	if (g_optionCount > MAX_VISIBLE_ROWS)
	{
		auto scrollArrowSize = Scale(15);
		auto scrollArrowCentreX = GetMenuBoxLeft() + GetMenuBoxWidth() - Scale(12);
		auto scrollArrowCentreY = Scale(17);
		ImVec2 scrollArrowsMin = { scrollArrowCentreX - scrollArrowSize / 2, minClipRect.y + scrollArrowCentreY - scrollArrowSize / 2 };
		ImVec2 scrollArrowsMax = { scrollArrowsMin.x + scrollArrowSize, maxClipRect.y - Scale(OPTION_ROW_HEIGHT) + scrollArrowCentreY + scrollArrowSize / 2 };

		DrawScrollArrows(scrollArrowsMin, scrollArrowsMax, scrollArrowSize, g_scrollArrows, g_optionScrollTop > 0, g_optionScrollTop < rowCount - MAX_VISIBLE_ROWS);
	}

}



static void ResetCategorySelection()
{
	g_categoryIndex = -1;
}

static void ResetOptionSelection()
{
	g_optionIndex = 0;
	g_optionCurrent = nullptr;
	g_optionCanReset = false;
	g_scrollArrows = {};
}

static void ResetSelection()
{
	auto time = ImGui::GetTime();

	g_flowStateTime = time;

	ResetCategorySelection();
	ResetOptionSelection();
}


static void MoveCursor(X86Context* ctx, int& cursorIndex, double& cursorTime, int min = 0, int max = INT_MAX, std::function<void(bool isDown)> onCursorMoved = nullptr)
{
	auto time = ImGui::GetTime();

	auto scrollUp = g_up;
	auto scrollDown = g_down;

	if (scrollUp || scrollDown)
		g_lastTappedTime = time;

	static constexpr auto FAST_SCROLL_THRESHOLD = 0.3;
	static constexpr auto FAST_SCROLL_SPEED = 1.0 / 6.5;

	auto fastScroll = (time - g_lastTappedTime) > FAST_SCROLL_THRESHOLD;

	if (fastScroll)
	{
		if ((time - g_lastIncrementTime) < FAST_SCROLL_SPEED)
		{
			fastScroll = false;
		}
		else
		{
			g_lastIncrementTime = time;

			scrollUp = g_upWasHeld;
			scrollDown = g_downWasHeld;
		}
	}

	if (scrollUp)
	{
		--cursorIndex;

		if (cursorIndex < min)
			cursorIndex = max - 1;
	}
	else if (scrollDown)
	{
		++cursorIndex;

		if (cursorIndex >= max)
			cursorIndex = min;
	}

	if (scrollUp || scrollDown)
	{
		Game_PlaySound(ctx, "cursor");

		cursorTime = time;

		if (onCursorMoved)
			onCursorMoved(scrollDown);
	}
}

/* --- Animated sprites --- */

// Originally the legacy options draw some fire animated (sprites) and some whites lines which moves every frame

struct LoopFire
{
	float X; // bottom left corner
	float Y;
	float ScaleX;
	float ScaleY;
	int FirstSprite;
	float SpritePeriod;
	std::span<const LoopKey> Alpha;
};

struct LoopLine
{
	float Y;
	float Length;
	float TexX;
	float TexY;
	std::span<const LoopKey> X;
	std::span<const LoopKey> Alpha;
};

static constexpr float LOOP_SCENE_FRAMES = 620.0f;

static constexpr LoopKey g_fire1Alpha[] = { { 0, 70 }, { 142, 55 }, { 194, 22 }, { 250, 81 }, { 290, 70 } };
static constexpr LoopKey g_fire2Alpha[] = { { 0, 70 } };
static constexpr LoopKey g_fire3Alpha[] = { { 0, 40 }, { 143, 21 }, { 264, 40 } };

static constexpr LoopFire g_loopFires[] =
{
	{ 542.0f, 360.0f, 2.5f, 1.5f, 3, 31.0f, g_fire1Alpha },
	{ 580.0f, 358.0f, 1.0f, 0.8f, 5, 31.0f, g_fire2Alpha },
	{ 342.0f, 344.0f, 1.0f, 0.8f, 0, 30.0f, g_fire3Alpha }
};

static constexpr LoopKey g_line1X[] = { { 0, 0.24269099533557892f }, { 619, 0.40625f }, { 620, 0.23125000298023224f }, { 649, 0.24296875298023224f } };
static constexpr LoopKey g_line1Alpha[] = { { 0, 0 }, { 49, 70 }, { 110, 71 }, { 151, 59 }, { 207, 70 }, { 299, 70 }, { 373, 52 }, { 413, 70 }, { 516, 69 }, { 619, 0 }, { 636, 0 }, { 649, 70 } };
static constexpr LoopKey g_line2X[] = { { 0, 0.7237054109573364f }, { 613, 0.6796875f }, { 614, 0.7306817770004272f }, { 700, 0.7237054109573364f } };
static constexpr LoopKey g_line2Alpha[] = { { 0, 0 }, { 49, 70 }, { 562, 70 }, { 613, 0 }, { 660, 0 }, { 700, 70 } };

static constexpr LoopLine g_loopLines[] =
{
	{ 365.0f, 486.0f, 437.0f, 17.0f, g_line1X, g_line1Alpha },
	{ 157.0f, 382.0f, 464.0f, 121.0f, g_line2X, g_line2Alpha }
};

static double g_loopSceneTime{};

static float GetLoopSceneFrame()
{
	return fmodf((float)((ImGui::GetTime() - g_loopSceneTime) * 60.0), LOOP_SCENE_FRAMES);
}

void DrawAnimatedFire(ImVec2 min, ImVec2 max, float fade)
{
	auto drawList = ImGui::GetBackgroundDrawList();
	auto frame = GetLoopSceneFrame();
	auto scaleX = (max.x - min.x) / 640.0f;
	auto scaleY = (max.y - min.y) / 480.0f;

	static constexpr float rowTops[] = { 0.0f, 31.0f, 63.0f, 95.0f, 128.0f };

	SetAdditive(true);

	for (auto& fire : g_loopFires)
	{
		// sprite progress every 2 frames with no blending
		float spriteFrame = fmodf(frame, fire.SpritePeriod);
		int sprite = (fire.FirstSprite + (int)(spriteFrame / 2.0f)) % 16;
		int col = sprite % 4;
		int row = sprite / 4;

		auto fireUVs = PIXELS_TO_UV_COORDS(512, 512, 384 + col * 32, rowTops[row], 32, rowTops[row + 1] - rowTops[row]);
		auto alpha = (int)(GetInterpolatedFrameValue(fire.Alpha, frame) * fade);


		ImVec2 fireMin = { min.x + fire.X * scaleX, min.y + fire.Y * scaleY - 32.0f * fire.ScaleY * scaleY };
		ImVec2 fireMax = { fireMin.x + 32.0f * fire.ScaleX * scaleY, min.y + fire.Y * scaleY };

		drawList->AddImage(g_upTexOptionsText.get(), fireMin, fireMax, GET_UV_COORDS(fireUVs), IM_COL32(255, 255, 255, alpha));
	}

	ResetAdditive();
}

void DrawLoopLines(ImVec2 min, ImVec2 max, float fade)
{
	auto drawList = ImGui::GetBackgroundDrawList();
	auto frame = GetLoopSceneFrame();
	auto scaleX = (max.x - min.x) / 640.0f;
	auto scaleY = (max.y - min.y) / 480.0f;

	for (auto& line : g_loopLines)
	{
		auto alpha = (int)(GetInterpolatedFrameValue(line.Alpha, frame) * fade);

		if (alpha <= 0)
			continue;

		// stretch the length of the lines depending on the screen size
		float centreX = min.x + GetInterpolatedFrameValue(line.X, frame) * 640.0f * scaleX;
		float centreY = min.y + line.Y * scaleY;
		float halfLength = line.Length * 0.5f * scaleX;
		float halfWidth = 11.0f * scaleY;

		// the strip turns at 90 degrees clockwise
		ImVec2 uvLeftTop = { line.TexX / 512.0f, (line.TexY + line.Length) / 512.0f };
		ImVec2 uvRightTop = { line.TexX / 512.0f, line.TexY / 512.0f };
		ImVec2 uvRightBottom = { (line.TexX + 22.0f) / 512.0f, line.TexY / 512.0f };
		ImVec2 uvLeftBottom = { (line.TexX + 22.0f) / 512.0f, (line.TexY + line.Length) / 512.0f };

		drawList->AddImageQuad
		(
			g_upTexOptionsBG2.get(),
			{ centreX - halfLength, centreY - halfWidth },
			{ centreX + halfLength, centreY - halfWidth },
			{ centreX + halfLength, centreY + halfWidth },
			{ centreX - halfLength, centreY + halfWidth },
			uvLeftTop,
			uvRightTop,
			uvRightBottom,
			uvLeftBottom,
			IM_COL32(255, 255, 255, alpha)
		);
	}
}

/* --- Animated sprites END --- */

static float GetTitleAlphaValue()
{
	switch (OptionsMenu::s_state)
	{
	case OptionsMenuState::Intro:
	case OptionsMenuState::Outro:
		return 0.0f;

	case OptionsMenuState::Closing:
		return ComputeTitleBlink(g_containerClosedAnimTime + g_options_logo_close_delay / 60.0, true) * g_options_logo_alpha;
	default:
		return ComputeTitleBlink(g_logoTime, false) * g_options_logo_alpha;
	}
}

void DrawBackground(bool isPause = false)
{
	UpdateLivePreviewFade();

	auto shown = 1.0f - GetOptionPreviewFade();

	if (shown <= 0.0f)
		return;

	auto& res = ImGui::GetIO().DisplaySize;
	auto drawList = ImGui::GetBackgroundDrawList();

	ImVec2 min = { GetMenuBoxLeft(), g_aspectRatioOffsetY };

	float borderPixels = 32.f; // Options BG has an extra black border at the bottom, we don't want that.
	float uvMaxY = 1.0f - (borderPixels / 512.0f);

	// the background only zooms during the pause intro and outro
	PauseZoom zoom = { { 0.0f, 0.0f }, { res.x, res.y }, 1.0f, true };

	if (isPause && (OptionsMenu::s_state == OptionsMenuState::Intro || OptionsMenu::s_state == OptionsMenuState::Outro))
		zoom = ComputePauseZoom(g_transitionTime, OptionsMenu::s_state == OptionsMenuState::Outro);


	if (shown < 1.0f)
	{
		auto scale = Lerp(g_pause_zoom_start_scale, 1.0f, shown);
		ImVec2 centre = { res.x / 2.0f, res.y / 2.0f };
		zoom.Min = { centre.x - centre.x * scale, centre.y - centre.y * scale };
		zoom.Max = { centre.x + centre.x * scale, centre.y + centre.y * scale };
		zoom.Alpha *= shown;
	}

	drawList->AddImage
	(
		g_upTexOptionsBG.get(),
		zoom.Min,
		zoom.Max,
		{ 0, 0 },
		{ 1, uvMaxY },
		IM_COL32(255, 255, 255, (int)(255 * zoom.Alpha))
	);

	// draw fire and white lines under the menu box
	drawList->PushClipRect(zoom.Min, zoom.Max, true);
	DrawAnimatedFire(zoom.Min, zoom.Max, zoom.Alpha);
	DrawLoopLines(zoom.Min, zoom.Max, zoom.Alpha);
	drawList->PopClipRect();

	// draw Options Logo
	auto logoAlpha = (int)(GetTitleAlphaValue() * shown);

	// recreate the blink effect from legacy options
	if (logoAlpha > 0)
	{
		auto optionsUVs = PIXELS_TO_UV_COORDS(512, 512, 0, 0, 147, 448);
		auto optionsWidth = Scale(OPTIONS_LOGO_WIDTH);
		auto optionsHeight = Scale(OPTIONS_LOGO_HEIGHT);

		ImVec2 optionsMin = { 0.0f, min.y + Scale(10) };
		ImVec2 optionsMax = { optionsMin.x + optionsWidth, optionsMin.y + optionsHeight };

		drawList->AddImage(g_upTexOptionsText.get(), optionsMin, optionsMax, GET_UV_COORDS(optionsUVs), IM_COL32(255, 255, 255, logoAlpha));
	}

	// show version on main menu only
	if (!isPause)
	{
		DrawVersionString(g_fntSeurat, IM_COL32(255, 255, 255, (int)(70 * shown)));
	}
}

void DrawMenuListContainer(X86Context* ctx, ImVec2 min, ImVec2 max)
{
	auto* drawList = ImGui::GetBackgroundDrawList();
	auto& res = ImGui::GetIO().DisplaySize;

	auto containerUVs = PIXELS_TO_UV_COORDS(512, 512, 0, 247, 274, 264);
	auto containerWidth = GetMenuBoxWidth() * OPTIONS_PANEL_WIDTH_RATIO;
	auto containerHeight = res.y;
	auto containerOffsetX = Scale(1);
	auto containerOffsetY = Scale(136);

	// anim

	// 1.0 = fully on screen, 0.0 = fully off screen
	float t = OptionsMenu::s_state == OptionsMenuState::Closing ? (float)ComputeLinearMotion(g_containerClosedAnimTime, 0, g_options_container_frames, true) : (float)ComputeLinearMotion(g_containerAnimTime, 0, g_options_container_frames, false);


	float containerFinalX = GetMenuBoxLeft() + GetMenuBoxWidth() - containerWidth;

	// Start position is fully off screen to the right
	float containerStartX = res.x;

	// Interpolate between start and final
	float containerCurrentX = containerStartX + (containerFinalX - containerStartX) * t;

	ImVec2 containerMin = { containerCurrentX, min.y + containerOffsetY };
	ImVec2 containerMax = { containerCurrentX + containerWidth, containerMin.y + containerHeight };
	ImVec2 panelMax = { containerMax.x + GetMenuBoxLeft(), containerMax.y };

	drawList->AddImage(g_upTexOptionsBG2.get(), containerMin, panelMax, GET_UV_COORDS(containerUVs));

	if (t < 1.f)
		return;

	float optionsOffsetX = Scale(170);
	float optionsOffsetY = Scale(15);

	ImVec2 minOptions = { containerMin.x + optionsOffsetX, containerMin.y + optionsOffsetY };
	ImVec2 maxOptions = { containerMax.x, containerMax.y - optionsOffsetY };

	optionsOffsetX -= Scale(170);
	ImVec2 minClipRect = { containerMin.x + optionsOffsetX, containerMin.y + optionsOffsetY };
	ImVec2 maxClipRect = { containerMax.x, minClipRect.y + Scale(OPTION_ROW_HEIGHT) * MAX_VISIBLE_ROWS };
	DrawOptions(ctx, minClipRect, maxClipRect, minOptions, maxOptions);
	if (OptionsMenu::s_flowState == OptionsMenuFlowState::OptionCursor || OptionsMenu::s_flowState == OptionsMenuFlowState::OptionSelected)
		DrawDescription(optionDescription); // we draw option description separately so they don't get discard by the clip rect.

}


struct CategoryEntry
{
	std::string Name;
	int IconRow;
	bool IsCurrent;
};

static std::vector<CategoryEntry> GetCategoryEntries()
{
	std::vector<CategoryEntry> entries;

	if (g_isRemapMenu)
	{
		for (int i = 0; i < GetRemapSectionCount(); i++)
		{
			auto iconRow = (ERemapSection)i == ERemapSection::Keyboard ? ICON_ROW_KEYBOARD : ICON_ROW_INPUT;
			entries.push_back({ GetRemapSectionName(i), iconRow, i == g_remapSectionIndex });
		}

		return entries;
	}

	for (int i = 0; i < (int)OptionsMenuCategory::Count; i++)
	{
		auto category = (OptionsMenuCategory)i;

		if (IsCategoryVisible(category))
		{
			entries.push_back({ GetCategoryName(category), GetCategoryIconRow(category), i == g_categoryIndex });
		}
	}

	return entries;
}

static void DrawCategories(ImVec2 min, ImVec2 max)
{
	auto drawList = ImGui::GetBackgroundDrawList();

	if (g_categoryIndex == -1)
		g_categoryIndex = 0;

	auto entries = GetCategoryEntries();

	// support for 4:3 stuff so it doesn't go off screen
	const float centreX = min.x + Scale(CATEGORY_COLUMN_CENTRE_X);

	const float safeLeft = (min.x > 0.0f ? min.x : 0.0f) + Scale(CATEGORY_EDGE_MARGIN);
	const float safeRight = GetMenuBoxLeft() + GetMenuBoxWidth() * (1.0f - OPTIONS_PANEL_WIDTH_RATIO) - Scale(CATEGORY_EDGE_MARGIN);

#if OPTIONS_CATEGORY_STYLE == OPTIONS_CATEGORY_STYLE_ICON


	int visibleCount = (int)entries.size();

	if (visibleCount < 1)
		visibleCount = 1;

	const float pitchRatio = LEGACY_ICON_PITCH / LEGACY_ICON_SIZE;
	const float fittedSize = CATEGORY_STACK_HEIGHT / (pitchRatio * (visibleCount - 1) + 1.0f);
	const float iconUnits = fittedSize < LEGACY_ICON_SIZE ? fittedSize : LEGACY_ICON_SIZE;
	const float pitchUnits = iconUnits * pitchRatio;
	const float stackUnits = pitchUnits * (visibleCount - 1) + iconUnits;
	const float firstRowCentreUnits = CATEGORY_STACK_CENTRE_Y - stackUnits * 0.5f + iconUnits * 0.5f;


	const float widestPlate = Scale(iconUnits) * LEGACY_ICON_SELECTED_SCALE;
	const float columnCentreX = ClampBand(centreX - widestPlate * 0.5f, widestPlate, safeLeft, safeRight) + widestPlate * 0.5f;

#else


	float maxTextWidth = 0.0f;

	for (auto& entry : entries)
	{
		auto name = RemoveRubyAnnotations(entry.Name.c_str()).first;
		auto width = g_fntSeurat->CalcTextSizeA(g_fntSeuratSize, FLT_MAX, 0.0f, name.c_str()).x;

		if (width > maxTextWidth)
			maxTextWidth = width;
	}

	const float safeWidth = safeRight - safeLeft;

	float groupWidth = Scale(INLINE_ICON_SIZE) + Scale(INLINE_ICON_GAP) + maxTextWidth;


	// ensure we don't go out of bound part 2
	const float fitScale = (groupWidth > safeWidth && groupWidth > 0.0f) ? safeWidth / groupWidth : 1.0f;

	const float fontSize = g_fntSeuratSize * fitScale;
	const float iconSize = Scale(INLINE_ICON_SIZE) * fitScale;
	const float iconGap = Scale(INLINE_ICON_GAP) * fitScale;

	groupWidth *= fitScale;

	const float groupLeft = ClampBand(centreX - groupWidth * 0.5f, groupWidth, safeLeft, safeRight);
	const float textLeft = groupLeft + iconSize + iconGap;

#endif

	int visibleIndex = 0;

	for (auto& entry : entries)
	{
		const bool isCurrent = entry.IsCurrent;
		auto iconUVs = GetCategoryIconUVs(entry.IconRow, isCurrent);

#if OPTIONS_CATEGORY_STYLE == OPTIONS_CATEGORY_STYLE_ICON

		auto rowCentreY = min.y + Scale(firstRowCentreUnits + pitchUnits * visibleIndex);
		auto drawSize = Scale(iconUnits) * (isCurrent ? LEGACY_ICON_SELECTED_SCALE : 1.0f);

		ImVec2 iconMin = { columnCentreX - drawSize * 0.5f, rowCentreY - drawSize * 0.5f };
		ImVec2 iconMax = { iconMin.x + drawSize, iconMin.y + drawSize };

		drawList->AddImage(g_upTexOptionsIcons.get(), iconMin, iconMax, GET_UV_COORDS(iconUVs));

#else

		auto rowCentreY = min.y + Scale(CATEGORY_TOP + 48.f * visibleIndex) + iconSize * 0.5f; // gap between each category

		ImVec2 iconMin = { groupLeft, rowCentreY - iconSize * 0.5f };
		ImVec2 iconMax = { iconMin.x + iconSize, iconMin.y + iconSize };

		drawList->AddImage(g_upTexOptionsIcons.get(), iconMin, iconMax, GET_UV_COORDS(iconUVs));

		const std::string& text = entry.Name;
		auto textSize = g_fntSeurat->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, RemoveRubyAnnotations(text.c_str()).first.c_str());

		DrawRubyAnnotatedText
		(
			g_fntSeurat,
			fontSize,
			max.x / 2,
			{ textLeft + textSize.x * 0.5f, rowCentreY },
			5.f,
			text.c_str(),

			[=](const char* str, ImVec2 pos)
			{
				DrawMenuText(fontSize, pos, isCurrent ? HIGHLIGHT_COLOUR : IM_COL32(255, 255, 255, 255), str, isCurrent, 1.0f);
			},
			[=](const char* str, float size, ImVec2 pos)
			{
				// DrawTextWithShadow(g_fntSeurat, size, pos, IM_COL32(255, 255, 255, 255), str, 3.f, 0.5f);
			},

			true
			);

#endif

		visibleIndex++;
	}

	if (OptionsMenu::s_flowState == OptionsMenuFlowState::CategoryCursor)
	{
		if (g_isRemapMenu)
		{
			DrawDescription(GetRemapSectionDescription(g_remapSectionIndex));
		}
		else
		{
			DrawDescription(GetCategoryDescription((OptionsMenuCategory)g_categoryIndex));
		}
	}
}

static void OpenButtonGuide()
{
	std::array<Button, 3> buttons =
	{
		Button("Common_Exit", 115.0f, EButtonIcon::Y, EButtonAlignment::Left, &g_isControlsVisible),
		Button("Common_Reset", 100.0f, EButtonIcon::X, EButtonAlignment::Right, &g_optionCanReset),
		Button("Common_Select", 75.0f, EButtonIcon::A, EButtonAlignment::Right, &g_isControlsVisible),
	};

	ButtonGuide::Open(buttons);
	ButtonGuide::SetSideMargins(100);
	ButtonGuide::SetLayout(BUTTON_GUIDE_LEFT, BUTTON_GUIDE_RIGHT, BUTTON_GUIDE_TOP);
}

// hide whole UI and show the option alone (gauge for bloom and brightness)

// slider preview layout (in 640x480 space)
static constexpr float SLIDER_PREVIEW_CENTRE_Y = 344.0f;
static constexpr float SLIDER_PREVIEW_TITLE_GAP = 46.0f;
static constexpr float SLIDER_PREVIEW_ARROW_GAP = 45.0f;
static constexpr float SLIDER_PREVIEW_TEXT_WIDTH = 160.0f;
static constexpr float HP_GAUGE_SCALE = 0.8f;
static constexpr float HP_GAUGE_FILL_HEIGHT = 10.0f;
static constexpr float HP_GAUGE_TEX_SIZE = 128.0f;


static void DrawGaugeStrip(ImDrawList* drawList, float srcX, float srcY, float srcW, float srcH, float x0, float x1, float y, float scale)
{
	// half a texel in so the empty rows around the strip don't bleed in
	auto vMin = (srcY + 0.5f) / HP_GAUGE_TEX_SIZE;
	auto vMax = (srcY + srcH - 0.5f) / HP_GAUGE_TEX_SIZE;
	auto tileWidth = srcW * scale;

	for (auto x = x0; x < x1; x += tileWidth)
	{
		auto width = std::min(tileWidth, x1 - x);
		ImVec2 uvMin = { srcX / HP_GAUGE_TEX_SIZE, vMin };
		ImVec2 uvMax = { (srcX + width / scale) / HP_GAUGE_TEX_SIZE, vMax };

		drawList->AddImage(g_upTexHpGauge.get(), { x, y }, { x + width, y + srcH * scale }, uvMin, uvMax);
	}
}

// returns the fill area so we can put text in
static std::pair<ImVec2, ImVec2> DrawHpGauge(float centreX, float fillCentreY, float ratio)
{
	constexpr float HP_GAUGE_WIDTH = 256.0f;
	constexpr float HP_GAUGE_FILL_TOP = 4.0f;
	constexpr float HP_GAUGE_FILL_LEFT = 10.0f;
	constexpr float HP_GAUGE_FILL_RIGHT = 11.0f;
	constexpr float HP_GAUGE_LEFT_CAP_WIDTH = 48.0f;
	constexpr float HP_GAUGE_RIGHT_CAP_WIDTH = 49.0f;
	constexpr float HP_GAUGE_BOTTOM_TOP = 14.0f;

	auto drawList = ImGui::GetBackgroundDrawList();
	auto scale = Scale(HP_GAUGE_SCALE);

	auto left = centreX - HP_GAUGE_WIDTH * scale * 0.5f;
	auto right = centreX + HP_GAUGE_WIDTH * scale * 0.5f;
	auto top = fillCentreY - (HP_GAUGE_FILL_TOP + HP_GAUGE_FILL_HEIGHT * 0.5f) * scale;

	ImVec2 fillMin = { left + HP_GAUGE_FILL_LEFT * scale, top + HP_GAUGE_FILL_TOP * scale };
	ImVec2 fillMax = { right - HP_GAUGE_FILL_RIGHT * scale, fillMin.y + HP_GAUGE_FILL_HEIGHT * scale };

	// used to cover the dark empty part of the tex (as well as the green color in)
	drawList->AddRectFilled(fillMin, fillMax, IM_COL32(0, 0, 0, 170));

	auto fillRight = Lerp(fillMin.x, fillMax.x, std::clamp(ratio, 0.0f, 1.0f));

	if (fillRight > fillMin.x)
		DrawGaugeStrip(drawList, 0, 98, 128, 10, fillMin.x, fillRight, fillMin.y, scale);

	auto stripLeft = left + HP_GAUGE_LEFT_CAP_WIDTH * scale;
	auto stripRight = right - HP_GAUGE_RIGHT_CAP_WIDTH * scale;

	DrawGaugeStrip(drawList, 0, 1, 128, 4, stripLeft, stripRight, top, scale);
	DrawGaugeStrip(drawList, 0, 15, 128, 6, stripLeft, stripRight, top + HP_GAUGE_BOTTOM_TOP * scale, scale);

	auto leftCapUVs = PIXELS_TO_UV_COORDS(128, 128, 5, 22, 48, 34);
	auto rightCapUVs = PIXELS_TO_UV_COORDS(128, 128, 79, 56, 49, 33);

	drawList->AddImage(g_upTexHpGauge.get(), { left, top }, { stripLeft, top + 34 * scale }, GET_UV_COORDS(leftCapUVs));
	drawList->AddImage(g_upTexHpGauge.get(), { stripRight, top }, { right, top + 33 * scale }, GET_UV_COORDS(rightCapUVs));

	return { fillMin, fillMax };
}


static void DrawOptionPreview()
{
	auto alpha = GetOptionPreviewFade();

	if (!g_previewConfig || alpha <= 0.0f)
		return;

	auto gaugeConfig = GetPreviewGaugeConfig(g_previewConfig);
	auto drawList = ImGui::GetBackgroundDrawList();
	auto vtxStart = drawList->VtxBuffer.Size;
	auto& res = ImGui::GetIO().DisplaySize;

	float centerX = res.x / 2.0f;
	float centerY = g_aspectRatioOffsetY + Scale(SLIDER_PREVIEW_CENTRE_Y);

	float valueFontSize = g_fntSeuratSize * 0.6f;
	float halfWidth = Scale(SLIDER_PREVIEW_TEXT_WIDTH) * 0.5f;

	if (gaugeConfig)
	{
		// slide the gauge to the new value smoothly
		auto blend = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 15.0f);
		g_sliderPreviewValue = Lerp(g_sliderPreviewValue, gaugeConfig->Value, blend);

		auto [fillMin, fillMax] = DrawHpGauge(centerX, centerY, g_sliderPreviewValue);
		halfWidth = (fillMax.x - fillMin.x) * 0.5f;
	}
	else
	{
		valueFontSize = g_fntSeuratSize - Scale(5.33f);
	}

	ImVec2 valueSize = g_fntSeurat->CalcTextSizeA(valueFontSize, FLT_MAX, 0.0f, g_previewValueText.c_str());
	ImVec2 valuePos = { centerX - valueSize.x * 0.5f, centerY - valueSize.y * 0.5f };

	// draw value
	DrawTextWithOutline(g_fntSeurat, valueFontSize, valuePos, IM_COL32_WHITE, g_previewValueText.c_str(), 3.0f, IM_COL32(0, 0, 0, 255));

	halfWidth = std::max(halfWidth, valueSize.x * 0.5f) + Scale(SLIDER_PREVIEW_ARROW_GAP);

	std::string titleText = g_previewConfig->GetNameLocalised(Config::Language);
	ImVec2 titleSize = g_fntSeurat->CalcTextSizeA(g_fntSeuratSize, FLT_MAX, 0.0f, titleText.c_str());
	ImVec2 titlePos = { centerX - titleSize.x * 0.5f, centerY - Scale(SLIDER_PREVIEW_TITLE_GAP) - titleSize.y * 0.5f };

	// draw option name
	DrawMenuText(g_fntSeuratSize, titlePos, HIGHLIGHT_COLOUR, titleText.c_str(), true);

	ImVec2 arrowMin = { centerX - halfWidth, centerY - Scale(17) };
	ImVec2 arrowMax = { centerX + halfWidth, centerY + Scale(17) };
	DrawSelectionArrows(arrowMin, arrowMax, IsLivePreviewActive());

	ScaleVertexAlpha(drawList, vtxStart, alpha);
}

static void BeginOpening()
{
	OptionsMenu::s_state = OptionsMenuState::Opening;
	g_containerAnimTime = ImGui::GetTime();
	g_logoTime = g_containerAnimTime;
	OpenButtonGuide();
}

static void FinishClosing()
{
	if (GameMenuCommon::g_LegacyOptions == false)
	{
		if (OptionsMenu::s_isPause == false)
			GameMenuCommon::ResetNextMode();
	}
	else
	{
		GameMenuCommon::g_OptionsOpen = false;
	}

	OptionsMenu::s_isVisible = false;
}

void OptionsMenu::Draw(X86Context* ctx)
{
	if (!s_isVisible)
		return;


	bool upIsHeld = false;
	bool downIsHeld = false;
	bool leftIsHeld = false;
	bool rightIsHeld = false;

	float t = (float)ComputeLinearMotion(g_containerAnimTime, 0, g_options_container_frames, false);

	switch (s_state)
	{
	case OptionsMenuState::Intro:
	{
		// pause only, wait for the background to zoom in before showing anything else
		GameMenuCommon::g_LegacyOptions = false;

		if (ComputePauseZoom(g_transitionTime, false).IsDone)
			BeginOpening();

		break;
	}

	case OptionsMenuState::Opening:
	{

		GameMenuCommon::g_LegacyOptions = false;

		if (t > 0.8f)
		{
			s_state = OptionsMenuState::Idle;
		}
	}
	break;

	case OptionsMenuState::Idle:
	{
		if (auto pInputState = PJSPeripheral::GetControllerSlot(OptionsMenu::s_padIndex))
		{
			if (PJSPeripheral::IsHeld(OptionsMenu::s_padIndex, PJSPeripheral::BTN_DPAD_UP) || -pInputState->leftY > 0.5f)
				upIsHeld = true;

			if (!g_upWasHeld && upIsHeld)
				g_up = true;

			if (PJSPeripheral::IsHeld(OptionsMenu::s_padIndex, PJSPeripheral::BTN_DPAD_DOWN) || -pInputState->leftY < -0.5f)
				downIsHeld = true;

			if (!g_downWasHeld && downIsHeld)
				g_down = true;

			if (PJSPeripheral::IsHeld(OptionsMenu::s_padIndex, PJSPeripheral::BTN_DPAD_LEFT) || -pInputState->leftX > 0.5f)
				leftIsHeld = true;

			if (!g_leftWasHeld && leftIsHeld)
				g_left = true;

			if (PJSPeripheral::IsHeld(OptionsMenu::s_padIndex, PJSPeripheral::BTN_DPAD_RIGHT) || -pInputState->leftX < -0.5f)
				rightIsHeld = true;

			if (!g_rightWasHeld && rightIsHeld)
				g_right = true;

			if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_A) || PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_START))
				g_isAccepted = true;

			if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_B) || PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_Y))
				g_isDeclined = true;

			if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_Y))
				g_isLeaving = true;

			if (PJSPeripheral::IsJustPressed(OptionsMenu::s_padIndex, PJSPeripheral::BTN_X))
				g_isReset = true;
		}
		break;
	}

	case OptionsMenuState::Closing:
	{
		// wait for the container and the title blink, then zoom out
		if (GetFramesSince(g_containerClosedAnimTime) >= g_options_logo_close_delay + g_logo_blink_close_frames)
		{
			if (s_isPause)
			{
				s_state = OptionsMenuState::Outro;
				g_transitionTime = ImGui::GetTime();
			}
			else
			{
				FinishClosing();
			}
		}
		break;
	}

	case OptionsMenuState::Outro:
	{
		if (ComputePauseZoom(g_transitionTime, true).IsDone)
			FinishClosing();

		break;
	}
	case OptionsMenuState::Restarting:
	{
		static bool s_restartPromptOpened = false;
		static bool s_restartFaderBegun = false;
		static int s_restartMessageResult = -1;

		auto message = Localise("Options_Message_Restart");
		std::array<std::string, 1> options = { Localise("Common_Ok") };

		if (!s_restartPromptOpened)
		{
			s_restartPromptOpened = true;
		}

		if (!s_restartFaderBegun && MessageWindow::Open(message, &s_restartMessageResult, options, 0, 1, false, false, s_padIndex) == MSG_CLOSED)
		{
			if (s_restartMessageResult == 0)
			{
				Config::Save();
				Fader::FadeOut(1, []() { App::Restart({ "--quickboot" }); });
				s_restartFaderBegun = true;
			}
			else
			{
				s_state = OptionsMenuState::Opening;
			}

			s_restartPromptOpened = false;
			s_restartMessageResult = -1;
		}

		break;
	}
	}

	switch (s_flowState)
	{
	case OptionsMenuFlowState::CategoryCursor:
	{
		if (g_isRemapMenu)
		{
			MoveCursor(ctx, g_remapSectionIndex, g_flowStateTime, 0, GetRemapSectionCount(), [](bool)
				{
					ResetOptionSelection();

					g_categoryTime = ImGui::GetTime();
					g_cursorArrowsTime = g_categoryTime;
				});

			if (CheckAndDiscard(g_isAccepted))
			{
				Game_PlaySound(ctx, "decide");

				SetFlowState(OptionsMenuFlowState::OptionCursor);

				g_cursorArrowsTime = ImGui::GetTime();
			}

			if (CheckAndDiscard(g_isDeclined))
			{
				Game_PlaySound(ctx, "cancel");

				g_isRemapMenu = false;
				g_optionIndex = g_remapReturnOptionIndex;

				SetFlowState(OptionsMenuFlowState::OptionCursor);

				g_cursorArrowsTime = ImGui::GetTime();
				g_scrollArrows = {};
			}

			break;
		}

		auto categoryCount = (int)OptionsMenuCategory::Count;

		// Remove debug category from cursor select.
		if (!s_isDebugUnlocked)
			categoryCount -= 1;

		// was used for misc options, restore if we even re use it
		/**if (s_isPause)
			categoryCount -= 1;*/

		MoveCursor(ctx, g_categoryIndex, g_flowStateTime, 0, categoryCount, [](bool)
			{
				ResetOptionSelection();

				g_categoryTime = ImGui::GetTime();
				g_cursorArrowsTime = g_categoryTime;
				// s_commonMenu.SetDescription(GetCategoryDescription((OptionsMenuCategory)g_categoryIndex));
			});

		if (CheckAndDiscard(g_isAccepted))
		{
			Game_PlaySound(ctx, "decide");

			SetFlowState(OptionsMenuFlowState::OptionCursor);

			g_cursorArrowsTime = ImGui::GetTime();
		}

		if (CheckAndDiscard(g_isDeclined) || CheckAndDiscard(g_isLeaving) || OptionsMenu::s_GoToLegacyMenu == true)
		{
			if (OptionsMenu::s_GoToLegacyMenu == false)
				Game_PlaySound(ctx, "cancel");

			g_stateTime = ImGui::GetTime();
			g_categoryTime = g_stateTime;

			s_isRestartRequired = IsRestartRequired();

			if (OptionsMenu::s_isRestartRequired && !OptionsMenu::s_isPause)
			{
				s_state = OptionsMenuState::Restarting;
			}
			else
			{

				Close();
			}
		}

		OptionsMenu::s_GoToLegacyMenu = false;
		break;
	}

	case OptionsMenuFlowState::OptionCursor:
	{
		MoveCursor(ctx, g_optionIndex, g_flowStateTime, 0, g_optionCount, [](bool isDown)
			{
				g_cursorArrowsTime = ImGui::GetTime();

				if (isDown)
				{
					g_scrollArrows.BottomMotionTime = g_cursorArrowsTime;
				}
				else
				{
					g_scrollArrows.TopMotionTime = g_cursorArrowsTime;
				}
			});

		if (g_isSoundTest && CheckAndDiscard(g_isDeclined))
		{
			Game_PlaySound(ctx, "cancel");
			CloseSoundTest();
		}

		if (CheckAndDiscard(g_isDeclined))
		{
			Game_PlaySound(ctx, "cancel");

			SetFlowState(OptionsMenuFlowState::CategoryCursor);

			g_cursorArrowsTime = ImGui::GetTime();

			// s_commonMenu.SetDescription(GetCategoryDescription((OptionsMenuCategory)g_categoryIndex));
		}

		break;
	}
	}

	DrawBackground(OptionsMenu::s_isPause);

	auto& res = ImGui::GetIO().DisplaySize;
	ImVec2 min = { GetMenuBoxLeft(), g_aspectRatioOffsetY };
	ImVec2 max = { min.x + GetMenuBoxWidth(), res.y - min.y };

	bool isTransition = s_state == OptionsMenuState::Intro || s_state == OptionsMenuState::Outro;


	t = (float)ComputeLinearMotion(g_containerAnimTime, 0, g_options_container_frames, false);

	float menuAlpha = GetMenuAlpha();
	auto drawList = ImGui::GetBackgroundDrawList();
	auto vtxStart = drawList->VtxBuffer.Size;

	if (t >= 0.8f && !isTransition && s_state != OptionsMenuState::Closing)
	{
		DrawCategories(min, max);
		g_isControlsVisible = true;
	}

	if (!isTransition)
		DrawMenuListContainer(ctx, min, max);

	ScaleVertexAlpha(drawList, vtxStart, menuAlpha);
	ButtonGuide::s_alpha = menuAlpha;

	DrawOptionPreview();

	g_up = false;
	g_upWasHeld = upIsHeld;
	g_down = false;
	g_downWasHeld = downIsHeld;
	g_left = false;
	g_leftWasHeld = leftIsHeld;
	g_right = false;
	g_rightWasHeld = rightIsHeld;
	g_isAccepted = false;
	g_isDeclined = false;
	g_isReset = false;
	g_isLeaving = false;
}

void OptionsMenu::Open(bool isPause, uint32_t padIndex)
{

	OptionsMenu::s_isPause = isPause;
	OptionsMenu::s_padIndex = padIndex;

	UpdateFontSize();
	g_stateTime = ImGui::GetTime();
	g_transitionTime = g_stateTime;
	g_loopSceneTime = g_stateTime;
	g_optionPreviewFade = 0.0f;
	g_previewConfig = nullptr;
	ButtonGuide::s_alpha = 1.0f;
	g_isControlsVisible = false;

	s_isVisible = true;
	s_GoToLegacyMenu = false;
	g_isRemapMenu = false;
	g_captureState = EBindingCaptureState::Idle;
	CloseSoundTest();
	// Update stored values for config definitions
	// to check if their values have changed later.
	for (auto& def : g_configDefinitions)
		def->UpdateStore();

	ResetSelection();

	if (s_isPause)
	{
		GameMenuCommon::g_nextModeAddr = 0;
		GameMenuCommon::g_currentModeAddr = 0;
	}

	// pause version has an intro to expand the background unlike the main menu version
	if (s_isPause)
		s_state = OptionsMenuState::Intro;
	else
		BeginOpening();
}

bool OptionsMenu::IsRestartRequired()
{
	if (!s_isVisible)
		return false;

	for (auto& def : g_configDefinitions)
	{
		if (def->RequiresRestart() && def->IsValueChanged())
			return true;
	}

	return false;
}

void OptionsMenu::SetFlowState(OptionsMenuFlowState flowState)
{
	s_flowState = flowState;
	g_flowStateTime = ImGui::GetTime();
}


void OptionsMenu::Close()
{
	if (s_state == OptionsMenuState::Closing || s_state == OptionsMenuState::Outro)
		return;

	g_isControlsVisible = false;
	EndPauseHudHide();
	CloseSoundTest();

	if (s_isPause)
	{

		GameMenuCommon::g_nextModeAddr = 0;
		GameMenuCommon::g_currentModeAddr = 0;
		RequestQuitOptionForPause();
	}

	if (GameMenuCommon::g_LegacyOptions == false)
	{
		GameMenuCommon::ResetCurrentMode();
	}
	else
	{
		GameMenuCommon::g_nextModeAddr = 0;
	}


	s_state = OptionsMenuState::Closing;
	g_stateTime = ImGui::GetTime();
	g_containerClosedAnimTime = ImGui::GetTime();
	g_cursorArrowsTime = g_stateTime;
	ButtonGuide::Close();

	Config::Save();
}

bool OptionsMenu::CanClose()
{
	return true; //to do
}

void OptionsMenu::Init()
{
	optionDescription = "";
	g_upTexOptionsBG = LOAD_ZSTD_TEXTURE(g_options_bg);
	g_upTexOptionsBG2 = LOAD_ZSTD_TEXTURE(g_options_bg2);
	g_upTexOptionsIcons = LOAD_ZSTD_TEXTURE(g_optionsIcons);
	g_upTexOptionsText = LOAD_ZSTD_TEXTURE(g_optionsText);
	g_upTexArrow = LOAD_ZSTD_TEXTURE(g_arrow);
	g_upTexHpGauge = LOAD_ZSTD_TEXTURE(g_hpGauge);

	g_fntSeurat = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-DB.otf");
}
