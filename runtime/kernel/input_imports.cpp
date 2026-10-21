// XamInput*: player 1 is always connected (game controller or keyboard).
#include "../guest.h"
#include "../gpu/gpu.h"
#include "../platform/input.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "../memory.h"
#include <vector>

namespace rt
{
    constexpr uint32_t ErrorDeviceNotConnected = 0x48F;

    struct GuestGamepad
    {
        be<uint16_t> buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        be<int16_t> leftX, leftY, rightX, rightY;
    };
    static_assert(sizeof(GuestGamepad) == 12);

    struct GuestInputState
    {
        be<uint32_t> packet;
        GuestGamepad gamepad;
    };

    struct GuestCapabilities
    {
        uint8_t type;
        uint8_t subType;
        be<uint16_t> flags;
        GuestGamepad gamepad;
        be<uint16_t> leftMotor, rightMotor;
    };
    static_assert(sizeof(GuestCapabilities) == 20);

    // PEGGLE2_INPUT_SCRIPT="1000@300-310,L-1@400-460,M640:600@500,K@510-515,2:1000@700": over frame ranges,
    // hold buttons (hex), push the left stick fully left (L-1) or right (L1), move the mouse to a 1280x720
    // point, hold the left (K) or right (R) mouse button, or scroll one wheel step (W1 up, W-1 down, at the range's
    // first frame); a "2:" prefix addresses player 2, who then counts as
    // connected. For unattended testing.
    static bool ApplyScript(uint32_t user, PadState& s)
    {
        struct Press { uint32_t user; uint16_t buttons; int stick, wheel; float mouseX, mouseY; bool mouse, click, rightClick; uint64_t from, to; mutable bool scrolled; };
        static const std::vector<Press> script = [] {
            std::vector<Press> presses;
            const char* p = getenv("PEGGLE2_INPUT_SCRIPT");
            while (p && *p)
            {
                char* end;
                Press press{};
                if (p[0] == '2' && p[1] == ':')
                {
                    press.user = 1;
                    p += 2;
                }
                if (*p == 'K' || *p == 'R')
                {
                    (*p == 'K' ? press.click : press.rightClick) = true;
                    end = const_cast<char*>(p + 1);
                }
                else if (*p == 'L')
                    press.stick = int(strtol(p + 1, &end, 10));
                else if (*p == 'W')
                    press.wheel = int(strtol(p + 1, &end, 10));
                else if (*p == 'M')
                {
                    press.mouse = true;
                    press.mouseX = strtof(p + 1, &end);
                    press.mouseY = *end == ':' ? strtof(end + 1, &end) : 0.0f;
                }
                else
                    press.buttons = uint16_t(strtoul(p, &end, 16));
                press.from = *end == '@' ? strtoull(end + 1, &end, 10) : 0;
                press.to = *end == '-' ? strtoull(end + 1, &end, 10) : press.from;
                presses.push_back(press);
                p = *end == ',' ? end + 1 : nullptr;
            }
            return presses;
        }();
        bool applied = false;
        bool click = false, rightClick = false;
        uint64_t frame = gpu::FrameCount();
        for (const Press& press : script)
        {
            if (press.user != user || frame < press.from || frame > press.to)
                continue;
            s.buttons |= press.buttons;
            if (press.stick)
                s.leftX = int16_t(press.stick * 32767);
            if (press.mouse)
                MouseMoved(press.mouseX, press.mouseY);
            if (press.wheel && !press.scrolled)
            {
                press.scrolled = true; // once, though the game may read input more than once a frame
                MouseScrolled(float(press.wheel));
            }
            click |= press.click;
            rightClick |= press.rightClick;
            applied = true;
        }
        // K and R hold the left and right mouse buttons over their ranges.
        static bool clicking, rightClicking;
        if (user == 0 && click != clicking)
        {
            clicking = click;
            MouseButton(0, click);
        }
        if (user == 0 && rightClick != rightClicking)
        {
            rightClicking = rightClick;
            MouseButton(1, rightClick);
        }
        return applied;
    }

    // PEGGLE2_MEMSNAP="1800,1900,2000": at each frame, write every live heap allocation to
    // out/snap/<frame>.bin as (address, size, bytes) records, for finding game state by diffing.
    static void MaybeSnapshot()
    {
        static std::vector<uint64_t> frames = [] {
            std::vector<uint64_t> list;
            for (const char* p = getenv("PEGGLE2_MEMSNAP"); p && *p;)
            {
                char* end;
                list.push_back(strtoull(p, &end, 10));
                p = *end == ',' ? end + 1 : nullptr;
            }
            return list;
        }();
        uint64_t frame = gpu::FrameCount();
        if (frames.empty() || frame < frames.front())
            return;
        frames.erase(frames.begin());
        std::filesystem::create_directories("out/snap");
        FILE* f = fopen(fmt::format("out/snap/{}.bin", frame).c_str(), "wb");
        for (GuestHeap* heap : { &g_virtual4kHeap, &g_virtualHeap })
        {
            for (auto [address, size] : heap->Allocations())
            {
                uint32_t header[2] = { address, size };
                fwrite(header, 4, 2, f);
                fwrite(Translate(address), 1, size, f);
            }
        }
        for (auto [physical, size] : g_physicalHeap.Allocations())
        {
            uint32_t header[2] = { PhysicalToVirtual(physical), size };
            fwrite(header, 4, 2, f);
            fwrite(Translate(PhysicalToVirtual(physical)), 1, size, f);
        }
        // The executable's .data section, for globals.
        uint32_t data[2] = { 0x828B6000, 0x4E30F8 };
        fwrite(data, 4, 2, f);
        fwrite(Translate(data[0]), 1, data[1], f);
        fclose(f);
        Log("Memory snapshot at frame {}", frame);
    }

    void DeliverPointer();

    static uint32_t XamInputGetState(uint32_t user, uint32_t, GuestInputState* out)
    {
        if (user == 0)
            DeliverPointer();
        PadState s{};
        uint32_t packet = 0;
        if (!GetPadState(user, s, packet))
            return ErrorDeviceNotConnected;
        MaybeSnapshot();
        if (ApplyScript(user, s))
            packet += 0x10000 + uint32_t(gpu::FrameCount() & 0xFFFF);
        out->packet = packet;
        out->gamepad = { s.buttons, s.leftTrigger, s.rightTrigger, s.leftX, s.leftY, s.rightX, s.rightY };
        return 0;
    }

    static uint32_t XamInputGetCapabilities(uint32_t user, uint32_t, GuestCapabilities* out)
    {
        PadState s;
        uint32_t packet;
        if (!GetPadState(user, s, packet))
            return ErrorDeviceNotConnected;
        *out = {};
        out->type = 1;    // XINPUT_DEVTYPE_GAMEPAD
        out->subType = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        // Every control is present at full resolution.
        out->gamepad = { 0xF3FF, 0xFF, 0xFF, int16_t(-64), int16_t(-64), int16_t(-64), int16_t(-64) };
        out->leftMotor = 0xFFFF;
        out->rightMotor = 0xFFFF;
        return 0;
    }

    struct GuestVibration
    {
        be<uint16_t> leftMotor;
        be<uint16_t> rightMotor;
    };

    static uint32_t XamInputSetState(uint32_t user, uint32_t, GuestVibration* vibration)
    {
        PadState s;
        uint32_t packet;
        if (!GetPadState(user, s, packet))
            return ErrorDeviceNotConnected;
        if (vibration)
            SetRumble(user, vibration->leftMotor, vibration->rightMotor);
        return 0;
    }
}

GUEST_IMPORT(XamInputGetState, rt::XamInputGetState)
GUEST_IMPORT(XamInputGetCapabilities, rt::XamInputGetCapabilities)
GUEST_IMPORT(XamInputSetState, rt::XamInputSetState)
