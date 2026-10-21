// Deferred procedure calls: queued by interrupt routines, run soon after on a DPC thread.
#include "../thread.h"
#include "xbox_types.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace rt
{
    // XDPC as Xenia lays it out: type/importance/number, list entry, routine, context, arguments.
    struct Dpc
    {
        be<uint32_t> header;
        be<uint32_t> flink;
        be<uint32_t> blink;
        be<uint32_t> routine;
        be<uint32_t> context;
        be<uint32_t> argument1;
        be<uint32_t> argument2;
    };

    static std::mutex g_dpcMutex;
    static std::condition_variable g_dpcCv;
    static std::deque<uint32_t> g_dpcQueue;
    static bool g_dpcThreadStarted;

    static void DpcThread()
    {
        while (true)
        {
            uint32_t address;
            {
                std::unique_lock lock(g_dpcMutex);
                g_dpcCv.wait(lock, [] { return !g_dpcQueue.empty(); });
                address = g_dpcQueue.front();
                g_dpcQueue.pop_front();
                Translate<Dpc>(address)->flink = 0; // no longer queued
            }
            Dpc* dpc = Translate<Dpc>(address);
            GuestCall(dpc->routine, { address, dpc->context, dpc->argument1, dpc->argument2 });
        }
    }

    static void KeInitializeDpc(Dpc* dpc, uint32_t routine, uint32_t context)
    {
        constexpr uint32_t DpcObject = 19;
        dpc->header = DpcObject << 24;
        dpc->flink = 0;
        dpc->blink = 0;
        dpc->routine = routine;
        dpc->context = context;
        dpc->argument1 = 0;
        dpc->argument2 = 0;
    }

    static uint32_t KeInsertQueueDpc(Dpc* dpc, uint32_t argument1, uint32_t argument2)
    {
        std::lock_guard lock(g_dpcMutex);
        if (dpc->flink)
            return 0; // already queued
        if (!g_dpcThreadStarted)
        {
            g_dpcThreadStarted = true;
            CreateHostThread("DPC", DpcThread);
        }
        dpc->argument1 = argument1;
        dpc->argument2 = argument2;
        uint32_t address = ToGuest(dpc);
        dpc->flink = address; // non-zero marks it queued
        g_dpcQueue.push_back(address);
        g_dpcCv.notify_one();
        return 1;
    }
}

using namespace rt;
GUEST_IMPORT(KeInitializeDpc, KeInitializeDpc)
GUEST_IMPORT(KeInsertQueueDpc, KeInsertQueueDpc)
