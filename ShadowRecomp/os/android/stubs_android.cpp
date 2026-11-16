// Desktop-only features the Android build leaves out. Each one keeps its public interface
// so the shared code that calls it stays free of platform checks.

#include <install/update_checker.h>
#include <misc/rich_presence/shadow_rpc.h>
#include <ui/installer_wizard.h>

// --- Discord Rich Presence -----------------------------------------------------------
// The Discord IPC client has no Android transport.

void RichPresence::ShadowRpc::Start()
{
}

void RichPresence::ShadowRpc::Stop()
{
}

// --- Update checker --------------------------------------------------------------------
// Updates are handled by the launcher (UpdateManager.java), which can also install them.
// Reporting "up to date" keeps the title screen from offering to open the desktop
// download page.

void UpdateChecker::initialize()
{
}

bool UpdateChecker::start()
{
    return false;
}

UpdateChecker::Result UpdateChecker::check()
{
    return Result::AlreadyUpToDate;
}

void UpdateChecker::visitWebsite()
{
}

// --- Installer wizard ------------------------------------------------------------------
// The wizard picks the game source with a native file dialog, which Android does not
// have. The launcher stages the source instead and os/android/install_android.cpp runs
// the installer before the renderer starts, so the wizard is never shown.

void InstallerWizard::Init()
{
}

void InstallerWizard::Draw(X86Context* ctx)
{
    (void)ctx;
}

void InstallerWizard::Shutdown()
{
}

bool InstallerWizard::Run(std::filesystem::path installPath)
{
    (void)installPath;
    return false;
}
