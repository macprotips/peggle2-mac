#include "../guest.h"
#include <mutex>
#include <map>
#include <set>
#include <string>

namespace rt
{
    void UnimplementedImport(const char* name, PPCContext& ctx)
    {
        static std::mutex mutex;
        static std::set<std::string> reported;
        {
            std::lock_guard lock(mutex);
            static std::map<std::string, int> counts;
            if (getenv("PEGGLE2_COUNT_UNIMPLEMENTED") && ++counts[name] % 50 == 0)
                Log("UNIMPLEMENTED import {} called {} times", name, counts[name]);
            if (reported.insert(name).second)
                Log("UNIMPLEMENTED import {}(r3={:X}, r4={:X}, r5={:X}) lr={:X}", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
        }
        ctx.r3.u64 = 0;
    }
}
