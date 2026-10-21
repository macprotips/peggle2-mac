// Logging to stderr (and the log file), fatal errors, import tracing and the crash handler.
#pragma once
#include <fmt/core.h>

namespace rt
{
    void LogLine(const std::string& line);
    [[noreturn]] void FatalLine(const std::string& line);
    // Called (once) with the message before a fatal error ends the app; the app shows it to the player.
    void SetFatalHook(void (*hook)(const std::string& message));

    // PEGGLE2_TRACE=1 logs every implemented import call except the per-frame lock and TLS traffic;
    // PEGGLE2_TRACE=all logs everything.
    extern bool g_traceImports;
    bool TraceImport(const char* name);

    // Prints the fault address (as a guest address when it is inside guest memory) and the guest call
    // stack on SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT, then re-raises so macOS writes a crash report. (Write
    // watches' faults are handled here first; see HandleWriteFault.)
    void InstallCrashHandler();

    template<typename... Args>
    void Log(fmt::format_string<Args...> format, Args&&... args)
    {
        LogLine(fmt::format(format, std::forward<Args>(args)...));
    }

    template<typename... Args>
    [[noreturn]] void Fatal(fmt::format_string<Args...> format, Args&&... args)
    {
        FatalLine(fmt::format(format, std::forward<Args>(args)...));
    }
}
