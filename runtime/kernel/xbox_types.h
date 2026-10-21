// Xbox 360 kernel status codes, error codes and object layouts shared by the imports.
#pragma once
#include <cstdint>
#include <xbox.h>

namespace rt
{
    enum : uint32_t
    {
        STATUS_SUCCESS = 0x00000000,
        STATUS_ABANDONED_WAIT_0 = 0x00000080,
        STATUS_USER_APC = 0x000000C0,
        STATUS_ALERTED = 0x00000101,
        STATUS_TIMEOUT = 0x00000102,
        STATUS_PENDING = 0x00000103,
        STATUS_TIMER_RESUME_IGNORED = 0x40000025,
        STATUS_BUFFER_OVERFLOW = 0x80000005,
        STATUS_NO_MORE_FILES = 0x80000006,
        STATUS_UNSUCCESSFUL = 0xC0000001,
        STATUS_NOT_IMPLEMENTED = 0xC0000002,
        STATUS_INFO_LENGTH_MISMATCH = 0xC0000004,
        STATUS_INVALID_HANDLE = 0xC0000008,
        STATUS_INVALID_PARAMETER = 0xC000000D,
        STATUS_NO_SUCH_FILE = 0xC000000F,
        STATUS_END_OF_FILE = 0xC0000011,
        STATUS_NO_MEMORY = 0xC0000017,
        STATUS_ACCESS_DENIED = 0xC0000022,
        STATUS_BUFFER_TOO_SMALL = 0xC0000023,
        STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
        STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
        STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
        STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
        STATUS_MUTANT_NOT_OWNED = 0xC0000046,
        STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
        STATUS_DISK_FULL = 0xC000007F,
        STATUS_NOT_SUPPORTED = 0xC00000BB,
    };

    enum : uint32_t
    {
        ERROR_SUCCESS = 0,
        ERROR_FILE_NOT_FOUND = 2,
        ERROR_PATH_NOT_FOUND = 3,
        ERROR_ACCESS_DENIED = 5,
        ERROR_INVALID_HANDLE = 6,
        ERROR_NO_MORE_FILES = 18,
        ERROR_INVALID_PARAMETER = 87,
        ERROR_INSUFFICIENT_BUFFER = 122,
        ERROR_ALREADY_EXISTS = 183,
        ERROR_IO_PENDING = 997,
        ERROR_DEVICE_NOT_CONNECTED = 1167,
        ERROR_NOT_FOUND = 1168,
        ERROR_CANCELLED = 1223,
        ERROR_NO_SUCH_USER = 1317,
        ERROR_FUNCTION_FAILED = 1627,
    };

    // HRESULTs.
    constexpr uint32_t E_OUTOFMEMORY = 0x8007000E;
    constexpr uint32_t E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525; // HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER)

    // KOBJECTS type values found in dispatcher headers.
    enum : uint8_t
    {
        TypeNotificationEvent = 0,
        TypeSynchronizationEvent = 1,
        TypeMutant = 2,
        TypeSemaphore = 5,
        TypeThread = 6,
        TypeNotificationTimer = 8,
        TypeSynchronizationTimer = 9,
    };

    struct DispatcherHeader
    {
        uint8_t type;
        uint8_t absolute;
        uint8_t size;
        uint8_t inserted;
        be<int32_t> signalState;
        be<uint32_t> waitListFlink;
        be<uint32_t> waitListBlink;
    };
    static_assert(sizeof(DispatcherHeader) == 0x10);

    struct KMutant
    {
        DispatcherHeader header;
        be<uint32_t> mutantListFlink;
        be<uint32_t> mutantListBlink;
        be<uint32_t> ownerThread;
        uint8_t abandoned;
        uint8_t pad[3];
    };

    struct KSemaphore
    {
        DispatcherHeader header;
        be<int32_t> limit;
    };

    struct RtlCriticalSection
    {
        DispatcherHeader header;
        be<int32_t> lockCount;      // -1 when free
        be<int32_t> recursionCount;
        be<uint32_t> owningThread;
    };
    static_assert(sizeof(RtlCriticalSection) == 0x1C);

    struct AnsiString
    {
        be<uint16_t> length;
        be<uint16_t> maximumLength;
        be<uint32_t> buffer;
    };

    struct ObjectAttributes
    {
        be<uint32_t> rootDirectory;
        be<uint32_t> name; // AnsiString*
        be<uint32_t> attributes;
    };

    struct IoStatusBlock
    {
        be<uint32_t> status;
        be<uint32_t> information;
    };

    // 100 ns units since 1601-01-01, like FILETIME.
    uint64_t GuestSystemTime();
    // A kernel timeout (negative: relative, positive: absolute, in 100 ns) as a clamped relative duration.
    int64_t RelativeTimeout100ns(int64_t value);
}
