// Force-included into the recompiled code (see CMakeLists.txt) ahead of ppc_context.h.
#pragma once
#include <cstdint>

// The 360's VMX unit flushes denormals to zero and its FPU does not; the recompiled code would switch the
// host's FPCR between them, and on Apple silicon those writes serialise the core: in busy scenes (Berg's
// power) the game ran at 20 fps at 60 fps settings. One IEEE mode for both keeps the FPU (scalar, where
// the game's physics live) exact; only VMX results that would be denormal (below about 1e-38) differ.
#define PPC_NO_FLUSH_MODE_SWITCH

struct PPCContext;
[[noreturn]] void ReportMissingIndirectCall(PPCContext& ctx, uint32_t target);

// Indirect calls through an empty table slot report the guest target instead of jumping to null.
#define PPC_CALL_INDIRECT_FUNC(x)                                         \
    do                                                                    \
    {                                                                     \
        uint32_t target_ = uint32_t(x);                                   \
        PPCFunc* func_ = (target_ >= PPC_CODE_BASE && target_ < PPC_CODE_BASE + PPC_CODE_SIZE) \
            ? PPC_LOOKUP_FUNC(base, target_) : nullptr;                   \
        if (__builtin_expect(func_ == nullptr, 0))                        \
            ReportMissingIndirectCall(ctx, target_);                      \
        func_(ctx, base);                                                 \
    } while (0)

namespace rt::xma { void MmioWrite(uint32_t address, uint32_t stored); }

// Stores followed by eieio are MMIO. The XMA register block needs to see them; the GPU's registers are
// polled from memory, so everything else is stored as usual.
#define PPC_MM_STORE_U32(x, y)                                            \
    do                                                                    \
    {                                                                     \
        uint32_t mmioAddress_ = uint32_t(x);                              \
        uint32_t mmioValue_ = uint32_t(y);                                \
        if ((mmioAddress_ & 0xFFFF0000) == 0x7FEA0000)                    \
            rt::xma::MmioWrite(mmioAddress_, mmioValue_);                 \
        else                                                              \
            PPC_STORE_U32(mmioAddress_, mmioValue_);                      \
    } while (0)
