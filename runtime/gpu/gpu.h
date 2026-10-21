// Emulated Xenos GPU front end: registers, command processor, interrupts.
// Packet and register semantics follow Xenia (BSD-3-Clause, see THIRD_PARTY_NOTICES.md).
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include "xenia/gpu/register_file.h"

namespace rt::gpu
{
    constexpr uint32_t RegisterCount = xe::gpu::RegisterFile::kRegisterCount;

    // Register indices used by the front end.
    enum : uint32_t
    {
        REG_CP_RB_WPTR = 0x01C5,
        REG_SCRATCH_UMSK = 0x01DC,
        REG_SCRATCH_ADDR = 0x01DD,
        REG_SCRATCH_REG0 = 0x0578,
        REG_SCRATCH_REG7 = 0x057F,
        REG_COHER_STATUS_HOST = 0x0A31,
        REG_PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44,
        REG_PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45,
        REG_VGT_EVENT_INITIATOR = 0x21F9,
        REG_VGT_DMA_BASE = 0x21FA,
        REG_VGT_DMA_SIZE = 0x21FB,
        REG_VGT_DRAW_INITIATOR = 0x21FC,
        REG_SHADER_CONSTANT_FETCH_00_0 = 0x4800,
    };

    void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);
    // VdSwap: the title submitted a frame.
    void NoteSwapSubmitted();

    // Registers as the command processor sees them (host order).
    uint32_t ReadRegister(uint32_t index);
    const xe::gpu::RegisterFile& Registers();

    // Guest pointer for a GPU (physical) address.
    uint8_t* TranslatePhysical(uint32_t physical);

    // Shader microcode loaded by IM_LOAD or IM_LOAD_IMMEDIATE: kind 0 = vertex, 1 = pixel.
    struct ShaderLoad
    {
        uint32_t kind;
        const uint32_t* dwords; // big-endian
        uint32_t count;
    };
    // Time the command processor spent this frame waiting for streaming textures to settle (textures.mm;
    // PEGGLE2_PERF reports it).
    extern std::chrono::steady_clock::duration g_settleWait;

    // PEGGLE2_DUMP_SHADERS=dir: saves each new shader's microcode there (shader_dump.cpp).
    void DumpShader(const ShaderLoad& load);

    struct DrawRequest
    {
        uint32_t primitiveType;
        uint32_t indexCount;
        bool indexed;
        uint32_t indexBase;    // physical
        uint32_t indexFormat;  // 0 = 16-bit, 1 = 32-bit
        uint32_t indexEndian;
    };

    // Backend hooks, called on the command processor thread.
    struct Backend
    {
        std::function<void(const ShaderLoad&)> loadShader;
        std::function<void(const DrawRequest&)> draw;
        std::function<void(uint32_t frontbuffer, uint32_t width, uint32_t height)> swap;
        // Finish submitted work before the guest can observe GPU progress.
        std::function<void()> sync;
    };
    void SetBackend(Backend backend);

    uint64_t FrameCount();

    // GPU progress the title can observe: a count that changes with every such write, and a wait (at most
    // `timeout`) for it to differ from `seen`.
    uint32_t Progress();
    void WaitForProgress(uint32_t seen, std::chrono::microseconds timeout);
}
