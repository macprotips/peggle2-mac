// Video (Vd*) kernel imports. Semantics follow Xenia's xboxkrnl_video.cc (BSD-3-Clause).
#include "../gpu/gpu.h"
#include "../guest.h"
#include "xbox_types.h"
#include <cstring>

namespace rt
{
    struct GuestVideoMode
    {
        be<uint32_t> displayWidth;
        be<uint32_t> displayHeight;
        be<uint32_t> isInterlaced;
        be<uint32_t> isWidescreen;
        be<uint32_t> isHiDef;
        be<float> refreshRate;
        be<uint32_t> videoStandard;
        be<uint32_t> unknown0x8A;
        be<uint32_t> unknown0x01;
        be<uint32_t> reserved[3];
    };

    static void VdQueryVideoMode(GuestVideoMode* mode)
    {
        memset(mode, 0, sizeof(*mode));
        mode->displayWidth = 1280;
        mode->displayHeight = 720;
        mode->isWidescreen = 1;
        mode->isHiDef = 1;
        mode->refreshRate = 60.0f;
        mode->videoStandard = 1; // NTSC
        mode->unknown0x8A = 0x4A;
        mode->unknown0x01 = 0x01;
    }

    static uint32_t VdQueryVideoFlags()
    {
        return 1 | 2; // widescreen, at least 1024 wide
    }

    static uint32_t VdSetDisplayMode(uint32_t) { return 0; }
    static uint32_t VdInitializeEngines(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 1; }
    static void VdShutdownEngines() {}
    static uint32_t VdEnableDisableClockGating(uint32_t) { return 0; }
    static uint32_t VdIsHSIOTrainingSucceeded() { return 1; }
    static uint32_t VdRetrainEDRAMWorker(uint32_t) { return 0; }
    static uint32_t VdRetrainEDRAM(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
    static uint32_t VdCallGraphicsNotificationRoutines(uint32_t, uint32_t) { return 0; }
    static void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t) {}

    static void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
    {
        *type = 2;
        *power = 2.22222233f;
    }

    static void VdGetCurrentDisplayInformation(uint8_t* info)
    {
        memset(info, 0, 0x58);
        auto put16 = [&](uint32_t offset, uint16_t v) { *reinterpret_cast<be<uint16_t>*>(info + offset) = v; };
        auto put32 = [&](uint32_t offset, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(info + offset) = v; };
        put16(0x00, 1280);
        put16(0x02, 720);
        put32(0x08 + 0x08, 1280);  // scaler source rect x2
        put32(0x08 + 0x0C, 720);   // scaler source rect y2
        put32(0x08 + 0x10, 1280);  // scaled output width
        put32(0x08 + 0x14, 720);   // scaled output height
        put32(0x08 + 0x18, 1);     // vertical filter type
        put32(0x08 + 0x28, 1);     // horizontal filter type
        put16(0x40, 320);
        put16(0x42, 180);
        put16(0x44, 320);
        put16(0x46, 180);
        put16(0x48, 1280);
        put16(0x4A, 720);
        *reinterpret_cast<be<float>*>(info + 0x4C) = 60.0f;
        put16(0x56, 1280);
    }

    static uint32_t VdPersistDisplay(uint32_t, be<uint32_t>* out)
    {
        if (out)
            *out = g_systemHeap.Alloc(64);
        return 1;
    }

    static void VdGetSystemCommandBuffer(uint8_t* p0, be<uint32_t>* p1)
    {
        memset(p0, 0, 0x94);
        *reinterpret_cast<be<uint32_t>*>(p0) = 0xBEEF0000;
        *p1 = 0xBEEF0001;
    }

    PPC_FUNC(VdInitializeScalerCommandBufferImpl)
    {
        // 12 arguments: the last two (destination, count in words) are on the stack.
        uint32_t destination = uint32_t(IntArg(ctx, 10, 4));
        uint32_t count = uint32_t(IntArg(ctx, 11, 4));
        auto* words = Translate<be<uint32_t>>(destination);
        for (uint32_t i = 0; i < count; i++)
            words[i] = 0x80000000; // type-2 (no-op) packets
        ctx.r3.u64 = count;
    }

    static void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
    {
        gpu::SetInterruptCallback(callback, userData);
    }

    static void VdInitializeRingBuffer(uint32_t physical, uint32_t sizeLog2)
    {
        gpu::InitializeRingBuffer(physical, sizeLog2);
    }

    static void VdEnableRingBufferRPtrWriteBack(uint32_t physical, uint32_t blockSizeLog2)
    {
        gpu::EnableReadPointerWriteBack(physical, blockSizeLog2);
    }

    static uint32_t VdSetDisplayModeOverride(uint32_t, uint32_t, double, uint32_t, uint32_t) { return 0; }

    // VdSwap reserves 64 dwords in the ring; they are filled with the front buffer's fetch constant and a
    // swap packet the command processor understands, then no-ops.
    PPC_FUNC(VdSwapImpl)
    {
        RearmGpuReads(); // the frame's draws are about to be kicked
        gpu::NoteSwapSubmitted();
        // (buffer, fetch, writeback, systemBuffer, systemBuffer2, frontbuffer*, format*, colorSpace*, width*, height*)
        auto* buffer = Translate<be<uint32_t>>(ctx.r3.u32);
        auto* fetch = Translate<be<uint32_t>>(ctx.r4.u32);
        uint32_t width = *Translate<be<uint32_t>>(uint32_t(IntArg(ctx, 8, 4)));
        uint32_t height = *Translate<be<uint32_t>>(uint32_t(IntArg(ctx, 9, 4)));

        uint32_t words[6];
        for (int i = 0; i < 6; i++)
            words[i] = fetch[i];
        // The D3D texture header holds a CPU address; the GPU needs the physical one.
        uint32_t frontbufferVirtual = (words[1] >> 12) << 12;
        uint32_t frontbufferPhysical = frontbufferVirtual & 0x1FFFFFFF;
        words[1] = (words[1] & 0xFFF) | frontbufferPhysical;

        memset(buffer, 0, 64 * 4);
        uint32_t n = 0;
        buffer[n++] = (0u << 30) | ((6 - 1) << 16) | gpu::REG_SHADER_CONSTANT_FETCH_00_0;
        for (int i = 0; i < 6; i++)
            buffer[n++] = words[i];
        buffer[n++] = (3u << 30) | ((4 - 1) << 16) | (0x64 << 8); // PM4_XE_SWAP
        buffer[n++] = 0x53574150;                                // 'SWAP'
        buffer[n++] = frontbufferPhysical;
        buffer[n++] = width;
        buffer[n++] = height;
        while (n < 64)
            buffer[n++] = 0x80000000;
    }
}

using namespace rt;
GUEST_IMPORT(VdQueryVideoMode, VdQueryVideoMode)
GUEST_IMPORT(VdQueryVideoFlags, VdQueryVideoFlags)
GUEST_IMPORT(VdSetDisplayMode, VdSetDisplayMode)
GUEST_IMPORT(VdInitializeEngines, VdInitializeEngines)
GUEST_IMPORT(VdShutdownEngines, VdShutdownEngines)
GUEST_IMPORT(VdEnableDisableClockGating, VdEnableDisableClockGating)
GUEST_IMPORT(VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded)
GUEST_IMPORT(VdRetrainEDRAMWorker, VdRetrainEDRAMWorker)
GUEST_IMPORT(VdRetrainEDRAM, VdRetrainEDRAM)
GUEST_IMPORT(VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines)
GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress)
GUEST_IMPORT(VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma)
GUEST_IMPORT(VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation)
GUEST_IMPORT(VdPersistDisplay, VdPersistDisplay)
GUEST_IMPORT(VdGetSystemCommandBuffer, VdGetSystemCommandBuffer)
GUEST_IMPORT(VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback)
GUEST_IMPORT(VdInitializeRingBuffer, VdInitializeRingBuffer)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack)
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) { rt::VdInitializeScalerCommandBufferImpl(ctx, base); }
PPC_FUNC(__imp__VdSwap) { rt::VdSwapImpl(ctx, base); }
GUEST_IMPORT(VdSetDisplayModeOverride, rt::VdSetDisplayModeOverride)
