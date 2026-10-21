// The title's achievements, from its own XDBF resource, and the ones this profile has unlocked.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rt::achievements
{
    struct Achievement
    {
        uint16_t id = 0;
        uint16_t gamerscore = 0;
        uint32_t flags = 0;   // XACH flags: type in the low bits, 8 = shown while locked
        uint32_t imageId = 0;
        std::string name;
        std::string description; // how it was earned
        std::string unachieved;  // how to earn it
        std::vector<uint8_t> image; // PNG

        bool Secret() const { return !(flags & 8); }
    };

    // Reads the achievement table from the title's XDBF data, with strings in `language` (XC_LANGUAGE,
    // falling back to the table's default), and the unlocks recorded in `storage`.
    void Load(const uint8_t* xdbf, size_t size, uint32_t language, const std::filesystem::path& storage);

    // In the title's order. Empty until Load.
    const std::vector<Achievement>& All();

    // When it was unlocked (Unix seconds), or 0 while locked.
    int64_t UnlockTime(uint16_t id);

    // Records an unlock and saves it. Returns the achievement if this unlocked it, null if it was already
    // unlocked or is unknown.
    const Achievement* Unlock(uint16_t id);
}
