#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/utils/filesystemtools/FileHelper.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"
#include <fstream>
#include <unordered_map>

namespace Ship {
O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
    mZipArchive = nullptr;
    mZipArchiveSource = nullptr;
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

zip_t* O2rArchive::GetZipHandle() {
    std::lock_guard<std::mutex> lock(mPoolMutex);
    if (!mZipArchivePool.empty()) {
        zip_t* handle = mZipArchivePool.back();
        mZipArchivePool.pop_back();
        return handle;
    }
    return OpenZipFromBuffer(ZIP_RDONLY);
}

zip_t* O2rArchive::OpenZipFromBuffer(int flags, zip_source_t** sourceOut) {
    if (sourceOut != nullptr) {
        *sourceOut = nullptr;
    }

    zip_source_t* source = nullptr;
    if (mZipArchive != nullptr) {
        source = zip_source_buffer(mZipArchive, mArchiveBuffer.data(), mArchiveBuffer.size(), 0);
    } else {
        source = zip_source_buffer_create(mArchiveBuffer.data(), mArchiveBuffer.size(), 0, nullptr);
    }

    if (source == nullptr) {
        return nullptr;
    }

    zip_t* archive = zip_open_from_source(source, flags, nullptr);
    if (archive == nullptr) {
        zip_source_free(source);
    } else if (sourceOut != nullptr) {
        *sourceOut = source;
    }
    return archive;
}

void O2rArchive::ReleaseZipHandle(zip_t* handle) {
    if (handle == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mPoolMutex);
    mZipArchivePool.push_back(handle);
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    zip_t* zipArchive = GetZipHandle();
    if (zipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

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

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    struct zip_file* zipEntryFile = zip_fopen_index(zipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    ReleaseZipHandle(zipArchive);

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

bool O2rArchive::Open() {
    std::ifstream file(GetPath(), std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0) {
        SPDLOG_ERROR("Failed to determine size of zip file \"{}\"", GetPath());
        return false;
    }

    mArchiveBuffer.resize(static_cast<size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (!mArchiveBuffer.empty() &&
        !file.read(reinterpret_cast<char*>(mArchiveBuffer.data()), static_cast<std::streamsize>(mArchiveBuffer.size()))) {
        mArchiveBuffer.clear();
        SPDLOG_ERROR("Failed to read zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = OpenZipFromBuffer(ZIP_CREATE, &mZipArchiveSource);
    if (mZipArchive == nullptr) {
        mArchiveBuffer.clear();
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }
    zip_source_keep(mZipArchiveSource);

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

    if (mZipArchiveSource != nullptr) {
        zip_source_free(mZipArchiveSource);
        mZipArchiveSource = nullptr;
    }
    mArchiveBuffer.clear();
    mArchiveBuffer.shrink_to_fit();

    return success;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    bool success = true;

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

    // Commit changes to the in-memory source.
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        mZipArchive = nullptr;
        if (mZipArchiveSource != nullptr) {
            zip_source_free(mZipArchiveSource);
            mZipArchiveSource = nullptr;
        }
        return false;
    }
    mZipArchive = nullptr;

    // Clear the pool before replacing the shared buffer.
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto* handle : mZipArchivePool) {
            if (zip_close(handle) == -1) {
                success = false;
            }
        }
        mZipArchivePool.clear();
    }

    zip_stat_t sourceStat;
    zip_stat_init(&sourceStat);
    if (mZipArchiveSource == nullptr || zip_source_stat(mZipArchiveSource, &sourceStat) != 0 ||
        sourceStat.size > static_cast<zip_uint64_t>(SIZE_MAX) || zip_source_open(mZipArchiveSource) != 0) {
        SPDLOG_ERROR("Failed to read updated zip archive \"{}\" from memory", GetPath());
        if (mZipArchiveSource != nullptr) {
            zip_source_free(mZipArchiveSource);
            mZipArchiveSource = nullptr;
        }
        return false;
    }

    mArchiveBuffer.resize(static_cast<size_t>(sourceStat.size));
    size_t bytesRead = 0;
    while (bytesRead < mArchiveBuffer.size()) {
        const zip_int64_t read = zip_source_read(mZipArchiveSource, mArchiveBuffer.data() + bytesRead,
                                                 mArchiveBuffer.size() - bytesRead);
        if (read <= 0) {
            zip_source_close(mZipArchiveSource);
            zip_source_free(mZipArchiveSource);
            mZipArchiveSource = nullptr;
            SPDLOG_ERROR("Failed to read updated zip archive \"{}\" from memory", GetPath());
            return false;
        }
        bytesRead += static_cast<size_t>(read);
    }
    if (zip_source_close(mZipArchiveSource) != 0) {
        zip_source_free(mZipArchiveSource);
        mZipArchiveSource = nullptr;
        SPDLOG_ERROR("Failed to close updated zip archive source \"{}\"", GetPath());
        return false;
    }

    zip_source_free(mZipArchiveSource);
    mZipArchiveSource = nullptr;
    FileHelper::WriteAllBytes(GetPath(), mArchiveBuffer);

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip from the refreshed in-memory buffer so reads never return to disk.
    mZipArchive = OpenZipFromBuffer(ZIP_CREATE, &mZipArchiveSource);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }
    zip_source_keep(mZipArchiveSource);

    IndexFile(filePath);

    // Success
    return success;
}

} // namespace Ship
