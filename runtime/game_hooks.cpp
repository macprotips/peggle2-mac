// Peggle 2 specific hooks. Addresses refer to the retail XBLA executable (title 58411436).
#include "guest.h"
#include "memory.h"
#include "thread.h"
#include "gpu/gpu.h"
#include "platform/input.h"
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rt
{
    // The launcher pivot in the 1280x720 frame, measured from the gun's clamped aim extremes.
    // Note: fixed for the standard board camera; read the board transform if zoomed views need it.
    constexpr float GunPivotX = 741.0f, GunPivotY = 45.0f;

    // Gravity bends the shot below the line it leaves along, so pointing the cannon straight at the cursor
    // drops the ball under it. Aim instead along the arc through the cursor. A ball leaves the muzzle, about
    // 80 px out along the barrel (11 board units), at angle phi below horizontal and follows
    // y = x tan(phi) + K x^2 (1 + tan^2 phi) / 2 (screen pixels, +y down), with K = gravity / speed^2 measured
    // from tracked shots. Of the two angles that reach the cursor take the direct one; a point the arc cannot
    // reach is aimed at straight. The muzzle moves with the aim, so the solve is repeated from the new muzzle.
    // Note: K and the muzzle length are measured, not the game's own constants; PEGGLE2_AIM_K and
    // PEGGLE2_AIM_MUZZLE tune them.
    static const float g_aimCurvature = getenv("PEGGLE2_AIM_K") ? float(atof(getenv("PEGGLE2_AIM_K"))) : 0.0011f;
    static const float g_aimMuzzle = getenv("PEGGLE2_AIM_MUZZLE") ? float(atof(getenv("PEGGLE2_AIM_MUZZLE"))) : 80.0f;

    // Unit direction to launch along so the arc from the launch point passes through a point dx, dy away.
    static void ArcDirection(float dx, float dy, float& ux, float& uy)
    {
        const float X = std::fabs(dx), Y = dy, K = g_aimCurvature;
        ux = dx;
        uy = dy;
        if (X > 1.0f && K > 0)
        {
            const float a = K * X * X / 2, disc = X * X - 4 * a * (a - Y);
            if (disc >= 0)
            {
                ux = std::copysign(1.0f, dx);
                uy = (-X + std::sqrt(disc)) / (2 * a); // tan(phi) of the flatter, direct arc
            }
        }
        const float length = std::sqrt(ux * ux + uy * uy);
        ux /= length;
        uy /= length;
    }

    // dx, dy: cursor minus the cannon's pivot; returns the unit aim direction.
    static void BallisticAim(float& dx, float& dy)
    {
        float ux, uy;
        ArcDirection(dx, dy, ux, uy);
        for (int i = 0; i < 3 && std::sqrt(dx * dx + dy * dy) > g_aimMuzzle + 10; i++) // not when pointing at the barrel
            ArcDirection(dx - ux * g_aimMuzzle, dy - uy * g_aimMuzzle, ux, uy);
        dx = ux;
        dy = uy;
    }

    // When a gamepad tutorial step last ran, and when the aim update last ran (steady clock, ms).
    static std::atomic<int64_t> g_tutorialTick{ -1000000 };
    static std::atomic<int> g_tutorialAction; // TutorialAction of the waiting step
    static std::atomic<int64_t> g_aimUpdateTick{ -1000000 };
    // When the adventure map (AreaSelectState) last handled a message; it handles several a frame while shown.
    static std::atomic<int64_t> g_areaMapTick{ -1000000 };

    static int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

extern "C" PPC_FUNC(__imp__sub_8229D2F8);
extern "C" PPC_FUNC(__imp__sub_8223BD10);
extern "C" PPC_FUNC(__imp__sub_822651A8);

// AreaSelectState::HandleMessage (vtable 0x820DEB90, slot 9).
PPC_FUNC(sub_822651A8)
{
    rt::g_areaMapTick = rt::NowMs();
    __imp__sub_822651A8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_821A7398);

// GamepadTutorialState::Update(input): a step is waiting while its state (+16) is neither -1 (none)
// nor 3 (finished); +0x30 points to its name from tutorials.json. These steps (Aim, Fire, Nudge, SlowAim,
// PauseMenu, SpeedUp) wait for a gamepad input, so a click performs the one the step asks for.
PPC_FUNC(sub_8229D2F8)
{
    int32_t state = int32_t(*rt::Translate<be<uint32_t>>(ctx.r3.u32 + 16));
    if (state != -1 && state != 3)
    {
        rt::g_tutorialTick = rt::NowMs();
        const uint32_t name = *rt::Translate<be<uint32_t>>(ctx.r3.u32 + 0x30);
        const std::string step = name ? reinterpret_cast<const char*>(rt::Translate<char>(name)) : "";
        rt::g_tutorialAction = int(step == "PauseMenu" ? rt::TutorialAction::Start
                                 : step == "SpeedUp" ? rt::TutorialAction::X
                                                     : rt::TutorialAction::Aim);
    }
    __imp__sub_8229D2F8(ctx, base);
}

// BoardController's aim update for the local player (BoardController* in r3, its Gun* at +204). The
// gun's direction is at Gun+536 (board space, +y down); Gun::AimAt(Gun*, const Vector2*) sets it and
// clamps upward aims. With the mouse in charge the gun points at the cursor; during a gamepad tutorial
// step the mouse steers the stick instead, so the game alone moves the gun and the tutorial sees input.
PPC_FUNC(sub_8223BD10)
{
    const uint32_t controller = ctx.r3.u32;
    __imp__sub_8223BD10(ctx, base);
    // A gap in aim updates means the board was left (menus, results, the next level).
    static uint32_t s_gun;
    static bool s_fired;
    const int64_t now = rt::NowMs();
    if (now - rt::g_aimUpdateTick > 250)
        s_fired = false;
    rt::g_aimUpdateTick = now;

    float x, y;
    const uint32_t gun = *rt::Translate<be<uint32_t>>(controller + 204);
    if (gun && getenv("PEGGLE2_LOG_AIM"))
    {
        auto* d = rt::Translate<be<float>>(gun + 536);
        rt::Log("AIM {} {:.5f}", rt::NowMs(), std::atan2(float(d[0]), float(d[1])) * 57.29578f);
    }
    // Gun+528 is its state: 0 idle (the level intro, or a ball in play), 2 loading, 3 ready, 4 firing.
    // The cursor aims a loaded gun and, with AimWhileBallInPlay and once a shot has been fired on this
    // board, the gun while the ball is in play. Aiming during the intro would split the barrel from its
    // intro pose.
    const uint32_t state = gun ? uint32_t(*rt::Translate<be<uint32_t>>(gun + 528)) : 0;
    if (gun != s_gun)
    {
        s_gun = gun;
        s_fired = false;
    }
    if (state == 4)
        s_fired = true;
    const bool aimable = state == 2 || state == 3 || (state == 0 && s_fired && rt::AimWhileBallInPlay());
    if (!aimable || !rt::MouseAimTarget(x, y))
    {
        rt::SetSteeringStick(0);
        return;
    }
    float dx = x - rt::GunPivotX, dy = y - rt::GunPivotY;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f)
        return;
    rt::BallisticAim(dx, dy);

    if (rt::NowMs() - rt::g_tutorialTick < 250)
    {
        auto* current = rt::Translate<be<float>>(gun + 536);
        // Angle from straight down, positive to the right; push the stick by the remaining error.
        float error = std::atan2(dx, dy) - std::atan2(float(current[0]), float(current[1]));
        float push = std::clamp(error * 4.0f, -1.0f, 1.0f);
        rt::SetSteeringStick(std::fabs(error) < 0.01f ? 0 : int16_t(push * 32767.0f));
        return;
    }
    rt::SetSteeringStick(0);

    // The vector lives in the free stack space below the caller's frame.
    const uint32_t vector = ctx.r1.u32 - 0x40;
    auto* v = rt::Translate<be<float>>(vector);
    v[0] = dx;
    v[1] = dy;
    ctx.r3.u64 = gun;
    ctx.r4.u64 = vector;
    __imp__sub_821A7398(ctx, base);
}

// The game's developer console (help text lists /profile.unlock, /profile.unlockarea <area>, ...).
// HandleMessage(this, msg) passes type-11 messages (+4) that nobody consumed (+12 == 0) to the command
// dispatcher, which reads the command as an MSVC std::string at msg+16. PEGGLE2_CONSOLE="<frame>:<command>;..."
// delivers commands at those frames, for unattended content testing with PEGGLE2_DATA_DIR set.
namespace rt
{
    struct ConsoleCommand
    {
        uint64_t frame;
        std::string text;
    };

    static std::vector<ConsoleCommand> LoadConsoleScript()
    {
        std::vector<ConsoleCommand> commands;
        const char* script = getenv("PEGGLE2_CONSOLE");
        for (const char* p = script; p && *p;)
        {
            const char* end = strchr(p, ';');
            std::string item(p, end ? end : p + strlen(p));
            size_t colon = item.find(':');
            if (colon != std::string::npos)
                commands.push_back({ strtoull(item.c_str(), nullptr, 10), item.substr(colon + 1) });
            p = end ? end + 1 : nullptr;
        }
        return commands;
    }
}

extern "C" PPC_FUNC(__imp__sub_82268720);
extern "C" PPC_FUNC(__imp__sub_82268DC8);
PPC_FUNC(sub_82268720)
{
    static std::vector<rt::ConsoleCommand> pending = rt::LoadConsoleScript();
    const uint32_t self = ctx.r3.u32;
    __imp__sub_82268720(ctx, base);
    while (!pending.empty() && rt::gpu::FrameCount() >= pending.front().frame)
    {
        const std::string text = pending.front().text;
        pending.erase(pending.begin());

        static uint32_t message = rt::g_systemHeap.Alloc(0x40);
        memset(rt::Translate(message), 0, 0x40);
        *rt::Translate<be<uint32_t>>(message + 4) = 11;
        // std::string: 16-byte inline buffer (or heap pointer), then size, then capacity.
        if (text.size() <= 15)
        {
            memcpy(rt::Translate(message + 16), text.c_str(), text.size() + 1);
            *rt::Translate<be<uint32_t>>(message + 36) = 15;
        }
        else
        {
            uint32_t heap = rt::g_systemHeap.Alloc(uint32_t(text.size()) + 1);
            memcpy(rt::Translate(heap), text.c_str(), text.size() + 1);
            *rt::Translate<be<uint32_t>>(message + 16) = heap;
            *rt::Translate<be<uint32_t>>(message + 36) = uint32_t(text.size());
        }
        *rt::Translate<be<uint32_t>>(message + 32) = uint32_t(text.size());
        rt::Log("Console: {}", text);
        ctx.r3.u64 = self;
        ctx.r4.u64 = message;
        __imp__sub_82268DC8(ctx, base);
    }
}

// D3D's GPU wait (sub_82623150) polls a fence through sub_8261D378, which busy-spins and returns non-zero
// while the caller should keep waiting. On the console that spin shares a core with nothing important;
// here it pins a host core, so sleep briefly between polls instead.
extern "C" PPC_FUNC(__imp__sub_8261D378);
PPC_FUNC(sub_8261D378)
{
    __imp__sub_8261D378(ctx, base);
    if (ctx.r3.u32 != 0)
    {
        rt::WaitScope waiting; // the thread is waiting for the GPU, not working
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

// PopCap kept SexyApp's mouse path: MainWidget's Widget overrides (vtable at +4, slots 55/56/58) turn
// MouseMove/MouseDown/MouseUp(x, y) into the UI's type-20 pointer messages, gated by a byte at app+1402.
// The Xbox build never calls them; the mouse feeds them here, on the game thread, once per input poll.
namespace rt
{
    constexpr uint32_t AppPointer = 0x82D8D748;   // global holding the SexyApp instance
    constexpr uint32_t PointerEnabledOffset = 1402;
    constexpr uint32_t MainWidgetMouseMove = 0x8226C4A8, MainWidgetMouseDown = 0x8226C348, MainWidgetMouseUp = 0x8226C3F8;
    static std::atomic<uint32_t> g_mainWidget;    // MainWidget object (its Widget part is at +4)
    static bool g_buttonPressed;                  // set while a press is dispatched when a button takes it
    // Pointer-aware buttons (their SpatialButtonDeviceAdapters) the pointer is over.
    static std::mutex g_hoveredMutex;
    static std::set<uint32_t> g_hovered;

    static void SetHovered(uint32_t adapter, bool hovered)
    {
        std::lock_guard lock(g_hoveredMutex);
        if (hovered)
            g_hovered.insert(adapter);
        else
            g_hovered.erase(adapter);
    }

    // Adapters can be freed without a leave (their scene goes away), so an entry counts only while the
    // object is still a SpatialButtonDeviceAdapter that its scene has enabled (the flag at +8 the adapter
    // checks before handling input); stale entries are dropped.
    constexpr uint32_t SpatialButtonAdapterVtable = 0x820DACE4;

    static bool PointerOverButton()
    {
        std::lock_guard lock(g_hoveredMutex);
        for (auto it = g_hovered.begin(); it != g_hovered.end();)
        {
            const uint32_t adapter = *it;
            if (*Translate<be<uint32_t>>(adapter) != SpatialButtonAdapterVtable)
            {
                it = g_hovered.erase(it);
                continue;
            }
            if (*Translate<uint8_t>(adapter + 8) == 1)
                return true;
            ++it;
        }
        return false;
    }
}

// SpatialButtonDeviceAdapter: pointer entered (kind 7) and left (kind 8) its button.
extern "C" PPC_FUNC(__imp__sub_8223FEB8);
PPC_FUNC(sub_8223FEB8)
{
    rt::SetHovered(ctx.r3.u32, true);
    __imp__sub_8223FEB8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8223FF78);
PPC_FUNC(sub_8223FF78)
{
    rt::SetHovered(ctx.r3.u32, false);
    __imp__sub_8223FF78(ctx, base);
}

// OptionsDialogState (the settings dialog): HandleMessage records the open dialog, its destructor clears it.
// Its sliders only answer the gamepad and Kinect (whose hand messages switch the UI away from the pointer
// for good), so the mouse works them directly. Gizmos at +24/+28/+32 are the volume and screen-size
// SliderGizmos, +36 the colour-blind ToggleButtonGizmo. A gizmo's device is at +4 (a SliderDevice keeps its
// range at +88/+92, and SetValue is what the Kinect path calls) and its spatial adapter at +12, whose region
// (+4) reports the rectangle the console hit-tests (slot 3: x, y, width, height).
namespace rt
{
    constexpr uint32_t OptionsDialogVtable = 0x820DE760;
    constexpr uint32_t ButtonAdapterPress = 0x8223FE88, ButtonAdapterRelease = 0x8223FEA0, SliderSetValue = 0x8223FB80;
    static std::atomic<uint32_t> g_optionsDialog;

    static uint32_t OptionsDialog()
    {
        const uint32_t dialog = g_optionsDialog;
        return dialog && *Translate<be<uint32_t>>(dialog) == OptionsDialogVtable ? dialog : 0;
    }
}

extern "C" PPC_FUNC(__imp__sub_8225EEF8);
PPC_FUNC(sub_8225EEF8)
{
    rt::g_optionsDialog = ctx.r3.u32;
    __imp__sub_8225EEF8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8225E5F8);
PPC_FUNC(sub_8225E5F8)
{
    uint32_t expected = ctx.r3.u32;
    rt::g_optionsDialog.compare_exchange_strong(expected, 0);
    __imp__sub_8225E5F8(ctx, base);
}

namespace rt
{
    struct OptionsControl
    {
        uint32_t gizmo = 0;
        bool slider = false;
        float left = 0, top = 0, width = 0, height = 0;
        float Centre() const { return top + height / 2; }
    };

    // The dialog's rows as the console hit-tests them: three sliders, then the toggle, which has no region
    // and sits one row spacing above the first slider. Empty when the dialog is not open or laid out.
    static bool OptionsRows(OptionsControl (&rows)[4])
    {
        const uint32_t dialog = OptionsDialog();
        if (!dialog)
            return false;
        static const uint32_t out = g_systemHeap.Alloc(16);
        for (int i = 0; i < 4; i++)
        {
            rows[i] = { *Translate<be<uint32_t>>(dialog + 24 + 4 * i), i < 3 };
            const uint32_t adapter = rows[i].gizmo ? uint32_t(*Translate<be<uint32_t>>(rows[i].gizmo + 12)) : 0;
            const uint32_t region = adapter ? uint32_t(*Translate<be<uint32_t>>(adapter + 4)) : 0;
            if (!region)
                continue;
            GuestCall(*Translate<be<uint32_t>>(*Translate<be<uint32_t>>(region) + 12), { region, out, out + 4, out + 8, out + 12 });
            auto* r = Translate<be<int32_t>>(out); // x, y, width, height
            rows[i].left = float(r[0]);
            rows[i].top = float(r[1]);
            rows[i].width = float(r[2]);
            rows[i].height = float(r[3]);
        }
        if (rows[0].width <= 0 || rows[1].width <= 0 || !rows[3].gizmo)
            return false;
        const uint32_t toggle = rows[3].gizmo;
        rows[3] = rows[0];
        rows[3].gizmo = toggle;
        rows[3].slider = false;
        rows[3].top -= rows[1].top - rows[0].top;
        return true;
    }

    // The dialog's hit regions sit below what it draws: the slider rows by 0.13 of a row spacing, the Accept
    // button below them by 0.42 (measured at full screen size; the spacing scales with the other sizes).
    static float OptionsPointerY(const OptionsControl (&rows)[4], float y)
    {
        const float spacing = rows[1].top - rows[0].top;
        const float shifted = y + 0.13f * spacing;
        return shifted > rows[2].Centre() + spacing / 2 ? y + 0.42f * spacing : shifted;
    }

    // The row under (x, y) in region space. The slider regions are taller than the rows are apart (the
    // console takes the top one, so a press on Music would reach Sound), so a row only takes presses within
    // half a row spacing of its centre.
    static OptionsControl OptionsControlAt(const OptionsControl (&rows)[4], float x, float y)
    {
        const float halfSpacing = (rows[1].top - rows[0].top) / 2;
        for (const OptionsControl& row : rows)
            if (row.gizmo && x >= row.left && x < row.left + row.width && std::fabs(y - row.Centre()) <= halfSpacing)
                return row;
        return {};
    }

    // Sets a slider so its knob sits under the pointer. The Kinect handler spreads the range over the whole
    // region, far wider than the drawn track; the knob travels from 6.5% to 67.2% of the region's width.
    // Note: measured at full screen size, re-measure if knobs drift from the cursor at smaller sizes.
    static void DragSlider(const OptionsControl& control, float x)
    {
        const uint32_t device = *Translate<be<uint32_t>>(control.gizmo + 4);
        const int32_t low = *Translate<be<int32_t>>(device + 88), high = *Translate<be<int32_t>>(device + 92);
        const float t = std::clamp((x - (control.left + 0.065f * control.width)) / (0.607f * control.width), 0.0f, 1.0f);
        GuestCall(SliderSetValue, { device, uint64_t(int64_t(low + std::lround(t * float(high - low)))) });
    }
}

// SpatialButtonDeviceAdapter: the pointer pressed its button.
extern "C" PPC_FUNC(__imp__sub_8223FE88);
PPC_FUNC(sub_8223FE88)
{
    rt::g_buttonPressed = true;
    __imp__sub_8223FE88(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8226B8E0);
PPC_FUNC(sub_8226B8E0) // MainWidget constructor
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_8226B8E0(ctx, base);
    rt::g_mainWidget = self;
}

extern "C" PPC_FUNC(__imp__sub_8226BB68);
PPC_FUNC(sub_8226BB68) // MainWidget destructor
{
    uint32_t expected = ctx.r3.u32;
    rt::g_mainWidget.compare_exchange_strong(expected, 0);
    __imp__sub_8226BB68(ctx, base);
}

namespace rt
{
    // Called from XamInputGetState on the game thread.
    void DeliverPointer()
    {
        // While the aim update runs (gameplay), a click shoots (A). In menus a click on a pointer-aware
        // button goes through the game's pointer path; elsewhere (scenes such as the level map only
        // navigate by gamepad) it presses A to confirm the current selection.
        // Until the title screen binds a player (app+4604 stays -1 until its first accepted button press)
        // pointer clicks have no controller, and the title asks to sign in; press A instead.
        const bool gameplay = NowMs() - g_aimUpdateTick < 250;
        SetWheelAims(gameplay);
        // The adventure map scrolls sideways between areas (two buttons each) and ignores up and down.
        SetWheelHorizontal(gameplay || NowMs() - g_areaMapTick < 250);
        SetTutorialAction(gameplay && NowMs() - g_tutorialTick < 250 ? TutorialAction(g_tutorialAction.load()) : TutorialAction::None);
        const uint32_t app = *Translate<be<uint32_t>>(AppPointer);
        const bool owned = app && int32_t(*Translate<be<uint32_t>>(app + 4604)) != -1;
        const uint32_t widget = g_mainWidget;
        float x = 0, y = 0;
        bool left = false;
        const bool used = MousePointer(x, y, left);
        static bool lastLeft, clickIsA = true, pointerPressed;
        static OptionsControl held; // the settings slider or toggle being pressed
        static float lastX = -1, lastY = -1;
        if (!widget || !app || !used)
        {
            SetLeftClickPressesA(true);
            lastLeft = left;
            return;
        }
        *Translate<uint8_t>(app + PointerEnabledOffset) = 1;
        OptionsControl rows[4];
        const bool dialogOpen = OptionsRows(rows);
        const float gy = dialogOpen ? OptionsPointerY(rows, y) : y;
        const uint64_t ix = uint64_t(int64_t(std::lround(x))), iy = uint64_t(int64_t(std::lround(gy)));
        if (x != lastX || y != lastY)
        {
            GuestCall(MainWidgetMouseMove, { widget + 4, ix, iy });
            if (held.slider && OptionsDialog())
                DragSlider(held, x);
        }
        // A press goes to the button or settings control under the pointer. While aiming in a level, before
        // the title has bound a player (app+4604 stays -1 until then; pointer presses carry no controller, so
        // it would ask to sign in), or when the pointer is over nothing that takes it (scenes such as the
        // level map only navigate by gamepad), the click presses A instead. The choice holds until release.
        if (left && !lastLeft)
        {
            clickIsA = gameplay || !owned;
            if (!clickIsA)
            {
                // Hover is only worked out when the pointer moves; a one-pixel nudge here catches buttons
                // that became active (a menu finished animating in) under a pointer that has not moved since.
                GuestCall(MainWidgetMouseMove, { widget + 4, ix + 1, iy });
                GuestCall(MainWidgetMouseMove, { widget + 4, ix, iy });
                g_buttonPressed = false;
                GuestCall(MainWidgetMouseDown, { widget + 4, ix, iy });
                pointerPressed = true;
                if (!g_buttonPressed)
                {
                    held = dialogOpen ? OptionsControlAt(rows, x, gy) : OptionsControl{};
                    if (held.slider)
                        DragSlider(held, x);
                    else if (held.gizmo)
                        GuestCall(ButtonAdapterPress, { *Translate<be<uint32_t>>(held.gizmo + 12) });
                    else
                        clickIsA = !PointerOverButton();
                }
            }
        }
        else if (!left && lastLeft)
        {
            if (pointerPressed)
                GuestCall(MainWidgetMouseUp, { widget + 4, ix, iy });
            if (held.gizmo && !held.slider && OptionsDialog())
                GuestCall(ButtonAdapterRelease, { *Translate<be<uint32_t>>(held.gizmo + 12) });
            pointerPressed = false;
            held = {};
        }
        SetLeftClickPressesA(clickIsA);
        lastX = x;
        lastY = y;
        lastLeft = left;
    }
}

// Frame rate. The title's logic ticks at 60 Hz: on the 360 its frame (sub_8242F8A8) runs two ticks
// (sub_8243CE30, a step of 1.0 each) and then draws, and D3D presents every second vblank (the interval
// in bits 8-11 of the swap request to sub_8261F0B8). At 60 fps a frame runs one tick, draws it, and
// presents every vblank: the same 60 ticks a second, so game speed and physics are unchanged, with every
// tick shown, as the Xbox One release does.
namespace rt
{
    static int g_ticksThisFrame, g_preparesThisFrame;

    // PEGGLE2_PERF: per second, the longest tick and draw on the game thread (ms), independent of load.
    static const bool g_perf = getenv("PEGGLE2_PERF") != nullptr;
    static double g_maxTickMs, g_maxDrawMs;
    template <typename F> static void Timed(double& max, F&& run)
    {
        if (!g_perf)
            return run();
        const auto start = std::chrono::steady_clock::now();
        run();
        max = std::max(max, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        static auto last = start;
        if (start - last >= std::chrono::seconds(1))
        {
            last = start;
            Log("Perf: tick max {:.1f} ms, draw max {:.1f} ms", g_maxTickMs, g_maxDrawMs);
            g_maxTickMs = g_maxDrawMs = 0;
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_8242F8A8);
PPC_FUNC(sub_8242F8A8)
{
    rt::g_ticksThisFrame = rt::g_preparesThisFrame = 0;
    __imp__sub_8242F8A8(ctx, base);
}

// Each tick is preceded by sub_8242F2C8 (input, system notifications and app state); it pairs with its tick.
extern "C" PPC_FUNC(__imp__sub_8242F2C8);
PPC_FUNC(sub_8242F2C8)
{
    if (rt::FrameRate() == 60 && ++rt::g_preparesThisFrame > 1)
    {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_8242F2C8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8243CE30);
PPC_FUNC(sub_8243CE30)
{
    if (rt::FrameRate() == 60 && ++rt::g_ticksThisFrame > 1)
        return; // the second tick of the frame happens in the next frame instead
    // Fast-forward runs extra ticks: the same steps as normal play, just more of them per frame.
    const PPCContext entry = ctx;
    for (int i = rt::FastForwardHeld() ? 3 : 1; i > 0; i--)
    {
        ctx = entry;
        rt::Timed(rt::g_maxTickMs, [&] { __imp__sub_8243CE30(ctx, base); });
    }
}

extern "C" PPC_FUNC(__imp__sub_8243E248);
PPC_FUNC(sub_8243E248)
{
    rt::Timed(rt::g_maxDrawMs, [&] { __imp__sub_8243E248(ctx, base); });
}

extern "C" PPC_FUNC(__imp__sub_8261F0B8);
PPC_FUNC(sub_8261F0B8)
{
    if (rt::FrameRate() == 60)
        ctx.r3.u64 = (ctx.r3.u32 & ~0xF00u) | 0x100u; // present interval 1
    __imp__sub_8261F0B8(ctx, base);
}

// The title's debug print (printf-style, arguments in r4-r10). Retail builds format the message and drop
// it; errors are logged, and with PEGGLE2_GAME_LOG everything.
extern "C" PPC_FUNC(__imp__sub_8236EF40);
PPC_FUNC(sub_8236EF40)
{
    static const bool all = getenv("PEGGLE2_GAME_LOG") != nullptr;
    const char* format = reinterpret_cast<const char*>(rt::Translate<char>(ctx.r3.u32));
    std::string text;
    const uint32_t args[] = { ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    size_t next = 0;
    for (const char* p = format; *p; p++)
    {
        if (*p != '%' || !p[1])
        {
            text += *p;
            continue;
        }
        while (*++p && strchr("-+ #0123456789.lh", *p))
        {
        }
        const uint32_t arg = next < std::size(args) ? args[next] : 0;
        switch (*p)
        {
        case 's': text += arg ? reinterpret_cast<const char*>(rt::Translate<char>(arg)) : "(null)"; next++; break;
        case 'd': case 'i': text += std::to_string(int32_t(arg)); next++; break;
        case 'u': text += std::to_string(arg); next++; break;
        case 'x': case 'X': case 'p': text += fmt::format("{:X}", arg); next++; break;
        case '%': text += '%'; break;
        default: text += '?'; next++; break; // floats travel in FPRs; not reconstructed
        }
        if (!*p)
            break;
    }
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    if (all || text.find("Error") != std::string::npos || text.find("ERROR") != std::string::npos)
        rt::Log("Game: {}", text);
    __imp__sub_8236EF40(ctx, base);
}

// RectSpatial::Contains(x, y) (vtable 0x820D7B70, slot 1), the pointer hit test for buttons; bounds at +20
// left, +24 right, +28 top, +32 bottom. The pointer walks a tree (sub_8225C9D0): it tests the current node's
// children (call at 0x8225CA28, node in r26, children listed from node+4 through +8, a child's region at
// *(child+12)+4) and then the node itself (0x8225CA68). Grid items' regions were sized for Kinect's hand
// cursor: they start at the item's top left but run about 1.5x its size right and down (master select:
// 217 px regions 146 px apart), so siblings overlap and the first or current one kept the pointer, trailing
// the mouse by most of an item. Where sibling regions overlap, the item whose drawing is nearest now wins,
// its centre taken as the centre of the region's top-left two thirds. Regions nothing overlaps are unchanged.
namespace rt
{
    constexpr uint32_t RectSpatialVtable = 0x820D7B70;
    struct Region { uint32_t object; float left, right, top, bottom; };
    static std::vector<Region> g_siblings; // the regions competing in the current test (game thread only)

    static bool ReadRegion(uint32_t region, Region& out)
    {
        if (!region || *Translate<be<uint32_t>>(region) != RectSpatialVtable)
            return false;
        auto* b = Translate<be<float>>(region + 20);
        out = { region, b[0], b[1], b[2], b[3] };
        return true;
    }
    static bool Inside(const Region& r, float x, float y)
    {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }
    static float DistanceToItem(const Region& r, float x, float y)
    {
        const float cx = r.left + (r.right - r.left) / 3, cy = r.top + (r.bottom - r.top) / 3;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy);
    }
}

PPC_FUNC(sub_8225C560)
{
    auto* b = rt::Translate<be<float>>(ctx.r3.u32 + 20);
    const rt::Region self{ ctx.r3.u32, b[0], b[1], b[2], b[3] };
    const float x = float(ctx.f1.f64), y = float(ctx.f2.f64);
    bool hit = rt::Inside(self, x, y);

    // The siblings: the children of r26 when scanning them, the children of the node's parent (node+0) when
    // testing the node itself.
    const uint32_t caller = uint32_t(ctx.lr);
    if (caller == 0x8225CA28 || caller == 0x8225CA68)
    {
        const uint32_t parent = caller == 0x8225CA28 ? ctx.r26.u32 : uint32_t(*rt::Translate<be<uint32_t>>(ctx.r26.u32));
        rt::g_siblings.clear();
        for (uint32_t child = parent ? uint32_t(*rt::Translate<be<uint32_t>>(parent + 4)) : 0; child;
             child = *rt::Translate<be<uint32_t>>(child + 8))
        {
            const uint32_t holder = *rt::Translate<be<uint32_t>>(child + 12);
            rt::Region r;
            if (holder && rt::ReadRegion(*rt::Translate<be<uint32_t>>(holder + 4), r))
                rt::g_siblings.push_back(r);
        }
    }
    if (hit && (caller == 0x8225CA28 || caller == 0x8225CA68) &&
        std::any_of(rt::g_siblings.begin(), rt::g_siblings.end(), [&](const rt::Region& r) { return r.object == self.object; }))
    {
        const float mine = rt::DistanceToItem(self, x, y);
        for (const rt::Region& other : rt::g_siblings)
            if (other.object != self.object && rt::Inside(other, x, y) && rt::DistanceToItem(other, x, y) < mine)
                hit = false;
    }
    ctx.r3.u64 = hit;
}
