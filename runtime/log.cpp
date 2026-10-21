// Logging, import tracing, fatal errors and the crash handler.
#include "log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "memory.h"

namespace rt
{
    bool g_traceImports = getenv("PEGGLE2_TRACE") != nullptr;

    static std::mutex g_logMutex;

    bool TraceImport(const char* name)
    {
        static const bool all = strcmp(getenv("PEGGLE2_TRACE"), "all") == 0;
        return all || (strcmp(name, "RtlEnterCriticalSection") != 0 && strcmp(name, "RtlLeaveCriticalSection") != 0 &&
                       strcmp(name, "KeTlsGetValue") != 0);
    }

    void LogLine(const std::string& line)
    {
        uint64_t tid;
        pthread_threadid_np(nullptr, &tid);
        std::lock_guard lock(g_logMutex);
        fprintf(stderr, "[%5llu] %s\n", (unsigned long long)tid, line.c_str());
    }

    // Recompiled functions are named sub_<guest address>, so the host stack reads as the guest stack.
    void MaybeLogBacktrace(const char* importName)
    {
        static const char* wanted = getenv("PEGGLE2_BACKTRACE");
        const bool always = strcmp(importName, "CriticalSectionWait") == 0;
        if (!always && (!wanted || strcmp(wanted, importName) != 0))
            return;
        void* frames[48];
        int count = backtrace(frames, 48);
        char** symbols = backtrace_symbols(frames, count);
        std::string stack = std::string("  stack of ") + importName + ":";
        for (int i = 2; i < count; i++)
        {
            const char* sub = strstr(symbols[i], "sub_");
            if (sub)
                stack += " " + std::string(sub, strcspn(sub, " +"));
        }
        free(symbols);
        LogLine(stack);
    }

    static void CrashHandler(int signal, siginfo_t* info, void*)
    {
        // A write to a watched page is not a crash: unprotect it and let the write retry.
        if ((signal == SIGSEGV || signal == SIGBUS) && HandleWriteFault(info->si_addr))
            return;
        // Only async-signal-tolerant work here: format into a stack buffer and write(2).
        char line[256];
        uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
        uintptr_t guestBase = reinterpret_cast<uintptr_t>(g_base);
        int n;
        if (fault >= guestBase && fault < guestBase + 0x100000000ull)
            n = snprintf(line, sizeof(line), "\nCRASH: signal %d at guest address %08lX\n", signal, (unsigned long)(fault - guestBase));
        else
            n = snprintf(line, sizeof(line), "\nCRASH: signal %d at host address %p\n", signal, info->si_addr);
        write(STDERR_FILENO, line, size_t(n));
        void* frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        // Then crash for real, so macOS records a crash report (Console > Crash Reports).
        struct sigaction fallback{};
        fallback.sa_handler = SIG_DFL;
        sigaction(signal, &fallback, nullptr);
        raise(signal);
        _exit(128 + signal);
    }

    void InstallCrashHandler()
    {
        struct sigaction action{};
        action.sa_sigaction = CrashHandler;
        action.sa_flags = SA_SIGINFO;
        for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
            sigaction(signal, &action, nullptr);
    }

    static void (*g_fatalHook)(const std::string&);

    void SetFatalHook(void (*hook)(const std::string& message))
    {
        g_fatalHook = hook;
    }

    void FatalLine(const std::string& line)
    {
        LogLine("FATAL: " + line);
        static std::atomic<bool> reporting;
        if (g_fatalHook && !reporting.exchange(true))
            g_fatalHook(line);
        abort();
    }
}
