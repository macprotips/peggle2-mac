// Crash-safe file replacement (see atomic_file.h).
#include "atomic_file.h"
#include <fcntl.h>
#include <unistd.h>

namespace rt
{
    // fsync on macOS only hands the data to the drive, which may still lose it on power loss; F_FULLFSYNC
    // waits until it is on storage (falling back to fsync where the file system does not support it).
    bool FlushToStorage(int fd)
    {
        return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
    }

    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view contents)
    {
        const std::filesystem::path temporary = path.string() + ".tmp";
        const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return false;
        bool ok = true;
        for (size_t done = 0; ok && done < contents.size();)
        {
            const ssize_t n = write(fd, contents.data() + done, contents.size() - done);
            ok = n > 0;
            done += ok ? size_t(n) : 0;
        }
        ok = ok && FlushToStorage(fd);
        ok = close(fd) == 0 && ok;
        std::error_code error;
        if (ok)
            std::filesystem::rename(temporary, path, error);
        if (!ok || error)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }
}
