#include "input.h"
#import <AppKit/AppKit.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace rt
{
    enum : uint16_t
    {
        DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008,
        Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080,
        LeftShoulder = 0x0100, RightShoulder = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
    };

    static std::mutex g_mutex;
    static PadState g_state;
    static uint32_t g_packet;
    // Player 2: the second game controller, when one is connected.
    static PadState g_state2;
    static uint32_t g_packet2;
    static bool g_player2Connected;

    // Mouse: left = A in gameplay (menus take clicks through the game's pointer path), right = B (in a level:
    // fast-forward while held), middle = X; the wheel steps the d-pad up and down.
    static std::atomic<int16_t> g_steeringStick;
    static float g_mouseX, g_mouseY;
    static bool g_mouseUsed;
    static std::atomic<bool> g_leftClickPressesA{ true };
    static bool g_mouseAiming;
    static uint16_t g_mouseButtons;
    static int g_wheelSteps; // positive = up
    static std::atomic<bool> g_wheelAims; // in a level: wheel steps are fine aim and stop mouse aiming
    static std::atomic<bool> g_fastForward; // the right button is held down in a level
    static std::atomic<bool> g_wheelHorizontal; // the wheel taps the d-pad left/right (fine aim, the map)
    static std::atomic<TutorialAction> g_tutorialAction{ TutorialAction::None }; // a tutorial step is waiting
    static int g_clickReads; // game input reads since the left button went down
    static int g_wheelPhase; // game input reads into the current wheel tap

    void SetSteeringStick(int16_t x)
    {
        g_steeringStick = x;
    }

    void MouseMoved(float x, float y)
    {
        std::lock_guard lock(g_mutex);
        g_mouseX = x;
        g_mouseY = y;
        g_mouseAiming = true;
        g_mouseUsed = true;
    }

    void MouseButton(int button, bool down)
    {
        static const uint16_t bits[3] = { A, B, X };
        if (button < 0 || button > 2)
            return;
        std::lock_guard lock(g_mutex);
        // In a level the right button fast-forwards while held, as in the PC games, instead of pressing B.
        if (button == 1 && (down ? g_wheelAims.load() : g_fastForward.load()))
        {
            g_fastForward = down;
            return;
        }
        g_mouseButtons = down ? (g_mouseButtons | bits[button]) : (g_mouseButtons & ~bits[button]);
    }

    bool FastForwardHeld()
    {
        return g_fastForward && g_wheelAims;
    }

    void MouseScrolled(float dy)
    {
        std::lock_guard lock(g_mutex);
        g_wheelSteps += dy > 0 ? 1 : -1;
        // Fine aiming takes over from the cursor until the mouse moves again.
        if (g_wheelAims)
            g_mouseAiming = false;
    }

    void SetWheelAims(bool aims)
    {
        g_wheelAims = aims;
    }

    void SetWheelHorizontal(bool horizontal)
    {
        g_wheelHorizontal = horizontal;
    }

    void SetTutorialAction(TutorialAction action)
    {
        g_tutorialAction = action;
    }

    void SetLeftClickPressesA(bool pressesA)
    {
        g_leftClickPressesA = pressesA;
    }

    bool MousePointer(float& x, float& y, bool& left)
    {
        std::lock_guard lock(g_mutex);
        x = g_mouseX;
        y = g_mouseY;
        left = g_mouseButtons & A;
        return g_mouseUsed;
    }

    bool MouseAimTarget(float& x, float& y)
    {
        std::lock_guard lock(g_mutex);
        x = g_mouseX;
        y = g_mouseY;
        return g_mouseAiming;
    }

    static int16_t Axis(float v)
    {
        return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    static uint8_t Trigger(float v)
    {
        return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }

    static void ReadController(GCExtendedGamepad* pad, PadState& s)
    {
        struct { GCControllerButtonInput* input; uint16_t bit; } buttons[] = {
            { pad.dpad.up, DpadUp }, { pad.dpad.down, DpadDown }, { pad.dpad.left, DpadLeft }, { pad.dpad.right, DpadRight },
            { pad.buttonMenu, Start }, { pad.buttonOptions, Back }, { pad.leftThumbstickButton, LeftThumb },
            { pad.rightThumbstickButton, RightThumb }, { pad.leftShoulder, LeftShoulder }, { pad.rightShoulder, RightShoulder },
            { pad.buttonA, A }, { pad.buttonB, B }, { pad.buttonX, X }, { pad.buttonY, Y },
        };
        for (auto& b : buttons)
        {
            if (b.input.pressed)
                s.buttons |= b.bit;
        }
        s.leftTrigger = std::max(s.leftTrigger, Trigger(pad.leftTrigger.value));
        s.rightTrigger = std::max(s.rightTrigger, Trigger(pad.rightTrigger.value));
        if (pad.leftThumbstick.xAxis.value || pad.leftThumbstick.yAxis.value)
        {
            s.leftX = Axis(pad.leftThumbstick.xAxis.value);
            s.leftY = Axis(pad.leftThumbstick.yAxis.value);
        }
        if (pad.rightThumbstick.xAxis.value || pad.rightThumbstick.yAxis.value)
        {
            s.rightX = Axis(pad.rightThumbstick.xAxis.value);
            s.rightY = Axis(pad.rightThumbstick.yAxis.value);
        }
    }

    // Keyboard (player 1): arrows = left stick, Space = A, Escape = B (Start in a level, where B does nothing
    // and Mac players expect Escape to pause), X / Y, Q / E = shoulders, Z / C = triggers, Return = Start,
    // Tab = Back, WASD = right stick.
    static void ReadKeyboard(GCKeyboardInput* keys, PadState& s)
    {
        auto down = [&](GCKeyCode code) { return [keys buttonForKeyCode:code].pressed; };
        struct { GCKeyCode key; uint16_t bit; } buttons[] = {
            { GCKeyCodeSpacebar, A }, { GCKeyCodeEscape, g_wheelAims ? Start : B }, { GCKeyCodeKeyX, X }, { GCKeyCodeKeyY, Y },
            { GCKeyCodeKeyQ, LeftShoulder }, { GCKeyCodeKeyE, RightShoulder }, { GCKeyCodeReturnOrEnter, Start },
            { GCKeyCodeTab, Back },
        };
        for (auto& b : buttons)
        {
            if (down(b.key))
                s.buttons |= b.bit;
        }
        if (down(GCKeyCodeKeyZ))
            s.leftTrigger = 255;
        if (down(GCKeyCodeKeyC))
            s.rightTrigger = 255;
        int lx = down(GCKeyCodeRightArrow) - down(GCKeyCodeLeftArrow), ly = down(GCKeyCodeUpArrow) - down(GCKeyCodeDownArrow);
        int rx = down(GCKeyCodeKeyD) - down(GCKeyCodeKeyA), ry = down(GCKeyCodeKeyW) - down(GCKeyCodeKeyS);
        if (lx || ly)
        {
            s.leftX = int16_t(lx * 32767);
            s.leftY = int16_t(ly * 32767);
        }
        if (rx || ry)
        {
            s.rightX = int16_t(rx * 32767);
            s.rightY = int16_t(ry * 32767);
        }
    }

    // Player slots: a controller keeps its slot while connected, so when player 1's controller drops
    // mid-game player 2 stays player 2. A new controller takes the first free slot. (Main thread only.)
    static __weak GCController* g_slots[2];

    static void UpdateSlots()
    {
        NSArray<GCController*>* connected = GCController.controllers;
        for (auto& slot : g_slots)
        {
            GCController* controller = slot;
            if (controller && (![connected containsObject:controller] || !controller.extendedGamepad))
                slot = nil;
        }
        for (GCController* controller in connected)
        {
            if (!controller.extendedGamepad || controller == g_slots[0] || controller == g_slots[1])
                continue;
            for (int i = 0; i < 2; i++)
            {
                if (!g_slots[i])
                {
                    g_slots[i] = controller;
                    controller.playerIndex = GCControllerPlayerIndex(i); // the controller's player light
                    break;
                }
            }
        }
    }

    static void Poll()
    {
        // Slot 0's controller, the keyboard and the mouse are player 1; slot 1's controller is player 2.
        UpdateSlots();
        PadState s{}, s2{};
        GCController* player1 = g_slots[0];
        GCController* player2 = g_slots[1];
        if (player1)
            ReadController(player1.extendedGamepad, s);
        if (player2)
            ReadController(player2.extendedGamepad, s2);
        // The keyboard plays only while the game window has focus and no Command shortcut is being typed
        // (Cmd-Q would otherwise press LB, and Return in an alert would also press Start).
        GCKeyboardInput* keys = GCKeyboard.coalescedKeyboard.keyboardInput;
        const bool command = keys && ([keys buttonForKeyCode:GCKeyCodeLeftGUI].pressed || [keys buttonForKeyCode:GCKeyCodeRightGUI].pressed);
        NSWindow* key = NSApp.keyWindow;
        const bool gameFocused = NSApp.isActive && key && !key.attachedSheet && [key.contentView isKindOfClass:NSClassFromString(@"GameView")];
        if (keys && !command && gameFocused)
            ReadKeyboard(keys, s);

        std::lock_guard lock(g_mutex);
        // Steering with a stick hands aiming back from the mouse.
        if (std::abs(s.leftX) > 8000 || std::abs(s.leftY) > 8000)
            g_mouseAiming = false;
        // The left button's A is added in GetPadState, after the pointer code has decided what the click
        // is (merging it here used the previous click's decision, so a menu click could also press A).
        s.buttons |= uint16_t(g_mouseButtons & ~A);
        if (memcmp(&s, &g_state, sizeof(s)) != 0)
        {
            g_state = s;
            g_packet++;
        }
        g_player2Connected = player2 != nil;
        if (memcmp(&s2, &g_state2, sizeof(s2)) != 0)
        {
            g_state2 = s2;
            g_packet2++;
        }
    }

    // The players' game controllers in player order (the order Poll assigns them).
    // Rumble: the two XInput motors (left: low-frequency heavy, right: high-frequency light) drive one
    // continuous haptic event per controller, its intensity from the stronger motor and its sharpness
    // from their balance. Controllers without haptics are left alone.
    struct Rumble
    {
        __weak GCController* controller; // the engine's controller; a different one gets a new engine
        __strong CHHapticEngine* engine;
        __strong id<CHHapticAdvancedPatternPlayer> player;
        bool playing = false;
    };
    static Rumble g_rumble[2];
    static uint32_t g_rumbleRequest[2]; // left << 16 | right, as last requested by the title

    static void ApplyRumble(uint32_t user, uint32_t motors)
    {
        Rumble& r = g_rumble[user];
        GCController* controller = g_slots[user];
        if (r.engine && r.controller != controller)
        {
            [r.engine stopWithCompletionHandler:nil];
            r = Rumble{};
        }
        const float left = float(motors >> 16) / 65535.0f, right = float(motors & 0xFFFF) / 65535.0f;
        const float intensity = std::max(left, right);
        if (intensity <= 0.0f || !controller.haptics)
        {
            if (r.playing)
                [r.player stopAtTime:CHHapticTimeImmediate error:nil];
            r.playing = false;
            return;
        }
        if (!r.engine)
        {
            r.controller = controller;
            r.engine = [controller.haptics createEngineWithLocality:GCHapticsLocalityDefault];
            // The engine stops when the controller sleeps or the system reclaims it; start over next time.
            r.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
                dispatch_async(dispatch_get_main_queue(), ^{ g_rumble[user] = Rumble{}; });
            };
            CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f],
                              [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f] ]
                relativeTime:0 duration:30];
            CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:nil];
            if (!r.engine || ![r.engine startAndReturnError:nil] || !pattern)
            {
                r.engine = nil;
                return;
            }
            r.player = [r.engine createAdvancedPlayerWithPattern:pattern error:nil];
            r.player.loopEnabled = YES;
        }
        const float sharpness = (right * 0.8f + left * 0.1f) / (left + right);
        [r.player sendParameters:@[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0],
                                    [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpness relativeTime:0] ]
                          atTime:CHHapticTimeImmediate error:nil];
        if (!r.playing)
            r.playing = [r.player startAtTime:CHHapticTimeImmediate error:nil];
    }

    void SetRumble(uint32_t user, uint16_t left, uint16_t right)
    {
        if (user > 1)
            return;
        const uint32_t motors = uint32_t(left) << 16 | right;
        if (std::exchange(g_rumbleRequest[user], motors) == motors)
            return;
        dispatch_async(dispatch_get_main_queue(), ^{ ApplyRumble(user, motors); });
    }

    void StopRumble()
    {
        for (uint32_t user = 0; user < 2; user++)
        {
            g_rumbleRequest[user] = 0;
            if (NSThread.isMainThread)
                ApplyRumble(user, 0);
            else
                dispatch_async(dispatch_get_main_queue(), ^{ ApplyRumble(user, 0); });
        }
    }

    void StartInput()
    {
        // Controllers and keyboards report while the app is active; poll at 250 Hz on the main run loop.
        NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / 250.0 repeats:YES block:^(NSTimer*) { Poll(); }];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    }

    bool IsPlayer2Connected()
    {
        static const bool scripted = [] {
            const char* p = getenv("PEGGLE2_INPUT_SCRIPT");
            return p && strstr(p, "2:") != nullptr;
        }();
        std::lock_guard lock(g_mutex);
        return g_player2Connected || scripted;
    }

    bool GetPadState(uint32_t user, PadState& state, uint32_t& packet)
    {
        if (user == 1)
        {
            if (!IsPlayer2Connected())
                return false;
            std::lock_guard lock(g_mutex);
            state = g_state2;
            packet = g_packet2;
            return true;
        }
        if (user != 0)
            return false;
        std::lock_guard lock(g_mutex);
        state = g_state;
        packet = g_packet;
        if (g_leftClickPressesA && (g_mouseButtons & A))
        {
            // During a gamepad tutorial step a click performs the step's action instead, so a mouse player
            // can always go on: aiming steps get A plus RT held (precision) and one RB tap (a small step);
            // the pause and speed-up steps get a tap of Start or X (A would fire).
            switch (g_tutorialAction.load())
            {
            case TutorialAction::Start:
                if (g_clickReads == 0)
                    state.buttons |= Start;
                break;
            case TutorialAction::X:
                if (g_clickReads == 0)
                    state.buttons |= X;
                break;
            case TutorialAction::Aim:
                state.buttons |= A;
                state.rightTrigger = 255;
                if (g_clickReads == 0)
                    state.buttons |= RightShoulder;
                break;
            case TutorialAction::None:
                state.buttons |= A;
                break;
            }
            packet += 0x40000 + 0x100000 * uint32_t(g_tutorialAction.load()) + uint32_t(std::min(g_clickReads, 1));
            g_clickReads++;
        }
        else
            g_clickReads = 0;
        // Each wheel step is one d-pad step, timed in the game's own input reads (one per game tick): the
        // game nudges the aim on every read the d-pad is down, so it is held for exactly one read.
        if (g_wheelSteps || g_wheelPhase)
        {
            if (g_wheelPhase < 1)
            {
                const bool up = g_wheelSteps > 0;
                state.buttons |= g_wheelHorizontal ? (up ? DpadLeft : DpadRight) : (up ? DpadUp : DpadDown);
                packet += 0x80000 + uint32_t(g_wheelPhase);
            }
            if (++g_wheelPhase == 2)
            {
                g_wheelPhase = 0;
                g_wheelSteps += g_wheelSteps > 0 ? -1 : (g_wheelSteps < 0 ? 1 : 0);
            }
        }
        if (int16_t steer = g_steeringStick; steer && std::abs(state.leftX) < 8000 && std::abs(state.leftY) < 8000)
        {
            state.leftX = steer;
            packet += 0x20000 + uint16_t(steer);
        }
        return true;
    }
}
