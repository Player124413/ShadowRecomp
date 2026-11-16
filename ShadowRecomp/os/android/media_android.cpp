#include <os/media.h>

// Music attenuation (lowering the game's music while another app plays audio) is not
// implemented on Android.

bool os::media::IsSupported()
{
    return false;
}

bool os::media::IsExternalMediaPlaying()
{
    return false;
}
