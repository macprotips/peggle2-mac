// XAudio render driver imports (the clients Wwise and Bink mix through) and the XMA decoder interface.
#include "../audio/audio_output.h"
#include "../audio/xma.h"
#include "../thread.h"
#include "xbox_types.h"
#include <atomic>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
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

    // The console's audio hardware pulls each 256-sample frame just before it plays, one every 5.33 ms, and
    // titles pace their voices to that: Bink feeds its video's sound in step with the video clock, so its voice
    // holds only what is due and runs dry (an audible gap) if frames are rendered much earlier. So each client
    // renders one frame per period, keeping only LeadFrames queued, and refills at once only after a stall.
    constexpr size_t LeadFrames = 4;
    static void AudioDriverThread(size_t index)
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto FramePeriod = std::chrono::nanoseconds(1'000'000'000ull * AudioFrameSamples / AudioSampleRate);
        AudioClient& client = g_clients[index];
        // Real-time scheduling, as macOS audio apps use: the kernel runs this thread every period however busy
        // the Mac is. A frame's mix takes well under 1.5 ms of each period.
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        const auto ticks = [&](std::chrono::nanoseconds t) { return uint32_t(uint64_t(t.count()) * timebase.denom / timebase.numer); };
        thread_time_constraint_policy_data_t policy{ ticks(FramePeriod), ticks(std::chrono::microseconds(1500)), ticks(FramePeriod), false };
        thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        auto next = Clock::now();
        while (true)
        {
            WaitForAudioQueueBelow(index, LeadFrames + 1);
            if (!client.active)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                next = Clock::now();
                continue;
            }
            GuestCall(client.callback, { client.wrappedArgument });
            next += FramePeriod;
            if (const auto now = Clock::now(); now > next || QueuedAudioFrames(index) < LeadFrames)
                next = now; // behind: catch up without waiting
            else
                std::this_thread::sleep_until(next);
        }
    }

    static uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
    {
        *config = 0x00010001; // as Xenia reports
        return ERROR_SUCCESS;
    }

    static uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
    {
        StartAudioOutput();

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
        return E_OUTOFMEMORY;
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
        return allocated ? 0 : STATUS_NO_MEMORY;
    }

    static void XMAReleaseContext(uint32_t context)
    {
        xma::ReleaseContext(context);
    }
}

GUEST_IMPORT(XMACreateContext, rt::XMACreateContext)
GUEST_IMPORT(XMAReleaseContext, rt::XMAReleaseContext)
