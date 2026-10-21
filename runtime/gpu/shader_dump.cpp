// Temporary: dump unique shader microcode the title uploads (PEGGLE2_DUMP_SHADERS=dir).
#include "gpu.h"
#include "../log.h"
#include <cstdio>
#include <mutex>
#include <set>

namespace rt::gpu
{
    void DumpShader(const ShaderLoad& load)
    {
        static const char* dir = getenv("PEGGLE2_DUMP_SHADERS");
        if (!dir)
            return;
        static std::mutex mutex;
        static std::set<uint64_t> seen;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < load.count; i++)
            hash = (hash ^ load.dwords[i]) * 1099511628211ull;
        std::lock_guard lock(mutex);
        if (!seen.insert(hash ^ load.kind).second)
            return;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%016llx.bin", dir, load.kind ? "ps" : "vs", (unsigned long long)hash);
        if (FILE* f = fopen(path, "wb"))
        {
            fwrite(load.dwords, 4, load.count, f);
            fclose(f);
        }
    }
}
