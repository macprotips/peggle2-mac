// XAM state shared across imports: user data location, overlapped completion, language, saves.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>

namespace rt
{
    // Where profile settings and save content are stored on the host.
    void InitXam(const std::filesystem::path& userData);

    // XAM_OVERLAPPED: completes an asynchronous XAM request immediately. Returns ERROR_IO_PENDING.
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

    // The console language (XC_LANGUAGE), from the Mac's preferred language.
    uint32_t ConsoleLanguage();

    // Saves being written right now (files open for writing on a writable device); quitting waits for them.
    std::atomic<int>& SavesInProgress();

    // A system UI (dialog, Guide-like window) opened or closed; the title is told while any is open.
    void SystemUIShown(bool shown);

    // Queues a notification (XN_*) for every listener whose mask accepts it.
    void BroadcastNotification(uint32_t id, uint32_t data);
}
