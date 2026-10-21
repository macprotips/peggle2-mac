#pragma once
#include <fmt/core.h>

namespace rt
{
    void LogLine(const std::string& line);
    [[noreturn]] void FatalLine(const std::string& line);
    // Called (once) with the message before a fatal error ends the app; the app shows it to the player.
    void SetFatalHook(void (*hook)(const std::string& message));

    // Set PEGGLE2_TRACE=1 to log every implemented import call.
    extern bool g_traceImports;
    // PEGGLE2_TRACE=1 traces every import except the per-frame lock traffic; PEGGLE2_TRACE=all
    // traces everything.
    bool TraceImport(const char* name);

    // Prints the fault address (as a guest address when it is inside guest memory) and the guest call
    // stack on SIGSEGV/SIGBUS/SIGILL/SIGABRT, then exits.
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
