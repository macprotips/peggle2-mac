// Where the port keeps its files (see folders.h).
#include "folders.h"
#import <Foundation/Foundation.h>
#include <cstdlib>

namespace rt
{
    static std::filesystem::path SystemDirectory(NSSearchPathDirectory directory)
    {
        NSURL* url = [NSFileManager.defaultManager URLForDirectory:directory inDomain:NSUserDomainMask appropriateForURL:nil
                                                            create:YES error:nil];
        return url ? std::filesystem::path(url.fileSystemRepresentation) : std::filesystem::path(getenv("HOME") ?: ".") / "Library";
    }

    std::filesystem::path UserDataDirectory()
    {
        if (const char* override = getenv("PEGGLE2_DATA_DIR"))
            return override;
        const std::filesystem::path support = SystemDirectory(NSApplicationSupportDirectory);
        const std::filesystem::path dir = support / "Peggle 2", previous = support / "Peggle2";
        std::error_code error;
        if (!std::filesystem::exists(dir, error) && std::filesystem::is_directory(previous, error))
            std::filesystem::rename(previous, dir, error); // one step on the same volume: nothing is copied
        std::filesystem::create_directories(dir, error);
        return dir;
    }

    std::filesystem::path CacheDirectory()
    {
        std::error_code error;
        if (getenv("PEGGLE2_DATA_DIR"))
        {
            const std::filesystem::path dir = UserDataDirectory() / "cache";
            std::filesystem::create_directories(dir, error);
            return dir;
        }
        const std::filesystem::path dir = SystemDirectory(NSCachesDirectory) / "io.github.macprotips.peggle2";
        const std::filesystem::path previous = UserDataDirectory() / "cache";
        if (!std::filesystem::exists(dir, error) && std::filesystem::is_directory(previous, error))
            std::filesystem::rename(previous, dir, error);
        std::filesystem::create_directories(dir, error);
        return dir;
    }

    void MigrateSettings()
    {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        NSString* current = NSBundle.mainBundle.bundleIdentifier;
        NSDictionary* previous = [defaults persistentDomainForName:@"local.peggle2mac"];
        if (!current || !previous.count || [defaults persistentDomainForName:current].count)
            return;
        [defaults setPersistentDomain:previous forName:current];
        [defaults removePersistentDomainForName:@"local.peggle2mac"];
    }
}
