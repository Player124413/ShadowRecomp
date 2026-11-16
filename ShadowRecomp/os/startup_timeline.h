#pragma once

// Timeline of the startup sequence.
//
// STARTUP_PHASE("name") marks the end of one step of it. On Android this writes how long
// the step took, and how long ago the game was launched, to log.txt (see LogStartupPhase in
// os/android/logger_android.h): the game starts on a black screen there, and what a player
// reports about it is "it takes long". On the other platforms it does nothing.

#ifdef __ANDROID__
#include <os/android/logger_android.h>

#define STARTUP_PHASE(name) os::android::LogStartupPhase(name)
#else
#define STARTUP_PHASE(name) ((void)0)
#endif
