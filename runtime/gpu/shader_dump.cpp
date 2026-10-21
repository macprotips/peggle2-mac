// PEGGLE2_DUMP_SHADERS=dir: saves each distinct shader the title loads as raw microcode, named like the
// renderer's log lines (vs_/ps_ and the shader's hash), for tools/xenos_disasm.
#include "gpu.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace rt::gpu
{
    void DumpShader(const ShaderLoad& load)
    {
        static const char* dir = getenv("PEGGLE2_DUMP_SHADERS");
        if (!dir)
            return;
        const uint64_t hash = XXH3_64bits(load.dwords, load.count * 4) + load.kind; // as renderer.mm's LoadShader
        static std::mutex mutex;
        static std::set<uint64_t> seen;
        std::lock_guard lock(mutex);
        if (!seen.insert(hash).second)
            return;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%016llX.bin", dir, load.kind ? "ps" : "vs", (unsigned long long)hash);
        if (FILE* f = fopen(path, "wb"))
        {
            fwrite(load.dwords, 4, load.count, f);
            fclose(f);
        }
    }
}
