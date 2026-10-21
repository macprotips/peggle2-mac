#include "../thread.h"
#include "objects.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <mutex>

namespace rt
{
    // Timers are dispatcher objects; a service thread signals them at their due time and queues their
    // completion routine (if any) as an APC to the thread that set them.
    struct Timer : HostObject
    {
        uint32_t object = 0;
        uint64_t generation = 0; // bumped by set/cancel so stale schedule entries are ignored
        std::chrono::milliseconds period{ 0 };
        uint32_t routine = 0;
        uint32_t argument = 0;
        GuestThread* owner = nullptr;
    };

    using Clock = std::chrono::steady_clock;

    struct Scheduled
    {
        std::shared_ptr<Timer> timer;
        uint64_t generation;
    };

    static std::mutex g_timerMutex;
    static std::condition_variable g_timerCv;
    static std::multimap<Clock::time_point, Scheduled> g_schedule;
    static bool g_serviceStarted;

    static void TimerService()
    {
        std::unique_lock lock(g_timerMutex);
        while (true)
        {
            if (g_schedule.empty())
            {
                g_timerCv.wait(lock);
                continue;
            }
            auto next = g_schedule.begin();
            if (Clock::now() < next->first)
            {
                g_timerCv.wait_until(lock, next->first);
                continue;
            }

            auto [due, entry] = *next;
            g_schedule.erase(next);
            Timer& timer = *entry.timer;
            if (entry.generation != timer.generation)
                continue;

            SignalObject(timer.object, 1);
            if (timer.routine && timer.owner)
            {
                // PTIMERAPCROUTINE(argument, timerLowValue, timerHighValue)
                uint64_t now = GuestSystemTime();
                QueueApc(timer.owner, timer.routine, timer.argument, uint32_t(now), uint32_t(now >> 32));
            }
            if (timer.period.count() > 0)
                g_schedule.emplace(due + timer.period, entry);
        }
    }

    static uint32_t NtCreateTimer(be<uint32_t>* handle, uint32_t, uint32_t timerType)
    {
        auto timer = std::make_shared<Timer>();
        timer->object = CreateDispatcherObject(timerType ? TypeSynchronizationTimer : TypeNotificationTimer, 0x28, 0);
        *handle = CreateHandle(timer->object, timer);

        std::lock_guard lock(g_timerMutex);
        if (!g_serviceStarted)
        {
            g_serviceStarted = true;
            std::thread(TimerService).detach();
        }
        return STATUS_SUCCESS;
    }

    static uint32_t NtSetTimerEx(uint32_t handle, be<int64_t>* dueTime, uint32_t routine, uint32_t, uint32_t argument,
        uint32_t resume, int32_t periodMs, be<uint32_t>* previousState)
    {
        auto timer = HandleTo<Timer>(handle);
        if (!timer)
            return STATUS_INVALID_HANDLE;

        int64_t relative100ns = RelativeTimeout100ns(*dueTime);
        auto when = Clock::now() + std::chrono::nanoseconds(relative100ns * 100);

        if (previousState)
            *previousState = Translate<DispatcherHeader>(timer->object)->signalState > 0 ? 1 : 0;
        SignalObject(timer->object, 0); // setting a timer resets it

        std::lock_guard lock(g_timerMutex);
        timer->generation++;
        timer->period = std::chrono::milliseconds(periodMs > 0 ? periodMs : 0);
        timer->routine = routine;
        timer->argument = argument;
        timer->owner = CurrentThread();
        g_schedule.emplace(when, Scheduled{ timer, timer->generation });
        g_timerCv.notify_all();

        // Wake-from-standby is meaningless here; NT reports that but still sets the timer.
        constexpr uint32_t STATUS_TIMER_RESUME_IGNORED = 0x40000025;
        return resume ? STATUS_TIMER_RESUME_IGNORED : STATUS_SUCCESS;
    }

    static uint32_t NtCancelTimer(uint32_t handle, be<uint32_t>* currentState)
    {
        auto timer = HandleTo<Timer>(handle);
        if (!timer)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_timerMutex);
        timer->generation++;
        if (currentState)
            *currentState = Translate<DispatcherHeader>(timer->object)->signalState > 0 ? 1 : 0;
        return STATUS_SUCCESS;
    }
}

using namespace rt;
GUEST_IMPORT(NtCreateTimer, NtCreateTimer)
GUEST_IMPORT(NtSetTimerEx, NtSetTimerEx)
GUEST_IMPORT(NtCancelTimer, NtCancelTimer)
