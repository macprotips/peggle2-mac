// File system imports (Nt* file I/O on the mounted devices), with crash-safe saves.
#include <atomic>
#include "../atomic_file.h"
#include "../guest.h"
#include "objects.h"
#include "vfs.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

namespace rt
{
    enum : uint32_t
    {
        FILE_SUPERSEDE = 0,
        FILE_OPEN = 1,
        FILE_CREATE = 2,
        FILE_OPEN_IF = 3,
        FILE_OVERWRITE = 4,
        FILE_OVERWRITE_IF = 5,

        FILE_DIRECTORY_FILE = 0x1,
        FILE_NON_DIRECTORY_FILE = 0x40,

        FILE_SUPERSEDED = 0,
        FILE_OPENED = 1,
        FILE_CREATED = 2,
        FILE_OVERWRITTEN = 3,

        FILE_ATTRIBUTE_READONLY = 0x1,
        FILE_ATTRIBUTE_DIRECTORY = 0x10,
        FILE_ATTRIBUTE_NORMAL = 0x80,

        GENERIC_WRITE = 0x40000000,
        GENERIC_ALL = 0x10000000,
        FILE_WRITE_DATA = 0x2,
        FILE_APPEND_DATA = 0x4,
    };

    std::atomic<int>& SavesInProgress()
    {
        static std::atomic<int> count;
        return count;
    }

    struct FileObject : HostObject
    {
        std::string guestPath;
        std::filesystem::path hostPath;
        int fd = -1;
        bool directory = false;
        uint64_t position = 0;
        std::mutex mutex;

        // Directory enumeration state for NtQueryDirectoryFile.
        std::vector<std::filesystem::directory_entry> entries;
        size_t nextEntry = 0;
        bool listed = false;

        // Files created or overwritten on a writable device (saves) are written to a temporary file and
        // replace the original only when closed intact, so a crash, power loss or full disk mid-save
        // leaves the previous save as it was.
        std::filesystem::path commitPath;
        bool writeFailed = false;
        bool writable = false; // on a writable device (saves, cache); game files are read-only

        ~FileObject() override
        {
            if (fd < 0)
                return;
            if (commitPath.empty())
            {
                close(fd);
                return;
            }
            const bool synced = FlushToStorage(fd);
            close(fd);
            std::error_code error;
            if (synced && !writeFailed)
                std::filesystem::rename(hostPath, commitPath, error);
            else
                error = std::make_error_code(std::errc::io_error);
            if (error)
            {
                if (!writeFailed || std::filesystem::exists(hostPath))
                    Log("Saving {} failed; the previous version is kept", commitPath.string());
                std::filesystem::remove(hostPath, error);
            }
            SavesInProgress()--;
        }
    };

    static std::string GuestString(const AnsiString* s)
    {
        return s && s->buffer ? std::string(Translate<char>(s->buffer), s->length) : std::string();
    }

    static std::string PathFromAttributes(const ObjectAttributes* attributes)
    {
        std::string name = GuestString(Translate<AnsiString>(attributes->name));
        if (attributes->rootDirectory)
        {
            if (auto root = HandleTo<FileObject>(attributes->rootDirectory))
                return root->guestPath + "\\" + name;
        }
        return name;
    }

    static uint64_t ToFileTime(const struct timespec& ts)
    {
        constexpr uint64_t EpochDelta = 11644473600ull;
        return (uint64_t(ts.tv_sec) + EpochDelta) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
    }

    static uint32_t Attributes(const struct stat& st, bool writable)
    {
        if (S_ISDIR(st.st_mode))
            return FILE_ATTRIBUTE_DIRECTORY;
        return writable ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY;
    }

    struct NetworkOpenInfo
    {
        be<uint64_t> creationTime, lastAccessTime, lastWriteTime, changeTime;
        be<uint64_t> allocationSize, endOfFile;
        be<uint32_t> attributes, pad;
    };
    static_assert(sizeof(NetworkOpenInfo) == 56);

    static void FillNetworkInfo(NetworkOpenInfo* info, const struct stat& st, bool writable)
    {
        info->creationTime = ToFileTime(st.st_birthtimespec);
        info->lastAccessTime = ToFileTime(st.st_atimespec);
        info->lastWriteTime = ToFileTime(st.st_mtimespec);
        info->changeTime = ToFileTime(st.st_ctimespec);
        info->allocationSize = (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull;
        info->endOfFile = uint64_t(st.st_size);
        info->attributes = Attributes(st, writable);
        info->pad = 0;
    }

    static void CompleteIo(IoStatusBlock* io, uint32_t status, uint32_t information, uint32_t event)
    {
        if (io)
        {
            io->status = status;
            io->information = information;
        }
        if (event)
        {
            if (uint32_t object = HandleToObject(event))
                SetEvent(object);
        }
    }

    static uint32_t OpenFile(be<uint32_t>* handleOut, uint32_t desiredAccess, const ObjectAttributes* attributes, IoStatusBlock* io, uint32_t disposition, uint32_t options)
    {
        std::string guestPath = PathFromAttributes(attributes);
        auto resolved = ResolvePath(guestPath);
        if (!resolved)
        {
            Log("Open {}: no device", guestPath);
            CompleteIo(io, STATUS_OBJECT_PATH_NOT_FOUND, 0, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        const bool wantsWrite = desiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
        const bool creates = disposition != FILE_OPEN;
        struct stat st;
        bool exists = stat(resolved->host.c_str(), &st) == 0;

        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        {
            if (g_traceImports)
                Log("Open {} -> {}: not found", guestPath, resolved->host.string());
            uint32_t status = std::filesystem::exists(resolved->host.parent_path()) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            CompleteIo(io, status, 0, 0);
            return status;
        }
        if (exists && disposition == FILE_CREATE)
        {
            CompleteIo(io, STATUS_OBJECT_NAME_COLLISION, 0, 0);
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if ((creates || wantsWrite) && !resolved->writable && !(exists && !wantsWrite))
        {
            Log("Open {}: write access to read-only device denied", guestPath);
            CompleteIo(io, STATUS_ACCESS_DENIED, 0, 0);
            return STATUS_ACCESS_DENIED;
        }

        auto file = std::make_shared<FileObject>();
        file->guestPath = guestPath;
        file->hostPath = resolved->host;
        file->writable = resolved->writable;
        uint32_t information = FILE_OPENED;

        if ((options & FILE_DIRECTORY_FILE) || (exists && S_ISDIR(st.st_mode)))
        {
            if (!exists)
            {
                std::error_code ec;
                std::filesystem::create_directories(resolved->host, ec);
                information = FILE_CREATED;
            }
            file->directory = true;
        }
        else
        {
            int flags = wantsWrite || creates ? O_RDWR : O_RDONLY;
            if (!resolved->writable)
                flags = O_RDONLY;
            const bool replaces = exists && (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE);
            if (!exists)
            {
                flags |= O_CREAT;
                information = FILE_CREATED;
            }
            else if (replaces)
                information = disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
            if (resolved->writable && (!exists || replaces))
            {
                file->commitPath = resolved->host;
                file->hostPath = resolved->host.string() + ".saving";
                flags |= O_CREAT | O_TRUNC;
            }
            file->fd = open(file->hostPath.c_str(), flags, 0644);
            if (file->fd >= 0 && !file->commitPath.empty())
                SavesInProgress()++;
            if (file->fd < 0)
            {
                Log("Open {} -> {} failed: {}", guestPath, resolved->host.string(), strerror(errno));
                CompleteIo(io, STATUS_ACCESS_DENIED, 0, 0);
                return STATUS_ACCESS_DENIED;
            }
        }

        if (g_traceImports)
            Log("Open {} -> {} ({})", guestPath, resolved->host.string(), information);
        *handleOut = CreateHandle(0, file);
        CompleteIo(io, STATUS_SUCCESS, information, 0);
        return STATUS_SUCCESS;
    }

    static uint32_t NtCreateFile(be<uint32_t>* handleOut, uint32_t desiredAccess, ObjectAttributes* attributes, IoStatusBlock* io,
        uint32_t, uint32_t, uint32_t, uint32_t disposition, uint32_t options)
    {
        return OpenFile(handleOut, desiredAccess, attributes, io, disposition, options);
    }

    static uint32_t NtOpenFile(be<uint32_t>* handleOut, uint32_t desiredAccess, ObjectAttributes* attributes, IoStatusBlock* io, uint32_t options)
    {
        return OpenFile(handleOut, desiredAccess, attributes, io, FILE_OPEN, options);
    }

    // pread into guest memory. A system call cannot take the write fault that keeps a watched page's dirty
    // mark (it fails instead), and lifting the watch first races with the GPU watching the page again; so
    // reads into the physical windows (where watches live) land in a host buffer and are copied in.
    static ssize_t ReadGuest(int fd, uint8_t* dest, uint32_t length, off_t position)
    {
        if (!IsPhysicalWindow(ToGuest(dest)))
            return pread(fd, dest, length, position);
        thread_local std::vector<uint8_t> staging;
        staging.resize(length);
        const ssize_t n = pread(fd, staging.data(), length, position);
        if (n > 0)
            memcpy(dest, staging.data(), size_t(n));
        return n;
    }

    static uint32_t NtReadFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t, IoStatusBlock* io, uint8_t* buffer, uint32_t length, be<int64_t>* offset)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file || file->fd < 0)
            return STATUS_INVALID_HANDLE;
        if (apcRoutine)
            Log("NtReadFile: APC completion routine {:X} not supported", apcRoutine);

        std::lock_guard lock(file->mutex);
        // An offset of -2 (FILE_USE_FILE_POINTER_POSITION) means the current position.
        if (offset && int64_t(*offset) >= 0)
            file->position = uint64_t(int64_t(*offset));
        ssize_t n = ReadGuest(file->fd, buffer, length, off_t(file->position));
        if (n < 0)
        {
            Log("NtReadFile: read of {} bytes at {} failed: {}", length, file->position, strerror(errno));
            CompleteIo(io, STATUS_UNSUCCESSFUL, 0, event);
            return STATUS_UNSUCCESSFUL;
        }
        file->position += uint64_t(n);
        uint32_t status = (n == 0 && length > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        CompleteIo(io, status, uint32_t(n), event);
        return status;
    }

    static uint32_t NtReadFileScatter(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t, IoStatusBlock* io, be<uint32_t>* segments, uint32_t length, be<int64_t>* offset)
    {
        // Segments are 4 KB pages.
        auto file = HandleTo<FileObject>(handle);
        if (!file || file->fd < 0)
            return STATUS_INVALID_HANDLE;
        if (apcRoutine)
            Log("NtReadFileScatter: APC completion routine {:X} not supported", apcRoutine);
        std::lock_guard lock(file->mutex);
        if (offset && int64_t(*offset) >= 0)
            file->position = uint64_t(int64_t(*offset));
        uint32_t total = 0;
        for (uint32_t i = 0; total < length; i++)
        {
            uint32_t chunk = std::min<uint32_t>(0x1000, length - total);
            ssize_t n = ReadGuest(file->fd, Translate(segments[i]), chunk, off_t(file->position));
            if (n <= 0)
                break;
            file->position += uint64_t(n);
            total += uint32_t(n);
            if (uint32_t(n) < chunk)
                break;
        }
        uint32_t status = (total == 0 && length > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        CompleteIo(io, status, total, event);
        return status;
    }

    static uint32_t NtWriteFile(uint32_t handle, uint32_t event, uint32_t, uint32_t, IoStatusBlock* io, const uint8_t* buffer, uint32_t length, be<int64_t>* offset)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file || file->fd < 0)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(file->mutex);
        if (offset && int64_t(*offset) >= 0)
            file->position = uint64_t(int64_t(*offset));
        ssize_t n = pwrite(file->fd, buffer, length, off_t(file->position));
        if (n < 0 || uint32_t(n) < length)
        {
            file->writeFailed = true;
            const uint32_t status = (n >= 0 || errno == ENOSPC) ? STATUS_DISK_FULL : STATUS_ACCESS_DENIED;
            CompleteIo(io, status, n > 0 ? uint32_t(n) : 0, event);
            return status;
        }
        file->position += uint64_t(n);
        CompleteIo(io, STATUS_SUCCESS, uint32_t(n), event);
        return STATUS_SUCCESS;
    }

    enum : uint32_t
    {
        FileBasicInformation = 4,
        FileStandardInformation = 5,
        FileInternalInformation = 6,
        FileRenameInformation = 10,
        FileDispositionInformation = 13,
        FilePositionInformation = 14,
        FileAllocationInformation = 19,
        FileEndOfFileInformation = 20,
        FileNetworkOpenInformation = 34,
        FileFsVolumeInformation = 1,
        FileFsSizeInformation = 3,
        FileFsDeviceInformation = 4,
        FileFsAttributeInformation = 5,
    };

    static bool StatFile(FileObject& file, struct stat& st)
    {
        return file.fd >= 0 ? fstat(file.fd, &st) == 0 : stat(file.hostPath.c_str(), &st) == 0;
    }

    static uint32_t NtQueryInformationFile(uint32_t handle, IoStatusBlock* io, uint8_t* out, uint32_t length, uint32_t infoClass)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file)
            return STATUS_INVALID_HANDLE;
        struct stat st;
        if (!StatFile(*file, st))
            return STATUS_UNSUCCESSFUL;

        // Filled locally, then copied only if the caller's buffer holds it.
        alignas(8) uint8_t info[0x40] = {};
        uint32_t written = 0;
        switch (infoClass)
        {
        case FileBasicInformation:
        {
            auto* p = reinterpret_cast<be<uint64_t>*>(info);
            p[0] = ToFileTime(st.st_birthtimespec);
            p[1] = ToFileTime(st.st_atimespec);
            p[2] = ToFileTime(st.st_mtimespec);
            p[3] = ToFileTime(st.st_ctimespec);
            *reinterpret_cast<be<uint32_t>*>(info + 0x20) = Attributes(st, true);
            written = 0x28;
            break;
        }
        case FileStandardInformation:
            reinterpret_cast<be<uint64_t>*>(info)[0] = (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull;
            reinterpret_cast<be<uint64_t>*>(info)[1] = uint64_t(st.st_size);
            *reinterpret_cast<be<uint32_t>*>(info + 0x10) = 1;
            info[0x15] = S_ISDIR(st.st_mode) ? 1 : 0;
            written = 0x18;
            break;
        case FileInternalInformation:
            *reinterpret_cast<be<uint64_t>*>(info) = uint64_t(st.st_ino);
            written = 8;
            break;
        case FilePositionInformation:
            *reinterpret_cast<be<uint64_t>*>(info) = file->position;
            written = 8;
            break;
        case FileNetworkOpenInformation:
            FillNetworkInfo(reinterpret_cast<NetworkOpenInfo*>(info), st, true);
            written = sizeof(NetworkOpenInfo);
            break;
        default:
            Log("NtQueryInformationFile: unhandled class {} for {}", infoClass, file->guestPath);
            return STATUS_INVALID_PARAMETER;
        }
        if (written > length)
            return STATUS_INFO_LENGTH_MISMATCH;
        memcpy(out, info, written);
        CompleteIo(io, STATUS_SUCCESS, written, 0);
        return STATUS_SUCCESS;
    }

    static uint32_t NtSetInformationFile(uint32_t handle, IoStatusBlock* io, uint8_t* info, uint32_t, uint32_t infoClass)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file)
            return STATUS_INVALID_HANDLE;
        switch (infoClass)
        {
        case FilePositionInformation:
            file->position = *reinterpret_cast<be<uint64_t>*>(info);
            break;
        case FileEndOfFileInformation:
        case FileAllocationInformation:
            if (file->fd >= 0 && ftruncate(file->fd, off_t(uint64_t(*reinterpret_cast<be<uint64_t>*>(info)))) != 0)
                return STATUS_ACCESS_DENIED;
            break;
        case FileDispositionInformation:
            if (info[0])
            {
                if (!file->writable)
                    return STATUS_ACCESS_DENIED;
                std::error_code ec;
                std::filesystem::remove(file->hostPath, ec);
                if (!file->commitPath.empty())
                {
                    file->writeFailed = true; // nothing to commit
                    std::filesystem::remove(file->commitPath, ec);
                }
            }
            break;
        default:
            Log("NtSetInformationFile: unhandled class {} for {}", infoClass, file->guestPath);
            return STATUS_INVALID_PARAMETER;
        }
        CompleteIo(io, STATUS_SUCCESS, 0, 0);
        return STATUS_SUCCESS;
    }

    static uint32_t NtQueryFullAttributesFile(ObjectAttributes* attributes, NetworkOpenInfo* info)
    {
        std::string guestPath = PathFromAttributes(attributes);
        auto resolved = ResolvePath(guestPath);
        struct stat st;
        if (!resolved || stat(resolved->host.c_str(), &st) != 0)
        {
            if (g_traceImports)
                Log("QueryFullAttributes {}: not found", guestPath);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        FillNetworkInfo(info, st, resolved->writable);
        return STATUS_SUCCESS;
    }

    static uint32_t NtQueryVolumeInformationFile(uint32_t handle, IoStatusBlock* io, uint8_t* info, uint32_t length, uint32_t infoClass)
    {
        if (!HandleTo<FileObject>(handle))
            return STATUS_INVALID_HANDLE;
        const uint32_t needed = infoClass == FileFsAttributeInformation ? 16 : 0x18;
        if (length < needed)
            return STATUS_INFO_LENGTH_MISMATCH;
        memset(info, 0, length);
        uint32_t written = 0;
        switch (infoClass)
        {
        case FileFsVolumeInformation:
            written = 0x18;
            break;
        case FileFsSizeInformation:
        {
            auto* p = reinterpret_cast<be<uint64_t>*>(info);
            p[0] = 0x100000;  // total allocation units (16 KB each => 16 GB)
            p[1] = 0x80000;   // available
            reinterpret_cast<be<uint32_t>*>(info + 0x10)[0] = 32;  // sectors per unit
            reinterpret_cast<be<uint32_t>*>(info + 0x10)[1] = 512; // bytes per sector
            written = 0x18;
            break;
        }
        case FileFsAttributeInformation:
        {
            auto* p = reinterpret_cast<be<uint32_t>*>(info);
            p[0] = 0;
            p[1] = 255;
            p[2] = 4;
            memcpy(info + 12, "FATX", 4);
            written = 16;
            break;
        }
        default:
            Log("NtQueryVolumeInformationFile: unhandled class {}", infoClass);
            return STATUS_INVALID_PARAMETER;
        }
        CompleteIo(io, STATUS_SUCCESS, written, 0);
        return STATUS_SUCCESS;
    }

    static uint32_t NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t, uint32_t, IoStatusBlock* io, uint8_t* info, uint32_t length, AnsiString* fileName, uint32_t restart)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file || !file->directory)
            return STATUS_INVALID_HANDLE;

        std::lock_guard lock(file->mutex);
        if (!file->listed || restart)
        {
            file->entries.clear();
            std::error_code ec;
            for (auto& entry : std::filesystem::directory_iterator(file->hostPath, ec))
                file->entries.push_back(entry);
            std::sort(file->entries.begin(), file->entries.end());
            file->nextEntry = 0;
            file->listed = true;
        }

        std::string pattern = fileName ? GuestString(fileName) : "*";
        while (file->nextEntry < file->entries.size())
        {
            const auto& entry = file->entries[file->nextEntry++];
            std::string name = entry.path().filename().string();
            // Saves in progress (see FileObject) are not part of the content.
            if (name.ends_with(".saving") || fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) != 0)
                continue;
            uint32_t needed = 0x40 + uint32_t(name.size());
            if (needed > length)
            {
                file->nextEntry--;
                return STATUS_BUFFER_OVERFLOW;
            }
            struct stat st;
            if (stat(entry.path().c_str(), &st) != 0)
                continue; // removed since the listing
            memset(info, 0, needed);
            auto* p32 = reinterpret_cast<be<uint32_t>*>(info);
            auto* p64 = reinterpret_cast<be<uint64_t>*>(info + 8);
            p64[0] = ToFileTime(st.st_birthtimespec);
            p64[1] = ToFileTime(st.st_atimespec);
            p64[2] = ToFileTime(st.st_mtimespec);
            p64[3] = ToFileTime(st.st_ctimespec);
            p64[4] = uint64_t(st.st_size);
            p64[5] = (uint64_t(st.st_size) + 0xFFF) & ~0xFFFull;
            p32[0x38 / 4] = Attributes(st, true);
            p32[0x3C / 4] = uint32_t(name.size());
            memcpy(info + 0x40, name.data(), name.size());
            CompleteIo(io, STATUS_SUCCESS, needed, event);
            return STATUS_SUCCESS;
        }
        CompleteIo(io, STATUS_NO_MORE_FILES, 0, event);
        return STATUS_NO_MORE_FILES;
    }

    static uint32_t NtFlushBuffersFile(uint32_t handle, IoStatusBlock* io)
    {
        auto file = HandleTo<FileObject>(handle);
        if (!file)
            return STATUS_INVALID_HANDLE;
        if (file->fd >= 0)
            FlushToStorage(file->fd);
        CompleteIo(io, STATUS_SUCCESS, 0, 0);
        return STATUS_SUCCESS;
    }

    static uint32_t ObCreateSymbolicLink(AnsiString* link, AnsiString* target)
    {
        Log("Symbolic link {} -> {}", GuestString(link), GuestString(target));
        CreateSymbolicLink(GuestString(link), GuestString(target));
        return STATUS_SUCCESS;
    }

    static uint32_t ObDeleteSymbolicLink(AnsiString* link)
    {
        DeleteSymbolicLink(GuestString(link));
        return STATUS_SUCCESS;
    }
}

using namespace rt;
GUEST_IMPORT(NtCreateFile, NtCreateFile)
GUEST_IMPORT(NtOpenFile, NtOpenFile)
GUEST_IMPORT(NtReadFile, NtReadFile)
GUEST_IMPORT(NtReadFileScatter, NtReadFileScatter)
GUEST_IMPORT(NtWriteFile, NtWriteFile)
GUEST_IMPORT(NtQueryInformationFile, NtQueryInformationFile)
GUEST_IMPORT(NtSetInformationFile, NtSetInformationFile)
GUEST_IMPORT(NtQueryFullAttributesFile, NtQueryFullAttributesFile)
GUEST_IMPORT(NtQueryVolumeInformationFile, NtQueryVolumeInformationFile)
GUEST_IMPORT(NtQueryDirectoryFile, NtQueryDirectoryFile)
GUEST_IMPORT(NtFlushBuffersFile, NtFlushBuffersFile)
GUEST_IMPORT(ObCreateSymbolicLink, ObCreateSymbolicLink)
GUEST_IMPORT(ObDeleteSymbolicLink, ObDeleteSymbolicLink)
