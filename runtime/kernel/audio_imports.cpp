#include "../audio/audio_output.h"
#include "../audio/xma.h"
#include "../thread.h"
#include "xbox_types.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace rt
{
    // Render driver clients (Wwise, Bink). Each gets a host thread that runs its mixer callback
    // whenever its output queue has room.
    struct AudioClient
    {
        uint32_t callback = 0;
        uint32_t wrappedArgument = 0; // guest word holding the callback argument; the callback receives its address
        std::atomic<bool> active{ false };
        bool threadStarted = false;
    };
    static std::mutex g_clientsMutex;
    static AudioClient g_clients[AudioMaxClients];

    constexpr uint32_t DriverHandleBase = 0x41550000;
    constexpr size_t QueuedFrames = 6; // about 32 ms at 48 kHz

    static void AudioDriverThread(size_t index)
    {
        AudioClient& client = g_clients[index];
        while (true)
        {
            WaitForAudioQueueBelow(index, QueuedFrames);
            if (client.active)
                GuestCall(client.callback, { client.wrappedArgument });
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    static uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
    {
        *config = 0x00010001;
        return ERROR_SUCCESS;
    }

    static uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
    {
        if (!StartAudioOutput())
            Log("Audio output unavailable; continuing silently");

        std::lock_guard lock(g_clientsMutex);
        for (size_t i = 0; i < AudioMaxClients; i++)
        {
            AudioClient& client = g_clients[i];
            if (client.active)
                continue;
            client.callback = callback[0];
            if (!client.wrappedArgument)
                client.wrappedArgument = g_systemHeap.Alloc(4);
            *Translate<be<uint32_t>>(client.wrappedArgument) = uint32_t(callback[1]);
            ClearAudioClient(i);
            client.active = true;
            if (!client.threadStarted)
            {
                client.threadStarted = true;
                CreateHostThread(fmt::format("XAudio client {}", i), [i] { AudioDriverThread(i); });
            }
            *driver = DriverHandleBase | uint32_t(i);
            return ERROR_SUCCESS;
        }
        Log("XAudioRegisterRenderDriverClient: no free client slots");
        return 0x8007000E; // E_OUTOFMEMORY
    }

    static uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
    {
        size_t index = driver & 0xFFFF;
        if (index < AudioMaxClients)
        {
            g_clients[index].active = false;
            ClearAudioClient(index);
        }
        return ERROR_SUCCESS;
    }

    static uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, const float* samples)
    {
        size_t index = driver & 0xFFFF;
        if (index < AudioMaxClients)
            SubmitAudioFrame(index, samples);
        return ERROR_SUCCESS;
    }

    static uint32_t XAudioGetVoiceCategoryVolume(uint32_t, be<float>* volume)
    {
        *volume = 1.0f;
        return ERROR_SUCCESS;
    }

    static uint32_t XAudioEnableDucker(uint32_t) { return ERROR_SUCCESS; }
    static uint32_t XAudioGetDuckerLevel(be<float>* level) { if (level) *level = 1.0f; return ERROR_SUCCESS; }
    static uint32_t XAudioGetDuckerThreshold(be<float>* v) { if (v) *v = 0.0f; return ERROR_SUCCESS; }
    static uint32_t XAudioGetDuckerAttackTime(be<uint32_t>* v) { if (v) *v = 0; return ERROR_SUCCESS; }
    static uint32_t XAudioGetDuckerReleaseTime(be<uint32_t>* v) { if (v) *v = 0; return ERROR_SUCCESS; }
    static uint32_t XAudioGetDuckerHoldTime(be<uint32_t>* v) { if (v) *v = 0; return ERROR_SUCCESS; }
}

using namespace rt;
GUEST_IMPORT(XAudioGetSpeakerConfig, XAudioGetSpeakerConfig)
GUEST_IMPORT(XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient)
GUEST_IMPORT(XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient)
GUEST_IMPORT(XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume)
GUEST_IMPORT(XAudioEnableDucker, XAudioEnableDucker)
GUEST_IMPORT(XAudioGetDuckerLevel, XAudioGetDuckerLevel)
GUEST_IMPORT(XAudioGetDuckerThreshold, XAudioGetDuckerThreshold)
GUEST_IMPORT(XAudioGetDuckerAttackTime, XAudioGetDuckerAttackTime)
GUEST_IMPORT(XAudioGetDuckerReleaseTime, XAudioGetDuckerReleaseTime)
GUEST_IMPORT(XAudioGetDuckerHoldTime, XAudioGetDuckerHoldTime)

namespace rt
{
    // XMA contexts are allocated by the kernel; titles program them directly afterwards.
    static uint32_t XMACreateContext(be<uint32_t>* context)
    {
        uint32_t allocated = xma::AllocateContext();
        *context = allocated;
        return allocated ? 0 : 0xC0000017; // STATUS_NO_MEMORY
    }

    static void XMAReleaseContext(uint32_t context)
    {
        xma::ReleaseContext(context);
    }
}

GUEST_IMPORT(XMACreateContext, rt::XMACreateContext)
GUEST_IMPORT(XMAReleaseContext, rt::XMAReleaseContext)
