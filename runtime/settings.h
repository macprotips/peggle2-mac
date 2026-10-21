// Player settings the runtime reads while the game runs. The app shell loads and saves them.
#pragma once

namespace rt
{
    // Frames drawn per second: 60 (the default, as on the Xbox One) or 30 (as on the Xbox 360). Game speed is
    // the same either way.
    int FrameRate();
    void SetFrameRate(int fps);

    // Internal render resolution as a multiple of the console's 1280x720: 1 (original), 2 (2560x1440) or 3
    // (3840x2160, 4K). Takes effect at the next frame.
    int RenderScale();
    void SetRenderScale(int scale);

    // Whether the mouse keeps aiming the cannon while a ball is in play (as in the PC games). Off by default:
    // the Xbox 360 game locks the cannon until the next ball is loaded.
    bool AimWhileBallInPlay();
    void SetAimWhileBallInPlay(bool on);

    // Whether the startup logo videos end as soon as they start (the masters' videos still play). Off by
    // default.
    bool SkipIntroVideos();
    void SetSkipIntroVideos(bool on);
}
