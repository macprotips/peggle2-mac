// Stand-in for Xenia's GPU trace writer (tracing is not supported).
#pragma once
#include <cstdint>
namespace xe::gpu
{
    class TraceWriter
    {
    public:
        void WriteMemoryRead(uint32_t, uint32_t) {}
        void WriteMemoryWrite(uint32_t, uint32_t) {}
    };
}
