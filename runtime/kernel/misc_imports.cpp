#include "../guest.h"
#include "../thread.h"
#include "xbox_types.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <string>

namespace rt
{
    static void KeBugCheckEx(uint32_t code, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        Fatal("KeBugCheckEx({:X}, {:X}, {:X}, {:X}, {:X}) lr={:X}", code, a, b, c, d, uint32_t(CurrentThread()->ctx->lr));
    }

    static void KeBugCheck(uint32_t code)
    {
        KeBugCheckEx(code, 0, 0, 0, 0);
    }

    static void HalReturnToFirmware(uint32_t routine)
    {
        Log("HalReturnToFirmware({}): title requested exit", routine);
        exit(0);
    }

    static void DbgBreakPoint()
    {
        Log("DbgBreakPoint lr={:X}", uint32_t(CurrentThread()->ctx->lr));
    }

    // EXCEPTION_RECORD: code, flags, record, address, parameter count, parameters[].
    static void RtlRaiseException(uint32_t record)
    {
        auto* r = Translate<be<uint32_t>>(record);
        const uint32_t code = r[0];

        // MSVC names threads by raising 0x406D1388 with a THREADNAME_INFO for an attached debugger.
        // Without a debugger the title's handler swallows it, so naming it and continuing is exact.
        constexpr uint32_t SetThreadNameException = 0x406D1388;
        if (code == SetThreadNameException && r[4] >= 2 && r[5] == 0x1000)
        {
            const char* name = Translate<char>(r[6]);
            uint32_t threadId = r[7];
            Log("Thread {} named \"{}\"", threadId == 0xFFFFFFFF ? CurrentThread()->id : threadId, name ? name : "");
            return;
        }

        Fatal("RtlRaiseException code={:X} lr={:X}", code, uint32_t(CurrentThread()->ctx->lr));
    }

    // --- Configuration -------------------------------------------------------------------------

    static std::string CFStringToStd(CFStringRef string)
    {
        char buffer[64] = {};
        if (string)
            CFStringGetCString(string, buffer, sizeof(buffer), kCFStringEncodingUTF8);
        return buffer;
    }

    // The console language follows the Mac's preferred language where the title has it (English, German,
    // Spanish, French, Italian, Portuguese); otherwise English. Values are XC_LANGUAGE_*.
    uint32_t ConsoleLanguage()
    {
        std::string preferred;
        if (CFArrayRef languages = CFLocaleCopyPreferredLanguages())
        {
            if (CFArrayGetCount(languages) > 0)
                preferred = CFStringToStd(static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0)));
            CFRelease(languages);
        }
        const std::string code = preferred.substr(0, 2);
        if (code == "de") return 3;
        if (code == "fr") return 4;
        if (code == "es") return 5;
        if (code == "it") return 6;
        if (code == "pt") return 9;
        return 1;
    }

    // The Mac's region as an XC_COUNTRY_* code (United States when unknown).
    static uint8_t ConsoleCountry()
    {
        static const char* const countries[] = {
            nullptr, "AE", "AL", "AM", "AR", "AT", "AU", "AZ", "BE", "BG", "BH", "BN", "BO", "BR", "BY", "BZ", "CA", nullptr, "CH", "CL",
            "CN", "CO", "CR", "CZ", "DE", "DK", "DO", "DZ", "EC", "EE", "EG", "ES", "FI", "FO", "FR", "GB", "GE", "GR", "GT", "HK",
            "HN", "HR", "HU", "ID", "IE", "IL", "IN", "IQ", "IR", "IS", "IT", "JM", "JO", "JP", "KE", "KG", "KR", "KW", "KZ", "LB",
            "LI", "LT", "LU", "LV", "LY", "MA", "MC", "MK", "MN", "MO", "MV", "MX", "MY", "NI", "NL", "NO", "NZ", "OM", "PA", "PE",
            "PH", "PK", "PL", "PR", "PT", "PY", "QA", "RO", "RU", "SA", "SE", "SG", "SI", "SK", nullptr, "SV", "SY", "TH", "TN", "TR",
            "TT", "TW", "UA", "US", "UY", "UZ", "VE", "VN", "YE", "ZA",
        };
        std::string region;
        if (CFLocaleRef locale = CFLocaleCopyCurrent())
        {
            region = CFStringToStd(static_cast<CFStringRef>(CFLocaleGetValue(locale, kCFLocaleCountryCode)));
            CFRelease(locale);
        }
        for (uint8_t i = 1; i < std::size(countries); i++)
            if (countries[i] && region == countries[i])
                return i;
        return 103; // US
    }

    static uint32_t ExGetXConfigSetting(uint16_t category, uint16_t setting, uint8_t* buffer, uint16_t bufferSize, be<uint16_t>* requiredSize)
    {
        uint8_t value[4] = {};
        uint16_t size = 4;
        auto set32 = [&](uint32_t v) { *reinterpret_cast<be<uint32_t>*>(value) = v; };

        if (category == 2 && setting == 2)      // XCONFIG_SECURED_AV_REGION
            set32(0x00001000);                  // NTSC/USA
        else if (category == 3)                 // XCONFIG_USER_CATEGORY
        {
            switch (setting)
            {
            case 9:  set32(ConsoleLanguage()); break;          // XCONFIG_USER_LANGUAGE
            case 0xA: set32(0x00040000); break;                // XCONFIG_USER_VIDEO_FLAGS: widescreen
            case 0xE: size = 1; value[0] = ConsoleCountry(); break; // XCONFIG_USER_COUNTRY
            default: set32(0); break;           // time zone and retail flags
            }
        }
        else
        {
            Log("ExGetXConfigSetting: unknown category {} setting {}", category, setting);
            return STATUS_INVALID_PARAMETER;
        }

        if (requiredSize)
            *requiredSize = size;
        if (buffer)
        {
            if (bufferSize < size)
                return 0xC0000023; // STATUS_BUFFER_TOO_SMALL
            memcpy(buffer, value, size);
        }
        return STATUS_SUCCESS;
    }

    static uint32_t XGetGameRegion()
    {
        return 0xFFFF; // region free
    }

    static uint32_t XGetAVPack()
    {
        return 6; // HDMI
    }

    struct VideoMode
    {
        be<uint32_t> displayWidth;
        be<uint32_t> displayHeight;
        be<uint32_t> isInterlaced;
        be<uint32_t> isWidescreen;
        be<uint32_t> isHiDef;
        be<float> refreshRate;
        be<uint32_t> videoStandard;
        be<uint32_t> pad[5];
    };

    static void XGetVideoMode(VideoMode* mode)
    {
        memset(mode, 0, sizeof(*mode));
        mode->displayWidth = 1280;
        mode->displayHeight = 720;
        mode->isWidescreen = 1;
        mode->isHiDef = 1;
        mode->refreshRate = 60.0f;
        mode->videoStandard = 1; // NTSC
    }

    static uint32_t XamGetSystemVersion()
    {
        return 0x20447700; // 2.0.17489.0, the last dashboard
    }

    static uint32_t XNetLogonGetTitleID()
    {
        return 0x58411436;
    }

    static uint32_t ExRegisterTitleTerminateNotification(uint32_t, uint32_t)
    {
        return STATUS_SUCCESS;
    }

    static uint32_t KeEnableFpuExceptions(uint32_t)
    {
        return 0;
    }

    static void KiApcNormalRoutineNop()
    {
    }

}

using namespace rt;
GUEST_IMPORT(KeBugCheck, KeBugCheck)
GUEST_IMPORT(KeBugCheckEx, KeBugCheckEx)
GUEST_IMPORT(HalReturnToFirmware, HalReturnToFirmware)
GUEST_IMPORT(DbgBreakPoint, DbgBreakPoint)
GUEST_IMPORT(RtlRaiseException, RtlRaiseException)
GUEST_IMPORT(ExGetXConfigSetting, ExGetXConfigSetting)
GUEST_IMPORT(XGetGameRegion, XGetGameRegion)
GUEST_IMPORT(XGetAVPack, XGetAVPack)
GUEST_IMPORT(XGetVideoMode, XGetVideoMode)
GUEST_IMPORT(XamGetSystemVersion, XamGetSystemVersion)
GUEST_IMPORT(XNetLogonGetTitleID, XNetLogonGetTitleID)
GUEST_IMPORT(ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification)
GUEST_IMPORT(KeEnableFpuExceptions, KeEnableFpuExceptions)
GUEST_IMPORT(KiApcNormalRoutineNop, KiApcNormalRoutineNop)
