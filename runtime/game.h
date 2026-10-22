#pragma once
#include <cstdint>
// Game-state queries and gameplay hooks (game_hooks.cpp).
namespace game {
bool cutscene_active();
// The touch overlay's SKIP control was pressed during an in-engine cutscene: end it now (game_hooks.cpp), whatever the
// "Skip any cutscene" option says.
void request_scene_skip();
void player_stepped();    // the player movement step ran (frame_rate.cpp): --stage logs "player active" once
bool pause_menu_active(); // the in-stage pause menu (TPauseManager input, fn_801E3404) ran within the last 150 ms
uint32_t pause_object();  // the pause object while the pause menu runs (fn_801E3404 r3: +0x38 cursor, +0x34 mission)
bool in_stage();          // a stage is being played (player movement step within 300 ms, or its pause menu is up)
bool menus_active();      // the menu folder shows a menu phase (title, Select Mode, boot screens...), not 0 = a stage
bool stage_boot_loading(); // --stage startup: keep menu initialisation behind a loading screen
}
