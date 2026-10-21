// C runtime string imports the title calls through the kernel (sprintf family, Rtl* strings, times).
#include "../guest.h"
#include "xbox_types.h"
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

namespace rt
{
    struct UnicodeString
    {
        be<uint16_t> length;        // bytes
        be<uint16_t> maximumLength; // bytes
        be<uint32_t> buffer;        // big-endian UTF-16
    };

    static void RtlInitAnsiString(AnsiString* destination, const char* source)
    {
        uint16_t length = source ? uint16_t(strlen(source)) : 0;
        destination->length = length;
        destination->maximumLength = source ? length + 1 : 0;
        destination->buffer = ToGuest(source);
    }

    static void RtlInitUnicodeString(UnicodeString* destination, const be<uint16_t>* source)
    {
        uint16_t length = 0;
        while (source && source[length] != 0)
            length++;
        destination->length = length * 2;
        destination->maximumLength = source ? (length + 1) * 2 : 0;
        destination->buffer = ToGuest(source);
    }

    static uint32_t RtlUnicodeStringToAnsiString(AnsiString* destination, const UnicodeString* source, uint32_t allocate)
    {
        uint32_t chars = source->length / 2;
        if (allocate)
        {
            destination->buffer = g_systemHeap.Alloc(chars + 1);
            destination->maximumLength = chars + 1;
        }
        else if (chars + 1 > destination->maximumLength)
            return STATUS_BUFFER_OVERFLOW;

        auto* src = Translate<be<uint16_t>>(source->buffer);
        auto* dst = Translate<char>(destination->buffer);
        for (uint32_t i = 0; i < chars; i++)
            dst[i] = src[i] < 0x80 ? char(uint16_t(src[i])) : '?';
        dst[chars] = 0;
        destination->length = chars;
        return STATUS_SUCCESS;
    }

    static void RtlFreeAnsiString(AnsiString* string)
    {
        if (string->buffer)
            g_systemHeap.Free(string->buffer);
        string->buffer = 0;
        string->length = 0;
        string->maximumLength = 0;
    }

    static uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* destination, uint32_t destinationBytes, be<uint32_t>* written, const char* source, uint32_t sourceBytes)
    {
        uint32_t count = std::min(destinationBytes / 2, sourceBytes);
        for (uint32_t i = 0; i < count; i++)
            destination[i] = uint8_t(source[i]);
        if (written)
            *written = count * 2;
        return STATUS_SUCCESS;
    }

    static uint32_t RtlUnicodeToMultiByteN(char* destination, uint32_t destinationBytes, be<uint32_t>* written, const be<uint16_t>* source, uint32_t sourceBytes)
    {
        uint32_t count = std::min(destinationBytes, sourceBytes / 2);
        for (uint32_t i = 0; i < count; i++)
            destination[i] = source[i] < 0x100 ? char(uint16_t(source[i])) : '?';
        if (written)
            *written = count;
        return STATUS_SUCCESS;
    }

    struct TimeFields
    {
        be<uint16_t> year, month, day, hour, minute, second, milliseconds, weekday;
    };

    static void RtlTimeToTimeFields(const be<uint64_t>* time, TimeFields* fields)
    {
        constexpr uint64_t EpochDelta = 11644473600ull;
        uint64_t t = *time;
        time_t seconds = time_t(t / 10000000ull - EpochDelta);
        tm parts;
        gmtime_r(&seconds, &parts);
        fields->year = parts.tm_year + 1900;
        fields->month = parts.tm_mon + 1;
        fields->day = parts.tm_mday;
        fields->hour = parts.tm_hour;
        fields->minute = parts.tm_min;
        fields->second = parts.tm_sec;
        fields->milliseconds = uint16_t((t / 10000) % 1000);
        fields->weekday = parts.tm_wday;
    }

    static uint32_t RtlTimeFieldsToTime(const TimeFields* fields, be<uint64_t>* time)
    {
        constexpr uint64_t EpochDelta = 11644473600ull;
        tm parts{};
        parts.tm_year = fields->year - 1900;
        parts.tm_mon = fields->month - 1;
        parts.tm_mday = fields->day;
        parts.tm_hour = fields->hour;
        parts.tm_min = fields->minute;
        parts.tm_sec = fields->second;
        time_t seconds = timegm(&parts);
        if (seconds < 0)
            return 0;
        *time = (uint64_t(seconds) + EpochDelta) * 10000000ull + uint64_t(fields->milliseconds) * 10000;
        return 1;
    }

    static uint32_t RtlNtStatusToDosError(uint32_t status)
    {
        switch (status)
        {
        case STATUS_SUCCESS: return ERROR_SUCCESS;
        case STATUS_PENDING: return ERROR_IO_PENDING;
        case STATUS_NO_SUCH_FILE:
        case STATUS_OBJECT_NAME_NOT_FOUND: return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND: return ERROR_PATH_NOT_FOUND;
        case STATUS_ACCESS_DENIED: return ERROR_ACCESS_DENIED;
        case STATUS_INVALID_HANDLE: return ERROR_INVALID_HANDLE;
        case STATUS_INVALID_PARAMETER: return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MORE_FILES: return ERROR_NO_MORE_FILES;
        case STATUS_OBJECT_NAME_COLLISION: return ERROR_ALREADY_EXISTS;
        case STATUS_END_OF_FILE: return 38; // ERROR_HANDLE_EOF
        default:
            if ((status & 0xFFFF0000) == 0x80070000) // HRESULT_FROM_WIN32
                return status & 0xFFFF;
            Log("RtlNtStatusToDosError: unmapped status {:X}", status);
            return ERROR_FUNCTION_FAILED;
        }
    }

    // --- Guest printf --------------------------------------------------------------------------

    // Formats a guest printf string. next() returns the next 64-bit argument slot; doubles arrive as raw bits.
    static std::string FormatGuestString(const char* format, const std::function<uint64_t()>& next)
    {
        std::string out;
        for (const char* p = format; *p; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }
            if (p[1] == '%')
            {
                out += '%';
                p++;
                continue;
            }

            std::string spec = "%";
            p++;
            while (*p && strchr("-+ #0", *p))
                spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(next()));
                p++;
            }
            while (*p >= '0' && *p <= '9')
                spec += *p++;
            if (*p == '.')
            {
                spec += *p++;
                if (*p == '*')
                {
                    spec += std::to_string(int32_t(next()));
                    p++;
                }
                while (*p >= '0' && *p <= '9')
                    spec += *p++;
            }

            bool wide64 = false, shortArg = false, wideChar = false;
            while (*p && strchr("hlLqIjzt", *p))
            {
                if (*p == 'h') shortArg = true;
                if (*p == 'l' && p[1] == 'l') wide64 = true;
                if (*p == 'L' || *p == 'q' || *p == 'j') wide64 = true;
                if (*p == 'I' && p[1] == '6' && p[2] == '4') { wide64 = true; p += 2; }
                if (*p == 'l') wideChar = true;
                p++;
            }
            if (!*p)
                break;

            char conversion = *p;
            char buffer[512];
            switch (conversion)
            {
            case 'd': case 'i':
            {
                uint64_t v = next();
                int64_t value = wide64 ? int64_t(v) : shortArg ? int16_t(v) : int32_t(v);
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)value);
                out += buffer;
                break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                uint64_t v = next();
                uint64_t value = wide64 ? v : shortArg ? uint16_t(v) : uint32_t(v);
                snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), (unsigned long long)value);
                out += buffer;
                break;
            }
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(next()));
                out += buffer;
                break;
            case 'c':
                out += char(next());
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                uint64_t bits = next();
                double value;
                memcpy(&value, &bits, sizeof(value));
                snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
                out += buffer;
                break;
            }
            case 's': case 'S':
            {
                uint32_t address = uint32_t(next());
                std::string text;
                if (!address)
                    text = "(null)";
                else if (conversion == 'S' || wideChar)
                {
                    for (auto* w = Translate<be<uint16_t>>(address); *w != 0; w++)
                        text += uint16_t(*w) < 0x80 ? char(uint16_t(*w)) : '?';
                }
                else
                    text = Translate<char>(address);
                if (spec == "%")
                    out += text;
                else
                {
                    int n = snprintf(nullptr, 0, (spec + "s").c_str(), text.c_str());
                    std::string padded(size_t(n), '\0');
                    snprintf(padded.data(), padded.size() + 1, (spec + "s").c_str(), text.c_str());
                    out += padded;
                }
                break;
            }
            default:
                out += spec + conversion;
                break;
            }
        }
        return out;
    }

    // MSVC _snprintf semantics: -1 when the output does not fit, and then no terminator.
    static uint32_t CopyOut(char* buffer, uint32_t count, const std::string& text)
    {
        if (count == 0)
            return 0xFFFFFFFF;
        size_t n = std::min<size_t>(text.size(), count);
        memcpy(buffer, text.data(), n);
        if (n < count)
            buffer[n] = 0;
        return text.size() > count ? 0xFFFFFFFF : uint32_t(text.size());
    }

    // Variadic imports read their arguments straight from the register/stack slots.
    PPC_FUNC(sprintfImpl)
    {
        size_t slot = 2;
        std::string text = FormatGuestString(Translate<char>(ctx.r4.u32), [&] { return IntArg(ctx, slot++, 8); });
        char* buffer = Translate<char>(ctx.r3.u32);
        memcpy(buffer, text.c_str(), text.size() + 1);
        ctx.r3.u64 = text.size();
    }

    PPC_FUNC(snprintfImpl)
    {
        size_t slot = 3;
        std::string text = FormatGuestString(Translate<char>(ctx.r5.u32), [&] { return IntArg(ctx, slot++, 8); });
        ctx.r3.u64 = int64_t(int32_t(CopyOut(Translate<char>(ctx.r3.u32), ctx.r4.u32, text)));
    }

    PPC_FUNC(vsnprintfImpl)
    {
        // The va_list points at consecutive 8-byte argument slots.
        uint32_t list = ctx.r6.u32;
        std::string text = FormatGuestString(Translate<char>(ctx.r5.u32), [&] {
            uint64_t value = *Translate<be<uint64_t>>(list);
            list += 8;
            return value;
        });
        ctx.r3.u64 = int64_t(int32_t(CopyOut(Translate<char>(ctx.r3.u32), ctx.r4.u32, text)));
    }

    PPC_FUNC(DbgPrintImpl)
    {
        size_t slot = 1;
        std::string text = FormatGuestString(Translate<char>(ctx.r3.u32), [&] { return IntArg(ctx, slot++, 8); });
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        Log("DbgPrint: {}", text);
        ctx.r3.u64 = 0;
    }
}

using namespace rt;
GUEST_IMPORT(RtlInitAnsiString, RtlInitAnsiString)
GUEST_IMPORT(RtlInitUnicodeString, RtlInitUnicodeString)
GUEST_IMPORT(RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString)
GUEST_IMPORT(RtlFreeAnsiString, RtlFreeAnsiString)
GUEST_IMPORT(RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN)
GUEST_IMPORT(RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN)
GUEST_IMPORT(RtlTimeToTimeFields, RtlTimeToTimeFields)
GUEST_IMPORT(RtlTimeFieldsToTime, RtlTimeFieldsToTime)
GUEST_IMPORT(RtlNtStatusToDosError, RtlNtStatusToDosError)
PPC_FUNC(__imp__sprintf) { rt::sprintfImpl(ctx, base); }
PPC_FUNC(__imp___snprintf) { rt::snprintfImpl(ctx, base); }
PPC_FUNC(__imp___vsnprintf) { rt::vsnprintfImpl(ctx, base); }
PPC_FUNC(__imp__DbgPrint) { rt::DbgPrintImpl(ctx, base); }
