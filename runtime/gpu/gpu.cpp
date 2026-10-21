#include "gpu.h"
#include "../log.h"
#include "../memory.h"
#include "../thread.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <ctime>
#include <pthread.h>
#include <pthread/qos.h>

extern char** environ;

namespace rt::gpu
{
    enum : uint32_t
    {
        PM4_NOP = 0x10,
        PM4_REG_RMW = 0x21,
        PM4_DRAW_INDX = 0x22,
        PM4_VIZ_QUERY = 0x23,
        PM4_WAIT_FOR_IDLE = 0x26,
        PM4_IM_LOAD = 0x27,
        PM4_IM_LOAD_IMMEDIATE = 0x2B,
        PM4_SET_CONSTANT = 0x2D,
        PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_DRAW_INDX_2 = 0x36,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_INVALIDATE_STATE = 0x3B,
        PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D,
        PM4_REG_TO_MEM = 0x3E,
        PM4_INDIRECT_BUFFER = 0x3F,
        PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46,
        PM4_ME_INIT = 0x48,
        PM4_SET_BIN_MASK = 0x50,
        PM4_SET_BIN_SELECT = 0x51,
        PM4_INTERRUPT = 0x54,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_EVENT_WRITE_SHD = 0x58,
        PM4_EVENT_WRITE_EXT = 0x5A,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_CONTEXT_UPDATE = 0x5E,
        PM4_SET_BIN_MASK_LO = 0x60,
        PM4_SET_BIN_MASK_HI = 0x61,
        PM4_SET_BIN_SELECT_LO = 0x62,
        PM4_SET_BIN_SELECT_HI = 0x63,
        PM4_XE_SWAP = 0x64, // written by our VdSwap
    };

    void DumpShader(const ShaderLoad& load);
    static xe::gpu::RegisterFile g_regs;
    static const bool g_tracePackets = getenv("PEGGLE2_GPU_TRACE") != nullptr;
    static Backend g_backend;

    static uint32_t g_ringBase;        // physical
    static uint32_t g_ringSize;        // bytes
    static uint32_t g_readIndex;       // dwords
    static std::atomic<uint64_t> g_swapsSubmitted; // VdSwap calls (the command processor counts g_frames)
    static std::atomic<bool> g_cpWaiting;          // the command processor is blocked in WAIT_REG_MEM
    static std::chrono::steady_clock::duration g_cpWaited; // time blocked in WAIT_REG_MEM this frame (PEGGLE2_PERF)
    static uint32_t g_writeBack;       // physical, 0 if disabled

    static std::atomic<uint32_t> g_interruptCallback;
    static std::atomic<uint32_t> g_interruptUserData;
    static std::atomic<uint64_t> g_frames;
    static uint64_t g_binSelect = ~0ull;
    static uint64_t g_binMask = ~0ull;

    uint8_t* TranslatePhysical(uint32_t physical)
    {
        return Translate(PhysicalToVirtual(physical));
    }

    uint32_t ReadRegister(uint32_t index)
    {
        return index < RegisterCount ? g_regs[index] : 0;
    }

    const xe::gpu::RegisterFile& Registers()
    {
        return g_regs;
    }

    uint64_t FrameCount()
    {
        return g_frames;
    }

    void SetBackend(Backend backend)
    {
        g_backend = std::move(backend);
    }

    static uint32_t Swap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
        case 2: return __builtin_bswap32(value);
        case 3: return (value >> 16) | (value << 16);
        default: return value;
        }
    }

    // Fences and interrupts tell the guest that earlier commands are done, so they must be.
    static std::chrono::steady_clock::duration g_gpuWaited; // time in Sync and presenting this frame (PEGGLE2_PERF)
    extern std::chrono::steady_clock::duration g_settleWait; // textures.mm
    static void Sync()
    {
        const auto start = std::chrono::steady_clock::now();
        if (g_backend.sync)
            g_backend.sync();
        g_gpuWaited += std::chrono::steady_clock::now() - start;
    }

    // A GPU memory write as the hardware does it: the low address bits pick the endian swap.
    static void StoreMemory(uint32_t address, uint32_t value)
    {
        Sync();
        uint32_t stored = Swap(value, address & 3);
        memcpy(TranslatePhysical(address & ~3u), &stored, 4);
    }

    static uint32_t LoadMemory(uint32_t address)
    {
        uint32_t raw;
        memcpy(&raw, TranslatePhysical(address & ~3u), 4);
        return Swap(raw, address & 3);
    }

    // CPU-visible registers: mirrored big-endian into the MMIO window so title reads see them.
    static void MirrorRegister(uint32_t index)
    {
        *Translate<be<uint32_t>>(GpuRegisterBase + index * 4) = g_regs[index];
    }

    // --- Interrupts ----------------------------------------------------------------------------

    struct Interrupt
    {
        uint32_t source;
        uint32_t cpu;
    };
    static std::mutex g_interruptMutex;
    static std::condition_variable g_interruptCv;
    static std::deque<Interrupt> g_interrupts;

    static void RaiseInterrupt(uint32_t source, uint32_t cpu)
    {
        std::lock_guard lock(g_interruptMutex);
        g_interrupts.push_back({ source, cpu });
        g_interruptCv.notify_one();
    }

    // The D3D interrupt service routine runs on its own guest-capable thread.
    static void InterruptThread()
    {
        while (true)
        {
            Interrupt interrupt;
            {
                std::unique_lock lock(g_interruptMutex);
                g_interruptCv.wait(lock, [] { return !g_interrupts.empty(); });
                interrupt = g_interrupts.front();
                g_interrupts.pop_front();
            }
            uint32_t callback = g_interruptCallback;
            if (!callback)
                continue;
            // KPCR current CPU, which the routine reads.
            *Translate<uint8_t>(CurrentThread()->pcr + 0x10C) = uint8_t(interrupt.cpu);
            GuestCall(callback, { interrupt.source, g_interruptUserData });
        }
    }

    static void VblankThread()
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        // 60 Hz like the console's display. The title's logic is paced by these interrupts, so the rate
        // is game speed, not a frame-rate setting.
        const auto period = std::chrono::nanoseconds(1000000000LL / 60);
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            next += period;
            std::this_thread::sleep_until(next);
            // After the Mac sleeps (or the thread is starved) resume from now instead of raising every
            // missed vblank at once, which would fast-forward the game.
            if (const auto now = std::chrono::steady_clock::now(); now - next > 4 * period)
                next = now;
            RaiseInterrupt(0, 2);
        }
    }

    // --- Command processor ---------------------------------------------------------------------

    static void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= RegisterCount)
            return;
        g_regs[index] = value;
        if (index >= REG_SCRATCH_REG0 && index <= REG_SCRATCH_REG7)
        {
            // Scratch registers are mirrored to memory when unmasked; D3D fences rely on it.
            uint32_t n = index - REG_SCRATCH_REG0;
            Sync();
            if ((1u << n) & g_regs[REG_SCRATCH_UMSK])
                *reinterpret_cast<be<uint32_t>*>(TranslatePhysical(g_regs[REG_SCRATCH_ADDR] + n * 4)) = value;
        }
        else if (index == REG_COHER_STATUS_HOST)
            g_regs[index] |= 0x80000000;
        MirrorRegister(index);
    }

    struct Reader
    {
        const uint8_t* base; // guest memory
        uint32_t size;       // bytes; ring buffers wrap
        uint32_t offset;     // bytes
        uint32_t remaining;  // bytes

        uint32_t Read()
        {
            uint32_t raw;
            memcpy(&raw, base + offset, 4);
            offset = (offset + 4) % size;
            remaining -= 4;
            return __builtin_bswap32(raw);
        }

        void Skip(uint32_t dwords)
        {
            offset = (offset + dwords * 4) % size;
            remaining -= dwords * 4;
        }
    };

    static bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        case 7: return true;
        default: return false;
        }
    }

    static void ExecuteBuffer(Reader reader);

    static void ExecuteType3(Reader& r, uint32_t packet)
    {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (g_tracePackets)
        {
            std::string words;
            Reader peek = r;
            for (uint32_t i = 0; i < count && i < 8; i++)
                words += fmt::format(" {:08X}", peek.Read());
            Log("PM4 {:02X} x{}:{}", opcode, count, words);
        }
        const uint32_t endOffset = (r.offset + count * 4) % r.size;
        const uint32_t endRemaining = r.remaining - count * 4;

        // Predicated (tiled) packets run only for selected bins; swaps never are.
        if ((packet & 1) && (!(g_binSelect & g_binMask) || opcode == PM4_XE_SWAP))
        {
            r.Skip(count);
            return;
        }

        switch (opcode)
        {
        case PM4_INTERRUPT:
        {
            uint32_t cpuMask = r.Read();
            Sync();
            for (uint32_t cpu = 0; cpu < 6; cpu++)
            {
                if (cpuMask & (1u << cpu))
                    RaiseInterrupt(1, cpu);
            }
            break;
        }
        case PM4_XE_SWAP:
        {
            r.Read(); // signature
            uint32_t frontbuffer = r.Read();
            uint32_t width = r.Read();
            uint32_t height = r.Read();
            const auto swapStart = std::chrono::steady_clock::now();
            if (g_backend.swap)
                g_backend.swap(frontbuffer, width, height);
            g_gpuWaited += std::chrono::steady_clock::now() - swapStart;
            g_frames++;
            // PEGGLE2_PERF: per second, the most command processor time a frame took (waits for guest memory,
            // the GPU and presenting excluded), and the most it waited for the GPU and presenting.
            static const bool perf = getenv("PEGGLE2_PERF") != nullptr;
            if (perf)
            {
                using Clock = std::chrono::steady_clock;
                static auto frameStart = Clock::now(), second = frameStart;
                static double maxMs, maxGpuMs, maxCpuMs, maxSettleMs;
                static uint64_t lastCpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
                const uint64_t cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
                maxCpuMs = std::max(maxCpuMs, (cpu - lastCpu) / 1e6);
                lastCpu = cpu;
                const auto now = Clock::now();
                maxMs = std::max(maxMs, std::chrono::duration<double, std::milli>(now - frameStart - g_cpWaited - g_gpuWaited).count());
                maxGpuMs = std::max(maxGpuMs, std::chrono::duration<double, std::milli>(g_gpuWaited).count());
                maxSettleMs = std::max(maxSettleMs, std::chrono::duration<double, std::milli>(g_settleWait).count());
                g_cpWaited = g_gpuWaited = g_settleWait = {};
                frameStart = now;
                if (now - second >= std::chrono::seconds(1))
                {
                    Log("Perf: command processor frame max {:.1f} ms (CPU time {:.1f} ms), GPU and present waits max {:.1f} ms, texture settle waits max {:.1f} ms",
                        maxMs, maxCpuMs, maxGpuMs, maxSettleMs);
                    maxMs = maxGpuMs = maxCpuMs = maxSettleMs = 0;
                    second = now;
                }
            }
            break;
        }
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t address = r.Read() & 0x1FFFFFFF;
            uint32_t length = r.Read() & 0xFFFFF;
            ExecuteBuffer({ TranslatePhysical(address), length * 4, 0, length * 4 });
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t info = r.Read();
            uint32_t poll = r.Read();
            uint32_t reference = r.Read();
            uint32_t mask = r.Read();
            uint32_t wait = r.Read();
            const bool memory = info & 0x10;
            const auto waitStart = std::chrono::steady_clock::now();
            bool reported = false;
            while (true)
            {
                if (!reported && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(5))
                {
                    reported = true;
                    Log("GPU: WAIT_REG_MEM on {} {:X} for {:X} (mask {:X}, function {}) still waiting after 5 s", memory ? "memory" : "register",
                        poll, reference, mask, info & 7);
                }
                uint32_t value = memory ? LoadMemory(poll) : g_regs[poll & 0x7FFF];
                if (!memory && poll == REG_COHER_STATUS_HOST)
                {
                    g_regs[REG_COHER_STATUS_HOST] = 0; // caches are coherent on the host
                    value = 0;
                }
                if (Compare(info, value & mask, reference))
                    break;
                g_cpWaiting.store(true, std::memory_order_relaxed);
                if (wait >= 0x100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
                else
                    std::this_thread::yield();
            }
            g_cpWaiting.store(false, std::memory_order_relaxed);
            g_cpWaited += std::chrono::steady_clock::now() - waitStart;
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = r.Read();
            uint32_t andMask = r.Read();
            uint32_t orMask = r.Read();
            uint32_t value = g_regs[info & 0x1FFF];
            value &= (info >> 31) ? g_regs[andMask & 0x1FFF] : andMask;
            value |= ((info >> 30) & 1) ? g_regs[orMask & 0x1FFF] : orMask;
            WriteRegister(info & 0x1FFF, value);
            break;
        }
        case PM4_REG_TO_MEM:
        {
            uint32_t reg = r.Read();
            uint32_t address = r.Read();
            StoreMemory(address, g_regs[reg & 0x7FFF]);
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t address = r.Read();
            for (uint32_t i = 1; i < count; i++, address += 4)
                StoreMemory(address, r.Read());
            break;
        }
        case PM4_COND_WRITE:
        {
            uint32_t info = r.Read();
            uint32_t poll = r.Read();
            uint32_t reference = r.Read();
            uint32_t mask = r.Read();
            uint32_t target = r.Read();
            uint32_t data = r.Read();
            uint32_t value = (info & 0x10) ? LoadMemory(poll) : g_regs[poll & 0x7FFF];
            if (Compare(info, value & mask, reference))
            {
                if (info & 0x100)
                    StoreMemory(target, data);
                else
                    WriteRegister(target, data);
            }
            break;
        }
        case PM4_EVENT_WRITE:
            WriteRegister(REG_VGT_EVENT_INITIATOR, r.Read() & 0x3F);
            break;
        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = r.Read();
            uint32_t address = r.Read();
            uint32_t value = r.Read();
            WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
            StoreMemory(address, (initiator >> 31) ? uint32_t(g_frames) : value);
            break;
        }
        case PM4_EVENT_WRITE_EXT:
        {
            uint32_t initiator = r.Read();
            uint32_t address = r.Read();
            WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
            // Screen extents: the whole 8192x8192 range, z 0-1, as 16-bit values.
            const uint16_t extents[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
            uint8_t* p = TranslatePhysical(address & ~3u);
            for (int i = 0; i < 6; i++)
            {
                p[i * 2] = uint8_t(extents[i] >> 8);
                p[i * 2 + 1] = uint8_t(extents[i]);
            }
            break;
        }
        case PM4_EVENT_WRITE_ZPD:
            WriteRegister(REG_VGT_EVENT_INITIATOR, r.Read() & 0x3F);
            break;
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
        {
            if (opcode == PM4_DRAW_INDX)
                r.Read(); // viz query condition
            uint32_t initiator = r.Read();
            WriteRegister(REG_VGT_DRAW_INITIATOR, initiator);
            DrawRequest draw{};
            draw.primitiveType = initiator & 0x3F;
            draw.indexCount = initiator >> 16;
            uint32_t source = (initiator >> 6) & 3;
            if (source == 0) // DMA: indices from memory
            {
                draw.indexed = true;
                draw.indexBase = r.Read();
                uint32_t dmaSize = r.Read();
                WriteRegister(REG_VGT_DMA_BASE, draw.indexBase);
                WriteRegister(REG_VGT_DMA_SIZE, dmaSize);
                draw.indexFormat = (initiator >> 11) & 1;
                draw.indexEndian = dmaSize >> 30;
            }
            if (g_backend.draw && source != 1)
                g_backend.draw(draw);
            break;
        }
        case PM4_SET_CONSTANT:
        {
            uint32_t offsetType = r.Read();
            uint32_t index = offsetType & 0x7FF;
            switch ((offsetType >> 16) & 0xFF)
            {
            case 0: index += 0x4000; break; // ALU
            case 1: index += 0x4800; break; // fetch
            case 2: index += 0x4900; break; // bool
            case 3: index += 0x4908; break; // loop
            case 4: index += 0x2000; break; // registers
            default: index = RegisterCount; break;
            }
            for (uint32_t i = 1; i < count; i++, index++)
            {
                uint32_t data = r.Read();
                if (index < RegisterCount)
                    WriteRegister(index, data);
            }
            break;
        }
        case PM4_SET_CONSTANT2:
        case PM4_SET_SHADER_CONSTANTS:
        {
            uint32_t index = r.Read() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++, index++)
                WriteRegister(index, r.Read());
            break;
        }
        case PM4_LOAD_ALU_CONSTANT:
        {
            uint32_t address = r.Read() & 0x3FFFFFFF;
            uint32_t offsetType = r.Read();
            uint32_t size = r.Read() & 0xFFF;
            uint32_t index = offsetType & 0x7FF;
            switch ((offsetType >> 16) & 0xFF)
            {
            case 0: index += 0x4000; break;
            case 1: index += 0x4800; break;
            case 2: index += 0x4900; break;
            case 3: index += 0x4908; break;
            case 4: index += 0x2000; break;
            default: size = 0; break;
            }
            for (uint32_t i = 0; i < size; i++, index++)
                WriteRegister(index, *reinterpret_cast<be<uint32_t>*>(TranslatePhysical(address + i * 4)));
            break;
        }
        case PM4_IM_LOAD:
        {
            uint32_t addressType = r.Read();
            uint32_t startSize = r.Read();
            ShaderLoad load{ addressType & 3, addressType & ~3u, reinterpret_cast<const uint32_t*>(TranslatePhysical(addressType & ~3u)), startSize & 0xFFFF };
            DumpShader(load);
            if (g_backend.loadShader)
                g_backend.loadShader(load);
            break;
        }
        case PM4_IM_LOAD_IMMEDIATE:
        {
            uint32_t kind = r.Read();
            uint32_t startSize = r.Read();
            uint32_t size = startSize & 0xFFFF;
            // The microcode follows in the packet; it may wrap in a ring buffer, so copy it out.
            static thread_local std::vector<uint32_t> code;
            code.resize(size);
            for (uint32_t i = 0; i < size; i++)
                code[i] = __builtin_bswap32(r.Read()); // keep guest (big-endian) order like IM_LOAD
            ShaderLoad load{ kind, 0, code.data(), size };
            DumpShader(load);
            if (g_backend.loadShader)
                g_backend.loadShader(load);
            break;
        }
        case PM4_VIZ_QUERY:
        {
            uint32_t word = r.Read();
            uint32_t id = word & 0x3F;
            if (word & 0x100)
            {
                if (id < 32)
                    g_regs[REG_PA_SC_VIZ_QUERY_STATUS_0] |= 1u << id;
                else
                    g_regs[REG_PA_SC_VIZ_QUERY_STATUS_1] |= 1u << (id - 32);
            }
            break;
        }
        case PM4_SET_BIN_MASK_LO: g_binMask = (g_binMask & ~0xFFFFFFFFull) | r.Read(); break;
        case PM4_SET_BIN_MASK_HI: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case PM4_SET_BIN_SELECT_LO: g_binSelect = (g_binSelect & ~0xFFFFFFFFull) | r.Read(); break;
        case PM4_SET_BIN_SELECT_HI: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;
        case PM4_SET_BIN_MASK:
        {
            uint64_t hi = r.Read();
            g_binMask = (hi << 32) | r.Read();
            break;
        }
        case PM4_SET_BIN_SELECT:
        {
            uint64_t hi = r.Read();
            g_binSelect = (hi << 32) | r.Read();
            break;
        }
        case PM4_ME_INIT:
        case PM4_NOP:
        case PM4_INVALIDATE_STATE:
        case PM4_CONTEXT_UPDATE:
        case PM4_WAIT_FOR_IDLE:
            break;
        default:
        {
            static bool reported[128];
            if (!reported[opcode])
            {
                reported[opcode] = true;
                Log("GPU: unhandled PM4 opcode {:02X} ({} dwords)", opcode, count);
            }
            break;
        }
        }

        // Whatever a handler consumed, continue after the packet.
        r.offset = endOffset;
        r.remaining = endRemaining;
    }

    // A packet claiming more data than the buffer holds means we misparsed; stop rather than wrap.
    static bool Fits(const Reader& r, uint32_t dwords, uint32_t packet)
    {
        if (uint64_t(dwords) * 4 <= r.remaining)
            return true;
        Log("GPU: packet {:08X} needs {} dwords, only {} left", packet, dwords, r.remaining / 4);
        return false;
    }

    static void ExecuteBuffer(Reader r)
    {
        if (g_tracePackets)
        {
            std::string words;
            Reader peek = r;
            for (uint32_t i = 0; i < r.remaining / 4 && i < 96; i++)
                words += fmt::format(" {:08X}", peek.Read());
            Log("BUFFER {} dwords:{}", r.remaining / 4, words);
        }
        while (r.remaining >= 4)
        {
            uint32_t packet = r.Read();
            uint32_t type = packet >> 30;
            uint32_t needed = type == 0 ? ((packet >> 16) & 0x3FFF) + 1 : type == 1 ? 2 : type == 3 ? ((packet >> 16) & 0x3FFF) + 1 : 0;
            if (!Fits(r, needed, packet))
                return;
            if (packet == 0)
                continue;
            switch (packet >> 30)
            {
            case 0:
            {
                uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                uint32_t index = packet & 0x7FFF;
                const bool single = (packet >> 15) & 1;
                if (g_tracePackets)
                    Log("REG {:04X} x{}{}", index, count, single ? " (single)" : "");
                for (uint32_t i = 0; i < count; i++)
                    WriteRegister(single ? index : index + i, r.Read());
                break;
            }
            case 1:
            {
                uint32_t a = r.Read();
                uint32_t b = r.Read();
                WriteRegister(packet & 0x7FF, a);
                WriteRegister((packet >> 11) & 0x7FF, b);
                break;
            }
            case 2:
                break;
            case 3:
                ExecuteType3(r, packet);
                break;
            }
        }
    }

    static void CommandProcessorThread()
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); // see StartHostThread
        while (true)
        {
            // The title writes CP_RB_WPTR through the MMIO window; poll it.
            uint32_t writeIndex = *Translate<be<uint32_t>>(GpuRegisterBase + REG_CP_RB_WPTR * 4);
            if (!g_ringSize || writeIndex == g_readIndex)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            uint32_t ringDwords = g_ringSize / 4;
            writeIndex %= ringDwords;
            uint32_t available = (writeIndex + ringDwords - g_readIndex) % ringDwords;
            ExecuteBuffer({ TranslatePhysical(g_ringBase), g_ringSize, g_readIndex * 4, available * 4 });
            g_readIndex = writeIndex;
            if (g_writeBack)
                *reinterpret_cast<be<uint32_t>*>(TranslatePhysical(g_writeBack)) = g_readIndex;
        }
    }

    // Waits (at most 50 ms) until the command processor has executed the draws of every frame the title has
    // submitted: it processed their swaps, or it is blocked in the last one's end-of-frame wait for the
    // previous flip, which follows that frame's draws. Bink decodes into the buffer the previous frame drew;
    // on the console the GPU was done with it by then. Called from the write-fault handler (see
    // MarkGpuRead), so it only reads atomics and sleeps.
    static void WaitForSubmittedFrames()
    {
        const uint64_t target = g_swapsSubmitted.load(std::memory_order_acquire);
        auto done = [target] {
            const uint64_t frames = g_frames.load(std::memory_order_acquire);
            return frames >= target || (frames + 1 == target && g_cpWaiting.load(std::memory_order_relaxed));
        };
        WaitScope waiting;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!done() && !g_gpuAwaitingWriters.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Hang watchdog: when no frame has been presented for 10 s (the clock stops while the Mac sleeps), saves
    // every thread's stack once, with macOS's sample tool, to ~/Library/Logs/Peggle 2/hang-<time>.txt.
    static void WatchdogThread()
    {
        uint64_t lastFrames = g_frames;
        auto lastProgress = std::chrono::steady_clock::now();
        bool reported = false;
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = std::chrono::steady_clock::now();
            if (const uint64_t frames = g_frames; frames != lastFrames || frames == 0)
            {
                lastFrames = frames;
                lastProgress = now;
                reported = false;
                continue;
            }
            if (reported || now - lastProgress < std::chrono::seconds(10))
                continue;
            reported = true;
            const char* home = getenv("HOME");
            if (!home)
                continue;
            const std::filesystem::path dir = std::filesystem::path(home) / "Library/Logs/Peggle 2";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            const std::string file = (dir / fmt::format("hang-{}.txt", std::time(nullptr))).string();
            Log("Watchdog: no frame for 10 s (frame {}); saving thread stacks to {}", lastFrames, file);
            const std::string pid = std::to_string(getpid());
            const char* argv[] = { "/usr/bin/sample", pid.c_str(), "2", "-file", file.c_str(), nullptr };
            pid_t child;
            if (posix_spawn(&child, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
                waitpid(child, nullptr, 0);
        }
    }

    static void StartThreads()
    {
        static bool started = false;
        if (started)
            return;
        started = true;

        // Registers with fixed values the title reads (as Xenia reports them).
        auto seed = [](uint32_t index, uint32_t value) { g_regs[index] = value; MirrorRegister(index); };
        seed(0x0F00, 0x08100748); // RB_EDRAM_TIMING
        seed(0x0F01, 0x0000200E); // RB_BC_CONTROL
        seed(0x194C, 0x000002D0); // D1MODE_V_COUNTER
        seed(0x1951, 0x00000001); // interrupt status: vblank
        seed(0x1961, 0x050002D0); // D1MODE_VIEWPORT_SIZE

        SetGpuReadWait(WaitForSubmittedFrames);
        std::thread(CommandProcessorThread).detach();
        if (getenv("PEGGLE2_GPU_STATUS"))
        {
            std::thread([] {
                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    uint32_t raw = *Translate<uint32_t>(GpuRegisterBase + REG_CP_RB_WPTR * 4);
                    Log("GPU status: read {:X} write(raw) {:08X} frames {}", g_readIndex, raw, uint64_t(g_frames));
                }
            }).detach();
        }
        CreateHostThread("GPU interrupts", InterruptThread);
        std::thread(VblankThread).detach();
        std::thread(WatchdogThread).detach();
    }

    void NoteSwapSubmitted()
    {
        g_swapsSubmitted.fetch_add(1, std::memory_order_release);
    }

    void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2)
    {
        g_ringBase = physical & 0x1FFFFFFF;
        g_ringSize = 1u << (sizeLog2 + 3);
        g_readIndex = 0;
        Log("GPU: ring buffer at {:X}, {:X} bytes", g_ringBase, g_ringSize);
        StartThreads();
    }

    void EnableReadPointerWriteBack(uint32_t physical, uint32_t)
    {
        g_writeBack = physical & 0x1FFFFFFF;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_interruptUserData = userData;
        g_interruptCallback = callback;
        Log("GPU: interrupt callback {:X} ({:X})", callback, userData);
    }
}
