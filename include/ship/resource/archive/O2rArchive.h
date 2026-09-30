#pragma once

#undef _DLL

#include <string>
#include <stdint.h>
#include <string>

#include "zip.h"

#include "ship/resource/File.h"
#include "ship/resource/Resource.h"
#include "ship/resource/archive/Archive.h"

#ifdef __WIIU__
#ifndef WIIU_DIAGNOSTICS
#define WIIU_DIAGNOSTICS 1
#endif
#endif

namespace Ship {
struct File;

class O2rArchive final : virtual public Archive {
  public:
    O2rArchive(const std::string& archivePath);
    ~O2rArchive();

    bool Open();
    bool Close();
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& data);

    std::shared_ptr<File> LoadFile(const std::string& filePath);
    std::shared_ptr<File> LoadFile(uint64_t hash);

#ifdef __WIIU__
    static void GetStats(uint32_t& loads, uint64_t& compressedBytes, uint64_t& microseconds);
#if WIIU_DIAGNOSTICS
    struct HitchArchiveStats {
        char name[32]{};
        uint64_t readMicroseconds = 0;
    };

    struct HitchStats {
        uint32_t loads = 0;
        uint64_t compressedBytes = 0;
        uint64_t readMicroseconds = 0;
        uint32_t archiveCount = 0;
        HitchArchiveStats topArchives[3]{};
    };

    static HitchStats GetHitchStats();
#endif
#endif

  private:
    zip_t* mZipArchive;
};
} // namespace Ship
