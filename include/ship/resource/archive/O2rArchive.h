#pragma once

#undef _DLL

#include <string>
#include <stdint.h>
#include <string>
#include <mutex>
#ifdef __WIIU__
#include <condition_variable>
#endif
#include <vector>

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

/**
 * @brief Archive implementation backed by a ZIP file (.o2r format).
 *
 * O2rArchive uses libzip to expose a Ship::Archive over a standard ZIP container.
 * The ".o2r" format is the successor to the MPQ-based ".otr" format and requires
 * no optional build flags to enable.
 *
 * To improve concurrent read throughput a pool of `zip_t*` handles is maintained
 * internally; reads acquire a handle from the pool and return it when done.
 */
class O2rArchive final : virtual public Archive {
  public:
    /**
     * @brief Constructs an O2rArchive for the given ZIP file path.
     * @param archivePath Absolute or relative filesystem path to the ".o2r" file.
     */
    O2rArchive(const std::string& archivePath);
    ~O2rArchive();

    /**
     * @brief Opens the ZIP file via libzip, creating the internal handle pool.
     * @return true if the file was opened successfully.
     */
    bool Open();

    /**
     * @brief Closes all pooled handles and releases libzip resources.
     * @return true on success.
     */
    bool Close();

    /**
     * @brief Writes raw data to a file entry inside the ZIP archive.
     * @param filename Target path within the archive.
     * @param data     Raw bytes to write.
     * @return true on success.
     */
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& data);

    /**
     * @brief Loads a file from the archive by its virtual path.
     * @param filePath Virtual path of the file within the ZIP.
     * @return Loaded File with a populated buffer, or nullptr if not found.
     */
    std::shared_ptr<File> LoadFile(const std::string& filePath);

    /**
     * @brief Loads a file from the archive by its 64-bit hash.
     * @param hash CRC/hash of the file path.
     * @return Loaded File with a populated buffer, or nullptr if not found.
     */
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
        uint64_t loadMicroseconds = 0;
        uint32_t archiveCount = 0;
        HitchArchiveStats topArchives[3]{};
    };

    static HitchStats GetHitchStats();
#endif
#endif

  private:
    /** @brief Acquires a zip_t* handle from the pool, opening a new one if the pool is empty. */
    zip_t* GetZipHandle();
    /** @brief Returns a zip_t* handle back to the pool for reuse. */
    void ReleaseZipHandle(zip_t* handle);
    zip_t* mZipArchive;
    std::mutex mPoolMutex;
    std::vector<zip_t*> mZipArchivePool;
#ifdef __WIIU__
    /** @brief Wii U: the single buffered handle from Open() is lent out; concurrent loaders wait for it. */
    std::condition_variable mHandleCv;
    bool mHandleBusy = false;

    struct ExactReadEntry {
        uint32_t localHeaderOffset;
        uint32_t compressedSize;
        uint32_t uncompressedSize;
        uint16_t method;
        uint16_t nameLength;
        uint16_t extraLength;
    };

    bool PrepareExactReads();
    bool LoadExactFile(size_t entryIndex, File& file, uint64_t& hitchReadMicroseconds,
                       std::shared_ptr<std::vector<char>>& compressedOut);
    void CloseExactReads();

    int mExactReadFd = -1;
    std::vector<ExactReadEntry> mExactReadEntries;
    std::mutex mExactReadMutex;
#endif
};
} // namespace Ship
