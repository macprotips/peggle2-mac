// XMA audio decoder hardware: 320 contexts in physical memory, driven through registers at 0x7FEA0000.
// Decoding is Xenia's XmaContext (vendored, BSD-3-Clause) on Xenia's FFmpeg fork; this file stands in
// for Xenia's XmaDecoder (register file, context allocation and the worker thread).
#include "xma.h"
#include "../log.h"
#include "../memory.h"
#include "xenia/apu/xma_context.h"
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rt
{
    uint8_t* TranslateGuest(uint32_t address)
    {
        return Translate(address);
    }
}

namespace rt::xma
{
    constexpr uint32_t RegisterBase = 0x7FEA0000;
    constexpr uint32_t ContextCount = 320;
    constexpr uint32_t ContextSize = 64;

    // Register indices (dwords from RegisterBase), from Xenia's xma_register_table.inc.
    enum : uint32_t
    {
        ContextArrayAddress = 0x600,
        CurrentContextIndex = 0x606,
        Context0Kick = 0x650,
        Context0Lock = 0x690,
        Context0Clear = 0x6A0,
    };

    static xe::Memory g_memory;
    static xe::apu::XmaContext g_contexts[ContextCount];
    static uint32_t g_contextArray; // guest virtual address
    static std::mutex g_allocationMutex;
    static std::bitset<ContextCount> g_allocated;
    static std::mutex g_workMutex;
    static std::condition_variable g_workCv;
    static bool g_workPending;

    // Registers the title reads with plain loads keep their value in guest memory, little-endian like the
    // hardware (the title reads them with lwbrx).
    static void StoreRegister(uint32_t index, uint32_t value)
    {
        memcpy(Translate(RegisterBase + index * 4), &value, 4);
    }

    static void WorkerThread()
    {
        uint32_t rotation = 0;
        while (true)
        {
            bool didWork = false;
            for (uint32_t i = 0; i < ContextCount; i++)
                didWork = g_contexts[i].Work() || didWork;
            static const bool stats = getenv("PEGGLE2_AUDIO_STATS") != nullptr;
            static uint32_t decodes;
            static auto last = std::chrono::steady_clock::now();
            decodes += didWork;
            if (stats && std::chrono::steady_clock::now() - last >= std::chrono::seconds(1))
            {
                Log("XMA stats: {} decode passes", decodes);
                decodes = 0;
                last = std::chrono::steady_clock::now();
            }
            // The rotating hardware context index: titles wait for it to move past a context they lock.
            rotation = (rotation + 1) % ContextCount;
            StoreRegister(CurrentContextIndex, rotation);
            if (!didWork)
            {
                std::unique_lock lock(g_workMutex);
                g_workCv.wait_for(lock, std::chrono::milliseconds(2), [] { return g_workPending; });
                g_workPending = false;
            }
        }
    }

    void Initialize()
    {
        uint32_t physical = g_physicalHeap.Alloc(ContextCount * ContextSize, 256);
        if (!physical)
            Fatal("XMA: cannot allocate the context array");
        g_contextArray = PhysicalToVirtual(physical);
        memset(Translate(g_contextArray), 0, ContextCount * ContextSize);
        for (uint32_t i = 0; i < ContextCount; i++)
        {
            if (g_contexts[i].Setup(i, &g_memory, g_contextArray + i * ContextSize))
                Fatal("XMA: decoder unavailable (FFmpeg xmaframes)");
        }
        StoreRegister(ContextArrayAddress, physical);
        std::thread(WorkerThread).detach();
    }

    uint32_t AllocateContext()
    {
        std::lock_guard lock(g_allocationMutex);
        for (uint32_t i = 0; i < ContextCount; i++)
        {
            if (g_allocated[i])
                continue;
            g_allocated[i] = true;
            g_contexts[i].set_is_allocated(true);
            return g_contextArray + i * ContextSize;
        }
        return 0;
    }

    void ReleaseContext(uint32_t context)
    {
        uint32_t index = (context - g_contextArray) / ContextSize;
        if (context < g_contextArray || index >= ContextCount)
            return;
        std::lock_guard lock(g_allocationMutex);
        if (!g_allocated[index])
            return;
        g_contexts[index].Release();
        g_allocated[index] = false;
    }

    void MmioWrite(uint32_t address, uint32_t stored)
    {
        // `stored` is the value as a big-endian store would lay it out; the hardware is little-endian.
        const uint32_t value = __builtin_bswap32(stored);
        const uint32_t index = (address - RegisterBase) / 4;
        StoreRegister(index, value);
        auto forEachBit = [&](uint32_t group, auto&& action) {
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                uint32_t id = group * 32 + bit;
                if ((value >> bit) & 1 && id < ContextCount)
                    action(g_contexts[id]);
            }
        };
        if (index >= Context0Kick && index < Context0Kick + 10)
        {
            forEachBit(index - Context0Kick, [](xe::apu::XmaContext& c) { c.Enable(); });
            std::lock_guard lock(g_workMutex);
            g_workPending = true;
            g_workCv.notify_one();
        }
        else if (index >= Context0Lock && index < Context0Lock + 10)
            forEachBit(index - Context0Lock, [](xe::apu::XmaContext& c) { c.Disable(); });
        else if (index >= Context0Clear && index < Context0Clear + 10)
            forEachBit(index - Context0Clear, [](xe::apu::XmaContext& c) { c.Clear(); });
    }
}
