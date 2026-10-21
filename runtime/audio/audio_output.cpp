// Host audio output: guest XAudio clients' frames, mixed to stereo for Core Audio.
#include "audio_output.h"
#include "../log.h"
#include <AudioToolbox/AudioToolbox.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace rt
{
    // Each client (Wwise, Bink) feeds a fixed ring of stereo frames: one producer (the client's driver
    // thread), one consumer (the Core Audio render callback). The render callback runs on a real-time
    // thread, so it takes no locks and allocates nothing.
    constexpr size_t RingFrames = 16;
    constexpr size_t FrameFloats = AudioFrameSamples * 2;

    struct ClientRing
    {
        std::array<std::array<float, FrameFloats>, RingFrames> frames;
        std::atomic<uint64_t> written{ 0 }; // frames produced (producer only)
        std::atomic<uint64_t> read{ 0 };    // frames consumed (consumer only)
        size_t offset = 0;                  // floats of the current frame consumed (consumer only)
        std::atomic<bool> clear{ false };   // drop everything queued (handled by the consumer)
        bool playing = false;               // had audio at the end of the last mix (consumer only)
    };
    static ClientRing g_clients[AudioMaxClients];
    static AudioComponentInstance g_unit;
    static std::mutex g_startMutex;
    static bool g_started, g_deviceStarted;

    // Diagnostics, gathered with atomics on the output thread and reported from a normal one.
    // PEGGLE2_AUDIO_STATS: per second, the peak level, clipped samples, mixes where a playing client ran dry,
    // and frames dropped because a ring was full. PEGGLE2_AUDIO_DUMP=file.raw: the output as raw stereo
    // float32 at 48 kHz (up to two minutes).
    static const bool g_stats = getenv("PEGGLE2_AUDIO_STATS") != nullptr;
    static std::atomic<uint32_t> g_underruns, g_dropped, g_clipped;
    static std::atomic<float> g_peak;
    static std::vector<float> g_capture;
    static std::atomic<size_t> g_captured;

    // Mixes every client's queued audio into out (stereo interleaved). Consumer side.
    static void Mix(float* out, size_t frameCount)
    {
        const size_t needed = frameCount * 2;
        memset(out, 0, needed * sizeof(float));
        for (ClientRing& client : g_clients)
        {
            uint64_t read = client.read.load(std::memory_order_relaxed);
            if (client.clear.exchange(false, std::memory_order_acquire))
            {
                read = client.written.load(std::memory_order_acquire);
                client.offset = 0;
            }
            const uint64_t written = client.written.load(std::memory_order_acquire);
            // Underruns leave silence for this client only.
            size_t done = 0;
            while (done < needed && read != written)
            {
                const float* frame = client.frames[read % RingFrames].data();
                const size_t n = std::min(needed - done, FrameFloats - client.offset);
                for (size_t i = 0; i < n; i++)
                    out[done + i] += frame[client.offset + i];
                done += n;
                client.offset += n;
                if (client.offset == FrameFloats)
                {
                    client.offset = 0;
                    read++;
                }
            }
            // Ran out mid-mix while playing: the rest of this mix is a gap, heard as a click.
            if (client.playing && done < needed)
                g_underruns.fetch_add(1, std::memory_order_relaxed);
            client.playing = done == needed || read != written;
            client.read.store(read, std::memory_order_release);
        }

        // Soft knee above 0.9 so the rare overs of the unnormalized downmix round off instead of clipping.
        for (size_t i = 0; i < needed; i++)
        {
            const float magnitude = std::abs(out[i]);
            if (magnitude > 0.9f)
                out[i] = std::copysign(0.9f + 0.1f * std::tanh((magnitude - 0.9f) * 10.0f), out[i]);
        }
    }

    // What reaches the output, for the diagnostics above. Output thread: no locks, no allocation.
    static void Observe(const float* out, size_t frameCount)
    {
        const size_t n = frameCount * 2;
        if (g_stats)
        {
            float peak = 0;
            uint32_t clipped = 0;
            for (size_t i = 0; i < n; i++)
            {
                peak = std::max(peak, std::abs(out[i]));
                clipped += std::abs(out[i]) > 1.0f;
            }
            if (peak > g_peak.load(std::memory_order_relaxed))
                g_peak.store(peak, std::memory_order_relaxed);
            g_clipped.fetch_add(clipped, std::memory_order_relaxed);
        }
        if (!g_capture.empty())
        {
            const size_t at = g_captured.load(std::memory_order_relaxed);
            if (at + n <= g_capture.size())
            {
                memcpy(g_capture.data() + at, out, n * sizeof(float));
                g_captured.store(at + n, std::memory_order_release);
            }
        }
    }

    static void DiagnosticsThread(FILE* dump)
    {
        size_t written = 0;
        for (int tick = 1;; tick++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (dump)
            {
                const size_t end = g_captured.load(std::memory_order_acquire);
                fwrite(g_capture.data() + written, sizeof(float), end - written, dump);
                fflush(dump);
                written = end;
            }
            if (g_stats && tick % 10 == 0)
                Log("Audio stats: peak {:.3f}, {} clipped samples, {} underruns, {} dropped frames", g_peak.exchange(0),
                    g_clipped.exchange(0), g_underruns.exchange(0), g_dropped.exchange(0));
        }
    }

    static OSStatus Render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frameCount, AudioBufferList* buffers)
    {
        float* out = static_cast<float*>(buffers->mBuffers[0].mData);
        Mix(out, frameCount);
        Observe(out, frameCount);
        return noErr;
    }

    // Without an output device the game still needs its audio consumed at the real rate (its mixers and
    // video playback are paced by it), so a thread plays it to nowhere.
    static void SilentOutputThread()
    {
        constexpr size_t Chunk = 480; // 10 ms
        static float scratch[Chunk * 2];
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
            if (const auto now = std::chrono::steady_clock::now(); now - next > std::chrono::milliseconds(100))
                next = now;
            Mix(scratch, Chunk);
            Observe(scratch, Chunk);
        }
    }

    static bool StartDevice()
    {
        AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component || AudioComponentInstanceNew(component, &g_unit) != noErr)
            return false;

        AudioStreamBasicDescription format{};
        format.mSampleRate = AudioSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 8;
        format.mBytesPerPacket = 8;
        AURenderCallbackStruct callback{ Render, nullptr };
        // Pull one XAudio frame at a time, as the console's hardware does, so the title renders only a frame
        // or two ahead (see AudioDriverThread). The device may round it to what it supports.
        UInt32 bufferFrames = AudioFrameSamples;
        AudioUnitSetProperty(g_unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufferFrames, sizeof(bufferFrames));
        if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr ||
            AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr ||
            AudioUnitInitialize(g_unit) != noErr || AudioOutputUnitStart(g_unit) != noErr)
        {
            AudioComponentInstanceDispose(g_unit);
            g_unit = nullptr;
            return false;
        }
        return true;
    }

    bool StartAudioOutput()
    {
        std::lock_guard lock(g_startMutex);
        if (g_started)
            return g_deviceStarted;
        g_started = true;
        FILE* dump = nullptr;
        if (const char* path = getenv("PEGGLE2_AUDIO_DUMP"); path && (dump = fopen(path, "wb")))
            g_capture.resize(AudioSampleRate * 2 * 120);
        if (g_stats || dump)
            std::thread(DiagnosticsThread, dump).detach();
        // PEGGLE2_NO_AUDIO_DEVICE plays to nowhere even when a device is present (for tests).
        g_deviceStarted = !getenv("PEGGLE2_NO_AUDIO_DEVICE") && StartDevice();
        if (!g_deviceStarted)
        {
            Log("Audio: no output device; continuing silently");
            std::thread(SilentOutputThread).detach();
        }
        return g_deviceStarted;
    }

    static float LoadBigEndianFloat(const float* p)
    {
        uint32_t bits;
        memcpy(&bits, p, 4);
        bits = __builtin_bswap32(bits);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    void SubmitAudioFrame(size_t client, const float* in)
    {
        ClientRing& ring = g_clients[client];
        const uint64_t written = ring.written.load(std::memory_order_relaxed);
        if (written - ring.read.load(std::memory_order_acquire) >= RingFrames)
        {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return; // full: the client got ahead of the device (WaitForAudioQueueBelow normally prevents it)
        }

        // The title always mixes 5.1 (Wwise spreads into the surrounds even for stereo speaker configs); the
        // console downmixed for stereo sets. ITU-R BS.775 coefficients: fronts at unity, center and
        // surrounds at -3 dB, LFE dropped (no normalization: the console's downmix is not quieter).
        constexpr size_t N = AudioFrameSamples;
        float* stereo = ring.frames[written % RingFrames].data();
        for (size_t i = 0; i < N; i++)
        {
            float fl = LoadBigEndianFloat(in + 0 * N + i);
            float fr = LoadBigEndianFloat(in + 1 * N + i);
            float fc = LoadBigEndianFloat(in + 2 * N + i);
            float bl = LoadBigEndianFloat(in + 4 * N + i);
            float br = LoadBigEndianFloat(in + 5 * N + i);
            stereo[i * 2] = fl + 0.7071f * (fc + bl);
            stereo[i * 2 + 1] = fr + 0.7071f * (fc + br);
        }
        ring.written.store(written + 1, std::memory_order_release);
    }

    void WaitForAudioQueueBelow(size_t client, size_t frames)
    {
        ClientRing& ring = g_clients[client];
        // The consumer is real-time and cannot signal, so poll; a frame plays in about 5 ms.
        while (ring.written.load(std::memory_order_relaxed) - ring.read.load(std::memory_order_acquire) >= frames)
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }

    size_t QueuedAudioFrames(size_t client)
    {
        const ClientRing& ring = g_clients[client];
        return size_t(ring.written.load(std::memory_order_relaxed) - ring.read.load(std::memory_order_acquire));
    }

    void ClearAudioClient(size_t client)
    {
        g_clients[client].clear.store(true, std::memory_order_release);
    }
}
