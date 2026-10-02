#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"
#include <unordered_map>

#ifdef __WIIU__
#include "ship/config/ConsoleVariable.h"
#include "port/wiiu/WiiUWatchdog.h"
#include <coreinit/fastmutex.h>
#if WIIU_DIAGNOSTICS
#include <atomic>
#include <coreinit/time.h>
#endif
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <list>
#include <limits>
#include <malloc.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <zlib.h>
#endif

namespace Ship {
#ifdef __WIIU__
namespace {
#if WIIU_DIAGNOSTICS
static std::atomic<uint32_t> sO2rLoadCount{ 0 };
static std::atomic<uint32_t> sO2rRepeatLoads{ 0 };
static std::atomic<uint32_t> sO2rCompressedBytes{ 0 };
static std::atomic<uint32_t> sO2rLoadMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rLocateMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rOpenMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rReadMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rInflateMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rRepeatCompressedBytes{ 0 };
static std::atomic<uint32_t> sO2rHitchLoadCount{ 0 };
static std::atomic<uint32_t> sO2rHitchCompressedBytes{ 0 };
static std::atomic<uint32_t> sO2rHitchReadMicroseconds{ 0 };
static std::atomic<uint32_t> sO2rHitchLoadMicroseconds{ 0 };
static std::unordered_set<std::string> sO2rLoadedPaths;
static OSFastMutex sO2rLoadedPathsMutex;

struct O2rArchiveStats {
    uint32_t loads = 0;
    uint32_t cacheHits = 0;
    uint64_t microseconds = 0;
    uint64_t hitchReadMicroseconds = 0;
};

static std::unordered_map<std::string, O2rArchiveStats> sO2rArchiveStats;
static OSFastMutex sO2rArchiveStatsMutex;
#endif
static OSFastMutex sO2rCacheMutex;
static OSFastMutex sO2rPreloadMutex;

constexpr size_t kO2rCacheMaxBytes = 32 * 1024 * 1024;
constexpr size_t kO2rCacheMaxEntryBytes = 4 * 1024 * 1024;

struct O2rCacheKey {
    const O2rArchive* archive;
    std::string filePath;

    bool operator==(const O2rCacheKey& other) const {
        return archive == other.archive && filePath == other.filePath;
    }
};

struct O2rCacheKeyHash {
    size_t operator()(const O2rCacheKey& key) const {
        const size_t archiveHash = std::hash<const O2rArchive*>{}(key.archive);
        const size_t pathHash = std::hash<std::string>{}(key.filePath);
        return archiveHash ^ (pathHash + 0x9e3779b9 + (archiveHash << 6) + (archiveHash >> 2));
    }
};

struct O2rCacheEntry {
    O2rCacheKey key;
    std::shared_ptr<std::vector<char>> compressedData;
    zip_uint16_t compMethod;
    zip_uint64_t uncompressedSize;
};

using O2rCacheList = std::list<O2rCacheEntry>;
static O2rCacheList sO2rCache;
static std::unordered_map<O2rCacheKey, O2rCacheList::iterator, O2rCacheKeyHash> sO2rCacheIndex;
static size_t sO2rCacheBytes = 0;
#if WIIU_DIAGNOSTICS
static uint32_t sO2rCacheHits = 0;
static uint64_t sO2rCacheHitBytes = 0;
#endif

struct O2rMutexInitializer {
    O2rMutexInitializer() {
#if WIIU_DIAGNOSTICS
        OSFastMutex_Init(&sO2rLoadedPathsMutex, "O2rLoadedPaths");
        OSFastMutex_Init(&sO2rArchiveStatsMutex, "O2rArchiveStats");
#endif
        OSFastMutex_Init(&sO2rCacheMutex, "O2rCache");
        OSFastMutex_Init(&sO2rPreloadMutex, "O2rPreload");
    }
};
static O2rMutexInitializer sO2rMutexInitializer;

static bool O2rCacheLoad(const O2rArchive* archive, const std::string& filePath,
                         std::shared_ptr<std::vector<char>>& compressedData, zip_uint16_t& compMethod,
                         zip_uint64_t& uncompressedSize) {
    OSFastMutex_Lock(&sO2rCacheMutex);
    const O2rCacheKey key{ archive, filePath };
    auto cacheEntry = sO2rCacheIndex.find(key);
    if (cacheEntry == sO2rCacheIndex.end()) {
        OSFastMutex_Unlock(&sO2rCacheMutex);
        return false;
    }

    sO2rCache.splice(sO2rCache.begin(), sO2rCache, cacheEntry->second);
    const auto& entry = sO2rCache.front();
    compressedData = entry.compressedData;
    compMethod = entry.compMethod;
    uncompressedSize = entry.uncompressedSize;
#if WIIU_DIAGNOSTICS
    sO2rCacheHits++;
    sO2rCacheHitBytes += compressedData->size();
#endif
    OSFastMutex_Unlock(&sO2rCacheMutex);
    return true;
}

static void O2rCacheStore(const O2rArchive* archive, const std::string& filePath,
                          const std::shared_ptr<std::vector<char>>& compressedData, zip_uint16_t compMethod,
                          zip_uint64_t uncompressedSize) {
    if (compressedData->size() > kO2rCacheMaxEntryBytes) {
        return;
    }

    OSFastMutex_Lock(&sO2rCacheMutex);
    const O2rCacheKey key{ archive, filePath };
    auto existingEntry = sO2rCacheIndex.find(key);
    if (existingEntry != sO2rCacheIndex.end()) {
        sO2rCacheBytes -= existingEntry->second->compressedData->size();
        sO2rCache.erase(existingEntry->second);
        sO2rCacheIndex.erase(existingEntry);
    }

    while (sO2rCacheBytes + compressedData->size() > kO2rCacheMaxBytes && !sO2rCache.empty()) {
        auto leastRecentlyUsed = std::prev(sO2rCache.end());
        sO2rCacheBytes -= leastRecentlyUsed->compressedData->size();
        sO2rCacheIndex.erase(leastRecentlyUsed->key);
        sO2rCache.pop_back();
    }

    sO2rCache.push_front({ key, compressedData, compMethod, uncompressedSize });
    sO2rCacheIndex.emplace(sO2rCache.front().key, sO2rCache.begin());
    sO2rCacheBytes += compressedData->size();
    OSFastMutex_Unlock(&sO2rCacheMutex);
}

static void O2rCacheClear(const O2rArchive* archive) {
    OSFastMutex_Lock(&sO2rCacheMutex);
    for (auto cacheEntry = sO2rCache.begin(); cacheEntry != sO2rCache.end();) {
        if (cacheEntry->key.archive == archive) {
            sO2rCacheBytes -= cacheEntry->compressedData->size();
            sO2rCacheIndex.erase(cacheEntry->key);
            cacheEntry = sO2rCache.erase(cacheEntry);
        } else {
            ++cacheEntry;
        }
    }
    OSFastMutex_Unlock(&sO2rCacheMutex);
}

#if WIIU_DIAGNOSTICS
static void O2rCacheGetStats(uint32_t& hits, uint64_t& hitBytes, size_t& cacheBytes) {
    OSFastMutex_Lock(&sO2rCacheMutex);
    hits = sO2rCacheHits;
    hitBytes = sO2rCacheHitBytes;
    cacheBytes = sO2rCacheBytes;
    OSFastMutex_Unlock(&sO2rCacheMutex);
}
#endif

// stdio buffer for each open archive. It is ours, not newlib's: newlib allocates it through the
// wrapped malloc (ExpHeap from 64 KiB up) but fclose frees it with _free_r, which corrupted the heap
// with a 128 KiB buffer (soh923p15). 0x40 alignment also lets wut's __wut_fsa_read fill it with one
// FSAReadFile instead of splitting off an unaligned head through its 64-byte bounce buffer.
constexpr size_t kO2rStdioBufferSize = 16 * 1024;
static std::unordered_map<const void*, void*> sO2rStdioBuffers;

struct O2rPreloadBuffer {
    void* data;
    size_t size;
};

static std::unordered_map<const void*, O2rPreloadBuffer> sO2rPreloadBuffers;
static size_t sO2rPreloadedBytes = 0;

static size_t O2rGetPreloadedBytes() {
    OSFastMutex_Lock(&sO2rPreloadMutex);
    const size_t bytes = sO2rPreloadedBytes;
    OSFastMutex_Unlock(&sO2rPreloadMutex);
    return bytes;
}

#if WIIU_DIAGNOSTICS
class O2rLoadTimer {
  public:
    O2rLoadTimer() : mStart(OSGetSystemTime()) {
    }

    ~O2rLoadTimer() {
        Record();
    }

    uint64_t Elapsed() const {
        return OSTicksToMicroseconds(OSGetSystemTime() - mStart);
    }

    void Record() {
        const OSTime now = OSGetSystemTime();
        sO2rLoadMicroseconds.fetch_add(OSTicksToMicroseconds(now - mStart), std::memory_order_relaxed);
        mStart = now;
    }

  private:
    OSTime mStart;
};

static std::string O2rArchiveFileName(O2rArchive& archive) {
    const std::string& path = archive.GetPath();
    const size_t separator = path.find_last_of("/");
    if (separator == std::string::npos || separator + 1 == path.size()) {
        return path;
    }
    return path.substr(separator + 1);
}

struct O2rArchiveReportEntry {
    const std::string* name;
    O2rArchiveStats stats;
};

static bool O2rArchiveReportEntryComesFirst(const std::pair<const std::string, O2rArchiveStats>& archive,
                                            const O2rArchiveReportEntry& entry) {
    return archive.second.loads > entry.stats.loads ||
           (archive.second.loads == entry.stats.loads && archive.first < *entry.name);
}

static void O2rEmitArchiveStats() {
    constexpr size_t kTopArchiveCount = 8;
    constexpr int kArchiveNameLength = 24;
    O2rArchiveReportEntry top[kTopArchiveCount]{};
    size_t topCount = 0;

    OSFastMutex_Lock(&sO2rArchiveStatsMutex);
    for (const auto& archive : sO2rArchiveStats) {
        size_t position = topCount;
        while (position > 0 && O2rArchiveReportEntryComesFirst(archive, top[position - 1])) {
            --position;
        }
        if (position >= kTopArchiveCount) {
            continue;
        }

        if (topCount < kTopArchiveCount) {
            ++topCount;
        }
        for (size_t i = topCount - 1; i > position; --i) {
            top[i] = top[i - 1];
        }
        top[position] = { &archive.first, archive.second };
    }

    char line[400];
    size_t length = static_cast<size_t>(std::snprintf(line, sizeof(line), "O2RARCH:"));
    for (size_t i = 0; i < topCount && length < sizeof(line) - 2; ++i) {
        length += static_cast<size_t>(std::snprintf(
            line + length, sizeof(line) - length, " %.*s=%u/%u", kArchiveNameLength, top[i].name->c_str(),
            top[i].stats.loads, static_cast<uint32_t>(top[i].stats.microseconds / 1000)));
    }
    OSFastMutex_Unlock(&sO2rArchiveStatsMutex);

    // snprintf returns the untruncated length; keep room for the newline and terminator.
    if (length > sizeof(line) - 2) {
        length = sizeof(line) - 2;
    }
    line[length++] = '\n';
    line[length] = '\0';
    Ship::WiiU::Watchdog::Emit("%s", line);
}

struct O2rHitchArchiveEntry {
    const std::string* name;
    uint64_t readMicroseconds;
};

static bool O2rHitchArchiveEntryComesFirst(const std::pair<const std::string, O2rArchiveStats>& archive,
                                           const O2rHitchArchiveEntry& entry) {
    return archive.second.hitchReadMicroseconds > entry.readMicroseconds ||
           (archive.second.hitchReadMicroseconds == entry.readMicroseconds && archive.first < *entry.name);
}
#endif
} // namespace

void O2rArchive::GetStats(uint32_t& loads, uint64_t& compressedBytes, uint64_t& microseconds) {
#if WIIU_DIAGNOSTICS
    loads = sO2rLoadCount.load(std::memory_order_relaxed);
    compressedBytes = sO2rCompressedBytes.load(std::memory_order_relaxed);
    microseconds = sO2rLoadMicroseconds.load(std::memory_order_relaxed);
#else
    loads = 0;
    compressedBytes = 0;
    microseconds = 0;
#endif
}

#if WIIU_DIAGNOSTICS
O2rArchive::HitchStats O2rArchive::GetHitchStats() {
    HitchStats stats;
    O2rHitchArchiveEntry top[3]{};

    OSFastMutex_Lock(&sO2rArchiveStatsMutex);
    stats.loads = sO2rHitchLoadCount.exchange(0, std::memory_order_relaxed);
    stats.compressedBytes = sO2rHitchCompressedBytes.exchange(0, std::memory_order_relaxed);
    stats.readMicroseconds = sO2rHitchReadMicroseconds.exchange(0, std::memory_order_relaxed);
    stats.loadMicroseconds = sO2rHitchLoadMicroseconds.exchange(0, std::memory_order_relaxed);

    for (auto& archive : sO2rArchiveStats) {
        if (archive.second.hitchReadMicroseconds != 0) {
            size_t position = stats.archiveCount;
            while (position > 0 && O2rHitchArchiveEntryComesFirst(archive, top[position - 1])) {
                --position;
            }
            if (position < std::size(top)) {
                if (stats.archiveCount < std::size(top)) {
                    ++stats.archiveCount;
                }
                for (size_t i = stats.archiveCount - 1; i > position; --i) {
                    top[i] = top[i - 1];
                }
                top[position] = { &archive.first, archive.second.hitchReadMicroseconds };
            }
        }
        archive.second.hitchReadMicroseconds = 0;
    }

    for (size_t i = 0; i < stats.archiveCount; ++i) {
        std::snprintf(stats.topArchives[i].name, sizeof(stats.topArchives[i].name), "%s", top[i].name->c_str());
        stats.topArchives[i].readMicroseconds = top[i].readMicroseconds;
    }
    OSFastMutex_Unlock(&sO2rArchiveStatsMutex);
    return stats;
}
#endif
#endif

O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
    mZipArchive = nullptr;
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

zip_t* O2rArchive::GetZipHandle() {
#ifdef __WIIU__
    // Wii U: exactly one file descriptor per archive. The upstream pool opened a fresh, unbuffered zip_open() for
    // every concurrent load (the pool starts empty, so even the first load did) and never closed them. With a full
    // texture pack (~35 archives) that passes newlib's OPEN_MAX of 64 and every later open() fails - SoH develop's
    // saves read as empty and its json parse_error terminated the game (2026-09-29). The unbuffered open also
    // parses the central directory one FSA read per entry (oot.o2r ~23 s vs 0.7 s buffered). So lend out the
    // buffered handle Open() created and make concurrent loaders wait for it; a zip_t is not thread-safe anyway.
    std::unique_lock<std::mutex> lock(mPoolMutex);
    mHandleCv.wait(lock, [this] { return !mHandleBusy || mZipArchive == nullptr; });
    if (mZipArchive == nullptr) {
        return nullptr;
    }
    mHandleBusy = true;
    return mZipArchive;
#else
    std::lock_guard<std::mutex> lock(mPoolMutex);
    if (!mZipArchivePool.empty()) {
        zip_t* handle = mZipArchivePool.back();
        mZipArchivePool.pop_back();
        return handle;
    }
    return zip_open(GetPath().c_str(), ZIP_RDONLY, nullptr);
#endif
}

void O2rArchive::ReleaseZipHandle(zip_t* handle) {
    if (handle == nullptr) {
        return;
    }
#ifdef __WIIU__
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        mHandleBusy = false;
    }
    mHandleCv.notify_one();
    return;
#endif

    std::lock_guard<std::mutex> lock(mPoolMutex);
    mZipArchivePool.push_back(handle);
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
#ifdef __WIIU__
#if WIIU_DIAGNOSTICS
    O2rLoadTimer loadTimer;
    uint64_t hitchReadMicroseconds = 0;
#endif
#endif

    zip_t* zipArchive = GetZipHandle();
    if (zipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

#ifdef __WIIU__
#if WIIU_DIAGNOSTICS
    const OSTime locateStart = OSGetSystemTime();
#endif
#endif
    auto zipEntryIndex = zip_name_locate(zipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(zipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }
#ifdef __WIIU__
#if WIIU_DIAGNOSTICS
    sO2rLocateMicroseconds.fetch_add(OSTicksToMicroseconds(OSGetSystemTime() - locateStart),
                                     std::memory_order_relaxed);
#endif
#endif

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

#ifdef __WIIU__
    const bool readCompressed =
        zipEntryStat.comp_method == ZIP_CM_STORE || zipEntryStat.comp_method == ZIP_CM_DEFLATE;
    std::shared_ptr<std::vector<char>> compressedData;
    bool cacheHit = false;
    if (readCompressed) {
        cacheHit = O2rCacheLoad(this, filePath, compressedData, zipEntryStat.comp_method, zipEntryStat.size);
        if (cacheHit) {
            zipEntryStat.comp_size = compressedData->size();
        }
    }

    struct zip_file* zipEntryFile = nullptr;
    if (!cacheHit) {
#if WIIU_DIAGNOSTICS
        const OSTime openStart = OSGetSystemTime();
#endif
        zipEntryFile = zip_fopen_index(zipArchive, zipEntryIndex, readCompressed ? ZIP_FL_COMPRESSED : 0);
#if WIIU_DIAGNOSTICS
        sO2rOpenMicroseconds.fetch_add(OSTicksToMicroseconds(OSGetSystemTime() - openStart),
                                       std::memory_order_relaxed);
#endif
    }
#else
    struct zip_file* zipEntryFile = zip_fopen_index(zipArchive, zipEntryIndex, 0);
#endif
    if (!zipEntryFile
#ifdef __WIIU__
        && !cacheHit
#endif
    ) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

#ifdef __WIIU__
    if (readCompressed && (zipEntryStat.comp_size > std::numeric_limits<uInt>::max() ||
                           zipEntryStat.size > std::numeric_limits<uInt>::max())) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
        if (!cacheHit) {
            zip_fclose(zipEntryFile);
        }
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }
#endif

    auto fileToLoad = std::make_shared<File>();

#ifdef __WIIU__
    if (readCompressed) {
        if (!cacheHit) {
            compressedData = std::make_shared<std::vector<char>>(zipEntryStat.comp_size);
#if WIIU_DIAGNOSTICS
            const OSTime readStart = OSGetSystemTime();
#endif
            const zip_int64_t bytesRead = zip_fread(zipEntryFile, compressedData->data(), zipEntryStat.comp_size);
#if WIIU_DIAGNOSTICS
            const uint64_t readMicroseconds = OSTicksToMicroseconds(OSGetSystemTime() - readStart);
            sO2rReadMicroseconds.fetch_add(readMicroseconds, std::memory_order_relaxed);
            hitchReadMicroseconds += readMicroseconds;
#endif
            if (bytesRead != static_cast<zip_int64_t>(zipEntryStat.comp_size)) {
                SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
                zip_fclose(zipEntryFile);
                ReleaseZipHandle(zipArchive);
                return nullptr;
            }
            O2rCacheStore(this, filePath, compressedData, zipEntryStat.comp_method, zipEntryStat.size);
        }

        zip_uint64_t outputSize = 0;
#if WIIU_DIAGNOSTICS
        const OSTime inflateStart = OSGetSystemTime();
#endif
        if (zipEntryStat.comp_method == ZIP_CM_STORE) {
            outputSize = zipEntryStat.comp_size;
            if (outputSize == zipEntryStat.size) {
                fileToLoad->Buffer = compressedData;
            }
        } else {
            fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);
            z_stream stream{};
            stream.next_in = reinterpret_cast<Bytef*>(compressedData->data());
            stream.avail_in = static_cast<uInt>(compressedData->size());
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
#if WIIU_DIAGNOSTICS
        sO2rInflateMicroseconds.fetch_add(OSTicksToMicroseconds(OSGetSystemTime() - inflateStart),
                                          std::memory_order_relaxed);
#endif

        if (outputSize != zipEntryStat.size) {
            SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
            if (!cacheHit) {
                zip_fclose(zipEntryFile);
            }
            ReleaseZipHandle(zipArchive);
            return nullptr;
        }
    } else {
        fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);
#if WIIU_DIAGNOSTICS
        const OSTime readStart = OSGetSystemTime();
#endif
#endif
    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }
#ifdef __WIIU__
#if WIIU_DIAGNOSTICS
    const uint64_t readMicroseconds = OSTicksToMicroseconds(OSGetSystemTime() - readStart);
    sO2rReadMicroseconds.fetch_add(readMicroseconds, std::memory_order_relaxed);
    hitchReadMicroseconds += readMicroseconds;
#endif
    }
#endif

    if (
#ifdef __WIIU__
        !cacheHit &&
#endif
        zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
#ifdef __WIIU__
        if (readCompressed && !cacheHit) {
            ReleaseZipHandle(zipArchive);
            return nullptr;
        }
#endif
    }

    ReleaseZipHandle(zipArchive);

    fileToLoad->IsLoaded = true;

#ifdef __WIIU__
#if WIIU_DIAGNOSTICS
    const uint64_t loadMicroseconds = loadTimer.Elapsed();
    const uint32_t loadCount = sO2rLoadCount.fetch_add(1, std::memory_order_relaxed) + 1;
    sO2rCompressedBytes.fetch_add(zipEntryStat.comp_size, std::memory_order_relaxed);

    const std::string archiveName = O2rArchiveFileName(*this);
    OSFastMutex_Lock(&sO2rArchiveStatsMutex);
    sO2rHitchLoadCount.fetch_add(1, std::memory_order_relaxed);
    sO2rHitchCompressedBytes.fetch_add(zipEntryStat.comp_size, std::memory_order_relaxed);
    sO2rHitchReadMicroseconds.fetch_add(hitchReadMicroseconds, std::memory_order_relaxed);
    sO2rHitchLoadMicroseconds.fetch_add(loadMicroseconds, std::memory_order_relaxed);
    auto& archiveStats = sO2rArchiveStats[archiveName];
    archiveStats.loads++;
    archiveStats.cacheHits += cacheHit ? 1 : 0;
    archiveStats.microseconds += loadMicroseconds;
    archiveStats.hitchReadMicroseconds += hitchReadMicroseconds;
    OSFastMutex_Unlock(&sO2rArchiveStatsMutex);

    OSFastMutex_Lock(&sO2rLoadedPathsMutex);
    const bool repeatLoad = !sO2rLoadedPaths.insert(filePath).second;
    if (repeatLoad) {
        sO2rRepeatLoads.fetch_add(1, std::memory_order_relaxed);
        sO2rRepeatCompressedBytes.fetch_add(zipEntryStat.comp_size, std::memory_order_relaxed);
    }
    OSFastMutex_Unlock(&sO2rLoadedPathsMutex);

    if ((loadCount % 256) == 0) {
        uint32_t cacheHits;
        uint64_t cacheHitBytes;
        size_t cacheBytes;
        O2rCacheGetStats(cacheHits, cacheHitBytes, cacheBytes);
        loadTimer.Record();
        Ship::WiiU::Watchdog::Emit(
            "O2R: loads=%u repeats=%u repeatKB=%u hits=%u hitKB=%u cacheKB=%u locateMs=%u openMs=%u readMs=%u "
            "inflateMs=%u totalMs=%u preloadKB=%u\n",
            loadCount, sO2rRepeatLoads.load(std::memory_order_relaxed),
            static_cast<uint32_t>(sO2rRepeatCompressedBytes.load(std::memory_order_relaxed) / 1024), cacheHits,
            static_cast<uint32_t>(cacheHitBytes / 1024), static_cast<uint32_t>(cacheBytes / 1024),
            static_cast<uint32_t>(sO2rLocateMicroseconds.load(std::memory_order_relaxed) / 1000),
            static_cast<uint32_t>(sO2rOpenMicroseconds.load(std::memory_order_relaxed) / 1000),
            static_cast<uint32_t>(sO2rReadMicroseconds.load(std::memory_order_relaxed) / 1000),
            static_cast<uint32_t>(sO2rInflateMicroseconds.load(std::memory_order_relaxed) / 1000),
            static_cast<uint32_t>(sO2rLoadMicroseconds.load(std::memory_order_relaxed) / 1000),
            static_cast<uint32_t>(O2rGetPreloadedBytes() / 1024));
        O2rEmitArchiveStats();
    }
#endif
#endif

    return fileToLoad;
}

// Archives are opened before Context::InitConsoleVariables (SoH's OTR version probe), so the
// console variables may not exist yet: fall back to the default then instead of dereferencing null.
static int32_t O2rPreloadCVar(const char* name, int32_t defaultValue) {
    auto context = Context::GetRawInstance();
    if (context == nullptr || context->GetConsoleVariables() == nullptr) {
        return defaultValue;
    }
    return context->GetConsoleVariables()->GetInteger(name, defaultValue);
}

// The version probe opens and closes every archive before the console variables exist; preloading
// then reads each small archive into RAM only to free it again (and gWiiU.O2rPreloadBudgetMB 0
// cannot turn that off), so preload only once the console variables are up.
static bool O2rPreloadCVarsReady() {
    auto context = Context::GetRawInstance();
    return context != nullptr && context->GetConsoleVariables() != nullptr;
}

bool O2rArchive::Open() {
#ifdef __WIIU__
    bool preloaded = false;
    bool preloadReserved = false;
    void* preloadBuffer = nullptr;
    size_t preloadSize = 0;
    const char* preloadSkipReason = "unknown";
#if WIIU_DIAGNOSTICS
    uint32_t preloadMilliseconds = 0;
#endif

    auto releasePreloadReservation = [&]() {
        if (!preloadReserved) {
            return;
        }

        OSFastMutex_Lock(&sO2rPreloadMutex);
        sO2rPreloadedBytes -= preloadSize;
        OSFastMutex_Unlock(&sO2rPreloadMutex);
        preloadReserved = false;
    };

    struct stat archiveStat;
    if (stat(GetPath().c_str(), &archiveStat) != 0 || archiveStat.st_size < 0) {
        preloadSkipReason = "size";
    } else {
        const uint64_t archiveSize64 = static_cast<uint64_t>(archiveStat.st_size);
        if (archiveSize64 > std::numeric_limits<size_t>::max()) {
            preloadSkipReason = "size";
        } else {
            const size_t archiveSize = static_cast<size_t>(archiveSize64);
            const int32_t maxArchiveMB =
                O2rPreloadCVar("gWiiU.O2rPreloadMaxArchiveMB", 16);
            const int32_t budgetMB =
                O2rPreloadCVar("gWiiU.O2rPreloadBudgetMB", 64);
            const uint64_t maxArchiveBytes = maxArchiveMB > 0
                                                  ? static_cast<uint64_t>(maxArchiveMB) * 1024 * 1024
                                                  : 0;
            const uint64_t budgetBytes = budgetMB > 0 ? static_cast<uint64_t>(budgetMB) * 1024 * 1024 : 0;

            if (!O2rPreloadCVarsReady()) {
                preloadSkipReason = "pre-init";
            } else if (budgetMB <= 0) {
                preloadSkipReason = "disabled";
            } else if (archiveSize64 == 0) {
                preloadSkipReason = "empty";
            } else if (archiveSize64 > maxArchiveBytes) {
                preloadSkipReason = "too-large";
            } else {
                preloadSize = archiveSize;
                preloadBuffer = memalign(0x40, preloadSize);
                if (preloadBuffer == nullptr) {
                    preloadSkipReason = "alloc";
                } else {
                    OSFastMutex_Lock(&sO2rPreloadMutex);
                    const bool withinBudget = static_cast<uint64_t>(sO2rPreloadedBytes) <= budgetBytes &&
                                              archiveSize64 <= budgetBytes - sO2rPreloadedBytes;
                    if (withinBudget) {
                        sO2rPreloadedBytes += preloadSize;
                        preloadReserved = true;
                    }
                    OSFastMutex_Unlock(&sO2rPreloadMutex);

                    if (!withinBudget) {
                        preloadSkipReason = "budget";
                        free(preloadBuffer);
                        preloadBuffer = nullptr;
                    } else {
                        FILE* preloadFile = std::fopen(GetPath().c_str(), "rb");
                        if (preloadFile == nullptr) {
                            preloadSkipReason = "open";
                            releasePreloadReservation();
                            free(preloadBuffer);
                            preloadBuffer = nullptr;
                        } else {
#if WIIU_DIAGNOSTICS
                            const OSTime preloadStart = OSGetSystemTime();
#endif
                            // Unbuffered: newlib then reads straight into preloadBuffer in one call; a buffered
                            // stream refills its small default buffer chunk by chunk (thousands of FSA reads at
                            // ~1.5 ms each for a 16 MB archive). _IONBF allocates nothing (no setvbuf/malloc trap).
                            std::setvbuf(preloadFile, nullptr, _IONBF, 0);
                            const size_t bytesRead = std::fread(preloadBuffer, 1, preloadSize, preloadFile);
                            std::fclose(preloadFile);
#if WIIU_DIAGNOSTICS
                            preloadMilliseconds = static_cast<uint32_t>(
                                OSTicksToMicroseconds(OSGetSystemTime() - preloadStart) / 1000);
#endif
                            if (bytesRead != preloadSize) {
                                preloadSkipReason = "read";
                                releasePreloadReservation();
                                free(preloadBuffer);
                                preloadBuffer = nullptr;
                            } else {
                                zip_error_t zipError;
                                zip_error_init(&zipError);
                                zip_source_t* source =
                                    zip_source_buffer_create(preloadBuffer, preloadSize, 0, &zipError);
                                if (source != nullptr) {
                                    mZipArchive = zip_open_from_source(source, ZIP_RDONLY, &zipError);
                                    if (mZipArchive != nullptr) {
                                        zip_error_fini(&zipError);
                                        OSFastMutex_Lock(&sO2rPreloadMutex);
                                        sO2rPreloadBuffers[this] = { preloadBuffer, preloadSize };
                                        OSFastMutex_Unlock(&sO2rPreloadMutex);
                                        preloadBuffer = nullptr;
                                        preloadReserved = false;
                                        preloaded = true;
                                    } else {
                                        zip_source_free(source);
                                        zip_error_fini(&zipError);
                                        preloadSkipReason = "zip-source";
                                        releasePreloadReservation();
                                        free(preloadBuffer);
                                        preloadBuffer = nullptr;
                                    }
                                } else {
                                    zip_error_fini(&zipError);
                                    preloadSkipReason = "zip-source";
                                    releasePreloadReservation();
                                    free(preloadBuffer);
                                    preloadBuffer = nullptr;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (!preloaded) {
        FILE* archiveFile = std::fopen(GetPath().c_str(), "rb");
        if (archiveFile == nullptr) {
            SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
            return false;
        }

        // Unbuffered made zip_open parse the central directory with one FSA read per entry (oot.o2r:
        // ~23 s instead of 0.7 s, soh923p12). 16 KiB lets one read cover a small entry's local header
        // and its data, without paying a large transfer per random-access load. HD texture packs have
        // entries larger than that (MM Reloaded: median 34 KiB compressed), so each load costs several
        // SD requests; gWiiU.O2rStdioBufferKB (4..1024, default 16) sizes the buffer for measuring that.
        // Always memalign'd: a large buffer from setvbuf(nullptr) faults in _free_r at fclose.
        const size_t stdioBufferSize =
            static_cast<size_t>(std::clamp(O2rPreloadCVar("gWiiU.O2rStdioBufferKB",
                                                          static_cast<int32_t>(kO2rStdioBufferSize / 1024)),
                                           4, 1024)) *
            1024;
        void* stdioBuffer = memalign(0x40, stdioBufferSize);
        if (stdioBuffer == nullptr ||
            std::setvbuf(archiveFile, static_cast<char*>(stdioBuffer), _IOFBF, stdioBufferSize) != 0) {
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
    }

#if WIIU_DIAGNOSTICS
    if (preloaded) {
        const size_t totalBytes = O2rGetPreloadedBytes();
        Ship::WiiU::Watchdog::Emit("O2RPRELOAD: name=%s bytes=%u ms=%u total=%u\n", O2rArchiveFileName(*this).c_str(),
                                   static_cast<uint32_t>(preloadSize), preloadMilliseconds,
                                   static_cast<uint32_t>(totalBytes / (1024 * 1024)));
    } else {
        Ship::WiiU::Watchdog::Emit("O2RPRELOAD: name=%s skip reason=%s\n", O2rArchiveFileName(*this).c_str(),
                                   preloadSkipReason);
    }
#else
    (void)preloadSkipReason;
#endif
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
    bool success = true;

    if (mZipArchive != nullptr) {
        if (zip_close(mZipArchive) == -1) {
            SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
            success = false;
        }
        mZipArchive = nullptr;
    }

    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (auto* handle : mZipArchivePool) {
        if (zip_close(handle) == -1) {
            SPDLOG_ERROR("Failed to close pooled zip file \"{}\"", GetPath());
            success = false;
        }
    }
    mZipArchivePool.clear();

    mZipArchive = nullptr;
#ifdef __WIIU__
    O2rCacheClear(this);
    // zip_close has fclose'd the FILE, so its buffer is no longer referenced.
    auto stdioBuffer = sO2rStdioBuffers.find(this);
    if (stdioBuffer != sO2rStdioBuffers.end()) {
        free(stdioBuffer->second);
        sO2rStdioBuffers.erase(stdioBuffer);
    }

    OSFastMutex_Lock(&sO2rPreloadMutex);
    auto preloadBuffer = sO2rPreloadBuffers.find(this);
    if (preloadBuffer != sO2rPreloadBuffers.end()) {
        free(preloadBuffer->second.data);
        sO2rPreloadedBytes -= preloadBuffer->second.size;
        sO2rPreloadBuffers.erase(preloadBuffer);
    }
    OSFastMutex_Unlock(&sO2rPreloadMutex);
#endif
    return success;
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

    // Clear the pool as the file on disk has likely changed
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto* handle : mZipArchivePool) {
            zip_close(handle);
        }
        mZipArchivePool.clear();
    }
#ifdef __WIIU__
    O2rCacheClear(this);
    auto stdioBuffer = sO2rStdioBuffers.find(this);
    if (stdioBuffer != sO2rStdioBuffers.end()) {
        free(stdioBuffer->second);
        sO2rStdioBuffers.erase(stdioBuffer);
    }
#endif
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
