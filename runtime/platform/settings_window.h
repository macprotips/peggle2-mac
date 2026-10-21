// The Settings window (Peggle 2 > Settings…) and the saved settings it edits.
#pragma once

namespace rt
{
    // Applies the saved settings (call once at launch, before the game starts).
    void LoadSettings();
    // Opens the Settings window, or brings it to the front. Main thread only.
    void ShowSettings();
}
