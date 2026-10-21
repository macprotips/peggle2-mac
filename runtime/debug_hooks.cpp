// Debugging aid: override a recompiled function to log its result, eg.
//   LOG_RESULT(82520F08)
// Recompiled functions are weak, so a definition here replaces sub_<address> and can still call the
// original through __imp__sub_<address>. Keep this file free of hooks when committing.
#include "guest.h"

#define LOG_RESULT(addr)                                                  \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                \
    PPC_FUNC(sub_##addr)                                                   \
    {                                                                      \
        __imp__sub_##addr(ctx, base);                                      \
        ::rt::Log("sub_" #addr " returned {:X}", ctx.r3.u32);              \
    }
