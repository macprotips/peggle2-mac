#pragma once
#include <cstddef>

namespace rt
{
    constexpr size_t AudioSampleRate = 48000;
    constexpr size_t AudioFrameSamples = 256; // per channel, per XAudio frame
    constexpr size_t AudioGuestChannels = 6;  // FL FR FC LFE BL BR, planar big-endian floats

    // Starts the host output device. Safe to call more than once.
    bool StartAudioOutput();

    constexpr size_t AudioMaxClients = 8;

    // Queues one guest frame (planar 5.1, big-endian floats) from a client, mixed down to stereo.
    // Clients are mixed together at output.
    void SubmitAudioFrame(size_t client, const float* guestPlanar);

    // Blocks until the client has fewer than `frames` frames queued.
    void WaitForAudioQueueBelow(size_t client, size_t frames);

    // Drops a client's queued audio.
    // Frames queued for the client and not yet played.
    size_t QueuedAudioFrames(size_t client);
    void ClearAudioClient(size_t client);
}
