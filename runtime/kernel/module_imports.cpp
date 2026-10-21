// Module and loader imports, and the kernel's exported variables.
#include "module.h"
#include "../guest.h"
#include "xbox_types.h"
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <string_view>
#include <thread>
#include <xex.h>

namespace rt
{
    static uint32_t g_xexHeader;   // guest copy of the XEX header
    static uint32_t g_ldrEntry;    // X_LDR_DATA_TABLE_ENTRY for the executable

    // KeTimeStampBundle: interrupt time and system time in 100 ns units, then the tick count in ms.
    // The kernel refreshes it every millisecond; GetTickCount and the RAD timers read it directly.
    static void TimeStampThread(uint32_t bundle)
    {
        pthread_setname_np("Kernel clock");
        auto* p = Translate<be<uint64_t>>(bundle);
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        while (true)
        {
            auto elapsed = std::chrono::steady_clock::now() - start;
            p[0] = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
            p[1] = GuestSystemTime();
            *reinterpret_cast<be<uint32_t>*>(p + 2) = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
        }
    }

    // Kernel data the title imports by ordinal. Each import slot receives the variable's address.
    static uint32_t KernelVariable(uint32_t ordinal)
    {
        auto alloc = [](uint32_t size) {
            uint32_t address = g_systemHeap.Alloc(size);
            memset(Translate(address), 0, size);
            return address;
        };
        auto store32 = [](uint32_t address, uint32_t value) { *Translate<be<uint32_t>>(address) = value; };
        switch (ordinal)
        {
        case 0x1B: // ExThreadObjectType: only its address is compared
        case 0x59: // KeDebugMonitorData: no debugger
        case 0x266: // KeCertMonitorData
        case 0x1BE: // VdGlobalDevice
        case 0x1BF: // VdGlobalXamDevice
            return alloc(0x100);
        case 0xAD: // KeTimeStampBundle
        {
            uint32_t bundle = alloc(0x18);
            std::thread(TimeStampThread, bundle).detach();
            return bundle;
        }
        case 0x156: // XboxHardwareInfo: flags, then the number of hardware threads
        {
            uint32_t info = alloc(0x10);
            store32(info, 0x00000020);
            *Translate<uint8_t>(info + 4) = 6;
            return info;
        }
        case 0x158: // XboxKrnlVersion: 2.0.17559.0, the final dashboard kernel
        {
            uint32_t version = alloc(8);
            auto* v = Translate<be<uint16_t>>(version);
            v[0] = 2;
            v[1] = 0;
            v[2] = 17559;
            v[3] = 0;
            return version;
        }
        case 0x193: // XexExecutableModuleHandle
        {
            uint32_t handle = alloc(4);
            store32(handle, g_ldrEntry);
            return handle;
        }
        case 0x1AE: // ExLoadedCommandLine
        {
            uint32_t line = alloc(0x400);
            strcpy(Translate<char>(line), "\"default.xex\"");
            return line;
        }
        case 0x1C0: // VdGpuClockInMHz
        {
            uint32_t clock = alloc(4);
            store32(clock, 500);
            return clock;
        }
        case 0x1C1: // VdHSIOCalibrationLock: an initialized, unowned critical section
        {
            uint32_t cs = alloc(0x1C);
            *Translate<uint8_t>(cs) = TypeSynchronizationEvent; // header.type
            store32(cs + 0x10, 0xFFFFFFFF);
            return cs;
        }
        default:
            return 0;
        }
    }

    // XenonUtils resolves function thunks; variable slots (record type 0) are bound here. It already
    // swapped each slot to host order, so the slot holds (type << 24) | (library << 16) | ordinal.
    static void BindVariableImports(const std::vector<uint8_t>& xexFile)
    {
        auto* imports = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xexFile.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (!imports)
            return;
        std::vector<std::string_view> names;
        const char* strings = reinterpret_cast<const char*>(imports + 1);
        for (uint32_t offset = 0; offset < imports->sizeOfStringTable && names.size() < imports->numImports;)
        {
            names.emplace_back(strings + offset);
            offset += (uint32_t(names.back().size()) + 1 + 3) & ~3u;
        }
        auto* library = reinterpret_cast<const Xex2ImportLibrary*>(strings + imports->sizeOfStringTable);
        // Function imports also have a data slot (record type 0) the title never reads; like Xenia, those
        // stay unbound. Counted rather than logged one by one.
        uint32_t unbound = 0;
        for (uint32_t i = 0; i < imports->numImports; i++)
        {
            auto* records = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
            const bool kernel = library->name < names.size() && names[library->name] == "xboxkrnl.exe";
            for (uint32_t r = 0; kernel && r < library->numberOfImports; r++)
            {
                uint32_t slot = records[r].firstThunk;
                uint32_t value;
                memcpy(&value, Translate(slot), 4);
                if (value >> 24)
                    continue; // a function thunk
                uint32_t variable = KernelVariable(value & 0xFFFF);
                if (!variable)
                {
                    unbound++;
                    continue;
                }
                *Translate<be<uint32_t>>(slot) = variable;
            }
            library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(library) + library->size);
        }
        Log("Kernel data imports: {} not bound (function slots and unused variables)", unbound);
    }

    void InitExecutableModule(const std::vector<uint8_t>& xexFile, uint32_t entryPoint)
    {
        auto* header = reinterpret_cast<const Xex2Header*>(xexFile.data());
        uint32_t headerSize = header->headerSize;
        g_xexHeader = g_systemHeap.Alloc(headerSize);
        memcpy(Translate(g_xexHeader), xexFile.data(), headerSize);

        g_ldrEntry = g_systemHeap.Alloc(0x80);
        uint8_t* ldr = Translate(g_ldrEntry);
        auto store32 = [&](uint32_t offset, uint32_t value) { *reinterpret_cast<be<uint32_t>*>(ldr + offset) = value; };
        // Single-module lists point back at the entry itself.
        for (uint32_t offset : { 0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u })
            store32(offset, g_ldrEntry + (offset & ~4u));
        store32(0x18, PPC_IMAGE_BASE);
        store32(0x1C, PPC_IMAGE_BASE);
        store32(0x20, PPC_IMAGE_SIZE);
        store32(0x38, PPC_IMAGE_SIZE);
        store32(0x3C, entryPoint);
        store32(0x58, g_xexHeader);

        BindVariableImports(xexFile);
    }

    // Same encoding as getOptHeaderPtr: keys ending in 0x00 hold the value inline, 0x01 point at the
    // inline value, anything else holds an offset from the header start.
    static uint32_t RtlImageXexHeaderField(uint32_t xexHeader, uint32_t key)
    {
        auto* header = Translate<Xex2Header>(xexHeader);
        auto* optHeaders = reinterpret_cast<const Xex2OptHeader*>(header + 1);
        for (uint32_t i = 0; i < header->headerCount; i++)
        {
            const Xex2OptHeader& opt = optHeaders[i];
            if (opt.key != key)
                continue;
            if ((key & 0xFF) == 0)
                return opt.value;
            if ((key & 0xFF) == 1)
                return ToGuest(&opt.value);
            return xexHeader + opt.offset;
        }
        return 0;
    }

    static uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* module)
    {
        if (!name)
        {
            *module = g_ldrEntry;
            return ERROR_SUCCESS;
        }
        Log("XexGetModuleHandle({}): not found", name);
        *module = 0;
        return ERROR_NOT_FOUND;
    }

    static uint32_t XexCheckExecutablePrivilege(uint32_t)
    {
        return 0;
    }

    static uint32_t XexLoadImage(const char* name, uint32_t, uint32_t, be<uint32_t>* module)
    {
        // Peggle 2 probes for optional debug modules (xbdm). They do not exist on retail consoles either.
        Log("XexLoadImage({}): not available", name ? name : "(null)");
        if (module)
            *module = 0;
        return STATUS_NO_SUCH_FILE;
    }

    static uint32_t XexGetProcedureAddress(uint32_t, uint32_t, be<uint32_t>* out)
    {
        *out = 0;
        return STATUS_NOT_IMPLEMENTED;
    }

    static void XexUnloadImage(uint32_t)
    {
    }
}

using namespace rt;
GUEST_IMPORT(RtlImageXexHeaderField, RtlImageXexHeaderField)
GUEST_IMPORT(XexGetModuleHandle, XexGetModuleHandle)
GUEST_IMPORT(XexCheckExecutablePrivilege, XexCheckExecutablePrivilege)
GUEST_IMPORT(XexLoadImage, XexLoadImage)
GUEST_IMPORT(XexGetProcedureAddress, XexGetProcedureAddress)
GUEST_IMPORT(XexUnloadImage, XexUnloadImage)
