// Player settings (see settings.h).
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace rt
{
    // PEGGLE2_FPS overrides the saved choice (for tests).
    static std::atomic<int> g_frameRate{ getenv("PEGGLE2_FPS") && atoi(getenv("PEGGLE2_FPS")) == 30 ? 30 : 60 };

    int FrameRate()
    {
        return g_frameRate.load(std::memory_order_relaxed);
    }

    void SetFrameRate(int fps)
    {
        if (!getenv("PEGGLE2_FPS"))
            g_frameRate = fps == 60 ? 60 : 30;
    }

    // PEGGLE2_SCALE overrides the saved choice (for tests).
    static std::atomic<int> g_renderScale{ getenv("PEGGLE2_SCALE") ? std::clamp(atoi(getenv("PEGGLE2_SCALE")), 1, 3) : 1 };

    int RenderScale()
    {
        return g_renderScale.load(std::memory_order_relaxed);
    }

    void SetRenderScale(int scale)
    {
        if (!getenv("PEGGLE2_SCALE"))
            g_renderScale = std::clamp(scale, 1, 3);
    }

    static std::atomic<bool> g_aimWhileBallInPlay{ getenv("PEGGLE2_AIM_IN_PLAY") != nullptr };

    bool AimWhileBallInPlay()
    {
        return g_aimWhileBallInPlay.load(std::memory_order_relaxed);
    }

    void SetAimWhileBallInPlay(bool on)
    {
        g_aimWhileBallInPlay = on;
    }

    static std::atomic<bool> g_skipIntroVideos{ getenv("PEGGLE2_SKIP_INTRO") != nullptr };

    bool SkipIntroVideos()
    {
        return g_skipIntroVideos.load(std::memory_order_relaxed);
    }

    void SetSkipIntroVideos(bool on)
    {
        g_skipIntroVideos = on;
    }
}
