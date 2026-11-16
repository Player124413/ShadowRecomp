#pragma once

// Background/foreground state of the app.
//
// Android destroys the game's surface when the app goes to the background and creates a
// new one when it returns. SDL pauses audio and parks the thread that pumps its events
// (the main thread) on its own, but this game renders from other threads, which would keep
// presenting into a dead surface. The main thread therefore records the transition here as
// soon as SDL reports it, and the game loop parks itself at the top of its next tick until
// the app is in the foreground again.
namespace os::android
{
    // Called from the SDL event watch on the main thread.
    void SetAppPaused(bool paused);

    bool IsAppPaused();

    // Blocks the calling thread while the app is in the background. Returns true if it had
    // to wait, i.e. the caller is resuming and the swap chain must be revalidated.
    bool WaitWhileAppPaused();
}
