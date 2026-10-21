// Controller state for the guest, from a game controller or the keyboard.
#pragma once
#include <cstdint>

namespace rt
{
    // XINPUT_GAMEPAD in host order.
    struct PadState
    {
        uint16_t buttons;
        uint8_t leftTrigger, rightTrigger;
        int16_t leftX, leftY, rightX, rightY;
    };

    // Starts polling on the main thread; call once the application is running.
    void StartInput();

    // Where the mouse points, in the guest's 1280x720 frame, while the mouse is the active aiming device
    // (it moved after the last stick input). False otherwise.
    bool MouseAimTarget(float& x, float& y);

    // A left-stick X value (0 = none) the game should see while the stick itself is centred. Lets the
    // mouse steer through stick-driven steps such as the aiming tutorial.
    void SetSteeringStick(int16_t x);

    // The mouse position (1280x720 frame) and left button, once the mouse has been used. False before.
    bool MousePointer(float& x, float& y, bool& left);
    // Whether a left click also presses A. In menus the game's own pointer path handles clicks, so A
    // would activate twice; in gameplay a click shoots.
    void SetLeftClickPressesA(bool pressesA);
    // In a level the scroll wheel taps the d-pad left/right (the game's fine aim steps); elsewhere up/down.
    void SetWheelAims(bool aims);
    // The wheel taps the d-pad left/right (up = left) instead of up/down.
    void SetWheelHorizontal(bool horizontal);
    // An aiming tutorial step (stick, LT/RT, d-pad/LB/RB, fire) is waiting; a click then performs it.
    // A gamepad tutorial step is waiting: what a left click does instead of plain A. Aim: A, holding RT and
    // tapping RB (precision and small steps); Start: the pause menu; X: speeding up moving pegs.
    enum class TutorialAction { None, Aim, Start, X };
    void SetTutorialAction(TutorialAction action);

    // Mouse events from the game view: position in 1280x720 frame coordinates.
    void MouseMoved(float x, float y);
    void MouseButton(int button, bool down); // 0 left, 1 right, 2 middle
    void MouseScrolled(float dy);

    // Player 2 is present when a second game controller is connected (or a test script drives one).
    bool IsPlayer2Connected();

    // Vibration requested by the title (XINPUT motor speeds) for player 0 or 1, played on that player's
    // controller where it supports haptics. StopRumble silences both (the app is quitting or inactive).
    void SetRumble(uint32_t user, uint16_t left, uint16_t right);
    void StopRumble();

    // The latest state of player 1. Returns false when no player is connected at that index.
    bool GetPadState(uint32_t user, PadState& state, uint32_t& packet);
}
