# Changelog

## Unreleased

- **Graphics API is now a setting: Auto (Vulkan, then OpenGL ES), Vulkan, or OpenGL ES** (Settings > Display >
  Screen > "Graphics API", `SHADOW_BACKEND=vulkan|gles` for a script). The port asks Dawn for Vulkan on Android and only falls
  back to OpenGL ES if Vulkan refuses to start, so a driver bug in one of them looks like a driver bug in "the game" -
  now one tap on the other API says which. The frame-rate overlay names the API and the adapter in use (that answer used
  to be three log lines deep).
- **Diagnostics for a black or missing world, on the phone itself**: the overlay counts world (3D) and HUD (2D) draws
  per second - a picture that keeps drawing is a different problem from a stage that submits nothing - plus two switches
  ("Draw both sides", "Ignore the depth buffer") that build new pipelines immediately, so culling and depth can be ruled
  in or out with a tap instead of a rebuild. Those two come with "Skip full-screen copies" and "Hide haze copies", which
  drop the game's 2D copy of the screen and its haze pass - the two things that draw *over* a finished world, so a world
  that is hidden rather than missing shows up the moment one of them is on. "Show log on the screen" draws the tail of the
  log over the game with any error lines pinned in red at the top, which is how a renderer error becomes a screenshot
  instead of a report.

**Android (new)**
- A 64-bit Android port: one APK for arm64 phones, a launcher that imports your own disc image and picks a graphics
  tier, and touch controls you edit on top of the running picture (drag, resize, opacity, rebind, presets, your own
  layouts). Bluetooth gamepads work alongside the touch controls.
- ETC2 textures on Android: textures the console stored block-compressed (CMPR) are re-encoded to ETC2 instead of being
  uploaded as 32-bit RGBA8, four to eight times less texture memory and texture bandwidth (EAC alpha only when a
  texture actually uses its alpha channel).
- The APK can be built on GitHub Actions from a link you paste when you start the run - an ISO/GCM image, an archive
  holding `generated/` or a disc folder, or `main.dol` alone. No secret, no local NDK.
- Immersive full screen in game: the status bar (the clock) and the three navigation buttons - back, home, recent -
  are hidden while you play, and a swipe from an edge brings them back for a moment. They used to sit on top of the
  picture, and the navigation bar took the taps meant for the controls at the bottom of the screen.
- Touch sticks that behave like the ports: the ring stays where the layout drew it (no jump to your thumb), the axis is
  measured from the point the thumb landed so a touch never starts at a deflection, the knob follows the thumb with a
  short trail and springs back to the centre, the deadzone is drawn, and a button press is animated. Floating sticks
  (the ring under the thumb) are still a choice in the editor, and now fade in instead of teleporting.
- **Fixed: the world was black on Android.** Textures the console stored block-compressed were re-encoded to ETC2 (4 bits
  per pixel) but uploaded into a texture still created as 32-bit RGBA8, so every one of them went up as garbage - in the
  first stage that is most of the level, while the HUD (paletted / RGB5A3 art, which is not transcoded) looked normal.
  The texture is now created with the format its bytes are in, and an upload whose size does not fit its format is
  refused instead of reading past the buffer (release builds do not run those checks). This also removes the extra
  upload traffic, which is part of the frame-rate problem.
- Adaptive render resolution (`runtime/dynamic_resolution.cpp`): the height follows the frame times that are actually
  delivered - down when frames are late (a stage with heavy overdraw, a phone getting warm), back up one small step at a
  time when there is margin. The `quality` preset now starts at 900 lines instead of the panel's own height (the
  launcher's resolution slider still offers Native), because a 2.4:1 1200-line panel is 2.5x the pixels of 720p for a
  picture that is hard to tell apart on glass.
- Pre-rendered movies: `SKIP` is a control on the touch overlay while one plays (it does not depend on the "cutscenes
  you have already seen" option), the option itself now starts on for a fresh Android install, and the render height is
  capped at 720 lines while a movie streams so the emulated Sofdec decoder gets the frame time instead of the GPU.
- `[drs]` log line once every 5 s with the measured frame rate and render scale, `[gx] movie started/ended` and the
  render height: what a "it runs at 40 fps" report needs to be actionable.
- **Cutscenes can be skipped from the touch screen**, whichever kind they are: `SKIP` now appears during an in-engine
  story scene as well as during a pre-rendered movie, and pressing it ends the scene without needing the "Skip any
  cutscene" option (a control labelled SKIP is an explicit request; the option only decides whether a bare START press
  also skips a scene you have not seen). A settings file written by an earlier build is migrated, so the option starts
  on for phones instead of keeping the old default - which is why START did nothing.
- **The texture encoder is about seven times faster**: 2 us per 4x4 block instead of 16, measured by
  `tools/android/etc2_check.cpp` (which now prints it). It solved a squared distance in floating point per pixel per
  modifier table per base-colour mode; the error of modifier m is now `|d|^2 - (2*m*|S1| - 3*m*m)`, so the best of the
  four modifiers is a handful of integer operations over a sub-block whose base colour is its mean refined twice.
  Decoded quality is if anything better (37.7 dB on the gradient instead of 37.4). This is the difference between a
  texture upload being invisible and a visible hitch: 16 us per block is a 260 ms freeze while a 512x512 texture loads.
- A texture the game re-uploads every frame - a movie frame, a streaming texture - is no longer transcoded
  (`third_party/aurora/lib/gfx/upload_churn.hpp`): encoding it on the render thread once per frame costs more than the
  RGBA8 upload it saves. A size that produces more than five new textures inside a second, spread over at least 200 ms,
  is left uncompressed while the stream runs; a level load's burst of same-sized textures keeps its compression.
  `tools/android/churn_test.cpp` checks the timing rules against a fake clock.
- `Display > Picture > Compressed textures` (Auto / Off / On), and the first log line now says which way it went and
  whether the GPU reported ETC2 at all: if a driver mis-samples ETC2 the world can be black while the HUD still draws,
  and that is now one tap away from being ruled out.
- The frame-rate overlay also shows the frame time, the height being rendered, the adaptive resolution's scale, how
  many textures are compressed and by how much, and whether a cutscene is on screen.
- The Android build is built and packaged by GitHub Actions on every push (`.github/workflows/android-apk.yml`): the
  disc-free tests, a full arm64 compile of the native port, and an installable launcher-only APK. Set the
  `SHADOW_GEN_URL` secret to your own `generated/` tarball and the same job builds the real APK.

**Fixes**
- Build one APK without a disc: `SHADOW_STUB_GAME_CODE=1 android/build.sh` links empty stand-ins for the game code, so
  the launcher and the packaging can be tried before `tools/gen.py` has ever run.

## v1.1 (2026-10-21)

**New**
- Controls page in the settings menu: rebind every game button, both sticks and the menu key to any key, mouse button or controller button. There are separate Controller and Keyboard and mouse setups, and Reset all goes back to the defaults.
- HDR settings now have their own section under Display.
- New "Original game logic" option, on by default so the game behaves as it always did. Turning it off unlocks the extras:
  - Far-away enemies update less often, for a smoother frame rate in busy stages.
  - "Objects per stage" can go past the original game's limit.
- Turning "Original game logic" back on puts View distance and Object distance back to 100% straight away.
- A short hint on first start shows where the settings menu is.
- New SR icon.

**Fixes**
- Crackling and dropouts in the sound are fixed.
- "Skip any cutscene" no longer skips a cutscene by itself (for example the New Game intro) without a button press.
- The bug report key (F12 / Back + Start) is now off by default. Turn it on under System if you want it.
- Menu sliders follow the mouse while you drag them.
- Closing the menu no longer asks you to restart. Settings that need a restart are marked in the menu.
- Busy scenes with lots of objects no longer cut out, and a crash in the graphics code is fixed.
- More engine speed-ups (a few percent more frames per second in some stages).

## v1.0 (2026-10-21)

First release. You need your own copy of Shadow the Hedgehog for GameCube (USA version).

**Setup**
- On first start the game asks for your disc image (`.iso`, `.gcm`, or `.rvz` with Dolphin installed) or a disc folder extracted with Dolphin. It checks it's the right version, unpacks it, and starts. After that it goes straight into the game.

**Graphics**
- Any resolution, higher frame rates, and widescreen / ultrawide screen shapes (the HUD isn't stretched)
- Real-time shadows, with colour and brightness controls
- HDR
- Wider field of view and longer view distance
- Your own texture packs

**Gameplay**
- Skip any cutscene
- Movement tweaks, and your own saved movement presets
- Play as one of the game's other character models
- Optional harder bosses
- Cheats

**Settings**
- Everything is in an in-game menu (Back on a controller, Esc on the keyboard). The game keeps running while it's open.
- Quick setup at the top for resolution, screen shape, HDR and shadows
- Option to mute the game when its window is in the background

**Other**
- Controller and keyboard support
- Faster engine (about 20-40% more frames per second at 4K than earlier test builds)

**Known issues**
- 2-player mode hasn't been tested yet.
- Ambient occlusion is a work in progress and is switched off.
- European and Japanese discs aren't supported yet.
- The stage editor isn't included yet.
