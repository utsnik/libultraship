#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __WIIU__
#include "port/wiiu/WiiUWatchdog.h"
#include <coreinit/fastmutex.h>
#include <coreinit/time.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <unordered_map>
#include <unordered_set>
#include <zlib.h>
#endif

namespace Ship {
#ifdef __WIIU__
namespace {
static uint32_t sO2rLoadCount = 0;
static uint32_t sO2rRepeatLoads = 0;
static uint64_t sO2rCompressedBytes = 0;
static uint64_t sO2rLoadMicroseconds = 0;
static uint64_t sO2rLocateMicroseconds = 0;
static uint64_t sO2rOpenMicroseconds = 0;
static uint64_t sO2rReadMicroseconds = 0;
static uint64_t sO2rInflateMicroseconds = 0;
static uint64_t sO2rRepeatCompressedBytes = 0;
static std::unordered_set<std::string> sO2rLoadedPaths;
static OSFastMutex sO2rLoadedPathsMutex;

struct O2rMutexInitializer {
    O2rMutexInitializer() { OSFastMutex_Init(&sO2rLoadedPathsMutex, "O2rLoadedPaths"); }
};
static O2rMutexInitializer sO2rMutexInitializer;

// stdio buffer for each open archive. It is ours, not newlib's: newlib allocates it through the
// wrapped malloc (ExpHeap from 64 KiB up) but fclose frees it with _free_r, which corrupted the heap
// with a 128 KiB buffer (soh923p15). 0x40 alignment also lets wut's __wut_fsa_read fill it with one
// FSAReadFile instead of splitting off an unaligned head through its 64-byte bounce buffer.
constexpr size_t kO2rStdioBufferSize = 4 * 1024;
static std::unordered_map<const void*, void*> sO2rStdioBuffers;

class O2rLoadTimer {
  public:
    O2rLoadTimer() : mStart(OSGetSystemTime()) {
    }

    ~O2rLoadTimer() {
        Record();
    }

    void Record() {
        const OSTime now = OSGetSystemTime();
        sO2rLoadMicroseconds += OSTicksToMicroseconds(now - mStart);
        mStart = now;
    }

  private:
    OSTime mStart;
};
} // namespace

void O2rArchive::GetStats(uint32_t& loads, uint64_t& compressedBytes, uint64_t& microseconds) {
    loads = sO2rLoadCount;
    compressedBytes = sO2rCompressedBytes;
    microseconds = sO2rLoadMicroseconds;
}
#endif

O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
#ifdef __WIIU__
    O2rLoadTimer loadTimer;
#endif

    if (mZipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

#ifdef __WIIU__
    const OSTime locateStart = OSGetSystemTime();
#endif
    auto zipEntryIndex = zip_name_locate(mZipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(mZipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }
#ifdef __WIIU__
    sO2rLocateMicroseconds += OSTicksToMicroseconds(OSGetSystemTime() - locateStart);
#endif

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        return nullptr;
    }

#ifdef __WIIU__
    const bool readCompressed =
        zipEntryStat.comp_method == ZIP_CM_STORE || zipEntryStat.comp_method == ZIP_CM_DEFLATE;
    const OSTime openStart = OSGetSystemTime();
    struct zip_file* zipEntryFile =
        zip_fopen_index(mZipArchive, zipEntryIndex, readCompressed ? ZIP_FL_COMPRESSED : 0);
    sO2rOpenMicroseconds += OSTicksToMicroseconds(OSGetSystemTime() - openStart);
#else
    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
#endif
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

#ifdef __WIIU__
    if (readCompressed && (zipEntryStat.comp_size > std::numeric_limits<uInt>::max() ||
                           zipEntryStat.size > std::numeric_limits<uInt>::max())) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
        zip_fclose(zipEntryFile);
        return nullptr;
    }
#endif

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

#ifdef __WIIU__
    if (readCompressed) {
        std::vector<char> compressedData(zipEntryStat.comp_size);
        const OSTime readStart = OSGetSystemTime();
        const zip_int64_t bytesRead = zip_fread(zipEntryFile, compressedData.data(), zipEntryStat.comp_size);
        sO2rReadMicroseconds += OSTicksToMicroseconds(OSGetSystemTime() - readStart);
        if (bytesRead != static_cast<zip_int64_t>(zipEntryStat.comp_size)) {
            SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
            zip_fclose(zipEntryFile);
            return nullptr;
        }

        zip_uint64_t outputSize = 0;
        const OSTime inflateStart = OSGetSystemTime();
        if (zipEntryStat.comp_method == ZIP_CM_STORE) {
            outputSize = zipEntryStat.comp_size;
            if (outputSize == zipEntryStat.size) {
                std::memcpy(fileToLoad->Buffer->data(), compressedData.data(), outputSize);
            }
        } else {
            z_stream stream{};
            stream.next_in = reinterpret_cast<Bytef*>(compressedData.data());
            stream.avail_in = static_cast<uInt>(compressedData.size());
            stream.next_out = reinterpret_cast<Bytef*>(fileToLoad->Buffer->data());
            stream.avail_out = static_cast<uInt>(fileToLoad->Buffer->size());

            if (inflateInit2(&stream, -MAX_WBITS) == Z_OK) {
                const int inflateResult = inflate(&stream, Z_FINISH);
                outputSize = stream.total_out;
                const int inflateEndResult = inflateEnd(&stream);
                if (inflateResult != Z_STREAM_END || inflateEndResult != Z_OK) {
                    outputSize = 0;
                }
            }
        }
        sO2rInflateMicroseconds += OSTicksToMicroseconds(OSGetSystemTime() - inflateStart);

        if (outputSize != zipEntryStat.size) {
            SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
            zip_fclose(zipEntryFile);
            return nullptr;
        }
    } else {
        const OSTime readStart = OSGetSystemTime();
#endif
    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
#ifdef __WIIU__
    sO2rReadMicroseconds += OSTicksToMicroseconds(OSGetSystemTime() - readStart);
    }
#endif

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
#ifdef __WIIU__
        if (readCompressed) {
            return nullptr;
        }
#endif
    }

    fileToLoad->IsLoaded = true;

#ifdef __WIIU__
    sO2rLoadCount++;
    sO2rCompressedBytes += zipEntryStat.comp_size;

    OSFastMutex_Lock(&sO2rLoadedPathsMutex);
    const bool repeatLoad = !sO2rLoadedPaths.insert(filePath).second;
    if (repeatLoad) {
        sO2rRepeatLoads++;
        sO2rRepeatCompressedBytes += zipEntryStat.comp_size;
    }
    OSFastMutex_Unlock(&sO2rLoadedPathsMutex);

    if ((sO2rLoadCount % 256) == 0) {
        loadTimer.Record();
        Ship::WiiU::Watchdog::Emit(
            "O2R: loads=%u repeats=%u repeatKB=%u locateMs=%u openMs=%u readMs=%u inflateMs=%u totalMs=%u\n",
            sO2rLoadCount, sO2rRepeatLoads, static_cast<uint32_t>(sO2rRepeatCompressedBytes / 1024),
            static_cast<uint32_t>(sO2rLocateMicroseconds / 1000), static_cast<uint32_t>(sO2rOpenMicroseconds / 1000),
            static_cast<uint32_t>(sO2rReadMicroseconds / 1000), static_cast<uint32_t>(sO2rInflateMicroseconds / 1000),
            static_cast<uint32_t>(sO2rLoadMicroseconds / 1000));
    }
#endif

    return fileToLoad;
}

bool O2rArchive::Open() {
#ifdef __WIIU__
    FILE* archiveFile = std::fopen(GetPath().c_str(), "rb");
    if (archiveFile == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    // Unbuffered made zip_open parse the central directory with one FSA read per entry (oot.o2r:
    // ~23 s instead of 0.7 s, soh923p12). 16 KiB lets one read cover a small entry's local header
    // and its data, without paying a large transfer per random-access load.
    void* stdioBuffer = memalign(0x40, kO2rStdioBufferSize);
    if (stdioBuffer == nullptr ||
        std::setvbuf(archiveFile, static_cast<char*>(stdioBuffer), _IOFBF, kO2rStdioBufferSize) != 0) {
        std::fclose(archiveFile);
        free(stdioBuffer);
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    zip_error_t zipError;
    zip_error_init(&zipError);
    zip_source_t* source = zip_source_filep_create(archiveFile, 0, ZIP_LENGTH_TO_END, &zipError);
    if (source == nullptr) {
        std::fclose(archiveFile);
        free(stdioBuffer);
        zip_error_fini(&zipError);
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = zip_open_from_source(source, ZIP_RDONLY, &zipError);
    if (mZipArchive == nullptr) {
        zip_source_free(source); // also closes archiveFile
        free(stdioBuffer);
        zip_error_fini(&zipError);
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }
    zip_error_fini(&zipError);
    sO2rStdioBuffers[this] = stdioBuffer;
#else
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }
#endif

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Cannot close zip file. Zip file not loaded. \"{}\"", GetPath());
        return false;
    }

    if (zip_close(mZipArchive) == -1) {
        SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = nullptr;
#ifdef __WIIU__
    // zip_close has fclose'd the FILE, so its buffer is no longer referenced.
    auto stdioBuffer = sO2rStdioBuffers.find(this);
    if (stdioBuffer != sO2rStdioBuffers.end()) {
        free(stdioBuffer->second);
        sO2rStdioBuffers.erase(stdioBuffer);
    }
#endif
    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        return false;
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
}

} // namespace Ship
