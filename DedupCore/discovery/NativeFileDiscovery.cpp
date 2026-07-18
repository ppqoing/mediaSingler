#include "NativeFileDiscovery.h"

#include "DiskInfo.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <unordered_set>
#include <vector>

namespace videosc::dedup {
namespace {

/** @brief 严格 UTF-16 到 UTF-8。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        return {};
    }
    return result;
}

/** @brief 严格 UTF-8 到 UTF-16。 */
std::wstring Utf8ToWide(const char* value) {
    if (value == nullptr || value[0] == '\0') return {};
    const int sourceLength = static_cast<int>(std::strlen(value));
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value,
                                           sourceLength,
                                           nullptr,
                                           0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value,
                            sourceLength,
                            result.data(),
                            length) != length) {
        return {};
    }
    return result;
}

/** @brief Windows FILETIME 转 Unix UTC 毫秒。 */
std::int64_t FileTimeToUnixMilliseconds(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000ULL);
}

/** @brief 获取不带通配符的绝对规范显示路径。 */
std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return path;
    buffer.resize(written);
    return std::filesystem::path(buffer).lexically_normal();
}

/** @brief Windows 路径键使用统一分隔符和 invariant 小写。 */
std::wstring NormalizePathKey(const std::filesystem::path& path) {
    std::wstring value = AbsolutePath(path).wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (!value.empty()) {
        const int required = LCMapStringEx(LOCALE_NAME_INVARIANT,
                                           LCMAP_LOWERCASE,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr,
                                           0);
        if (required > 0) {
            std::wstring lowered(static_cast<std::size_t>(required), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT,
                              LCMAP_LOWERCASE,
                              value.data(),
                              static_cast<int>(value.size()),
                              lowered.data(),
                              required,
                              nullptr,
                              nullptr,
                              0) == required) {
                value = std::move(lowered);
            }
        }
    }
    return value;
}

/** @brief 由 FindFirstFileExW 元数据创建路径记录。 */
FilePathRecord BuildRecord(const std::filesystem::path& path,
                           const WIN32_FIND_DATAW& data,
                           const DiscoveryRoot& root,
                           const std::uint64_t scanId) {
    FilePathRecord record;
    record.scan_id = scanId;
    record.path = AbsolutePath(path);
    record.normalized_path_key = NormalizePathKey(record.path);
    record.volume_guid = root.volume_guid;
    record.storage_target_key = root.storage_target_key;
    record.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    record.extension = record.path.extension().wstring();
    std::transform(record.extension.begin(), record.extension.end(), record.extension.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    record.creation_time_utc_ms = FileTimeToUnixMilliseconds(data.ftCreationTime);
    record.last_write_time_utc_ms = FileTimeToUnixMilliseconds(data.ftLastWriteTime);
    record.scan_root_priority = root.priority;
    record.state = FilePathState::Pending;
    record.sync_state = SyncState::LocalOnly;
    return record;
}

/** @brief 为单个文件查询 HDD 调度位置，失败时安全退化为空。 */
std::optional<std::uint64_t> QueryPhysicalStart(const std::filesystem::path& path) {
    const std::string utf8 = WideToUtf8(path.wstring());
    if (utf8.empty()) return std::nullopt;
    FilePhysicalLocation location{};
    if (!QueryFilePhysicalLocation(utf8.c_str(), &location)) return std::nullopt;
    return location.physicalStartByte;
}

}  // namespace

std::optional<DiscoveryRoot> NativeFileDiscovery::PrepareRoot(const std::filesystem::path& path,
                                                              const std::uint32_t priority,
                                                              const bool hdd_extent_optimization,
                                                              std::string& error) {
    const std::filesystem::path absolute = AbsolutePath(path);
    const DWORD attributes = GetFileAttributesW(absolute.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "scan_root_not_found:" + std::to_string(GetLastError());
        return std::nullopt;
    }
    const std::string utf8 = WideToUtf8(absolute.wstring());
    if (utf8.empty()) {
        error = "invalid_scan_root_encoding";
        return std::nullopt;
    }
    DiskTopologyInfo topology{};
    if (!QueryDiskTopology(utf8.c_str(), &topology)) {
        error = "disk_topology_error:" + std::to_string(topology.statusCode) + ":" +
                std::to_string(topology.win32Error);
        return std::nullopt;
    }
    DiscoveryRoot root;
    root.path = absolute;
    root.priority = priority;
    root.volume_guid = Utf8ToWide(topology.volumeGuid);
    root.storage_target_key = Utf8ToWide(topology.storageTargetKey);
    root.media_type = topology.mediaType == DISKINFO_MEDIA_HDD
                          ? StorageMediaType::Hdd
                          : (topology.mediaType == DISKINFO_MEDIA_SSD ? StorageMediaType::Ssd
                                                                      : StorageMediaType::Unknown);
    root.is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    root.query_physical_location = hdd_extent_optimization && root.media_type == StorageMediaType::Hdd;
    error.clear();
    return root;
}

DiscoveryStats NativeFileDiscovery::Enumerate(const DiscoveryRoot& root,
                                              const std::uint64_t scan_id,
                                              const std::atomic_bool& cancel_requested,
                                              const FileVisitor& visitor) {
    DiscoveryStats stats;
    if (!visitor) {
        stats.error = "file_visitor_required";
        return stats;
    }
    const DWORD rootAttributes = GetFileAttributesW(root.path.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
        stats.error = "scan_root_not_found:" + std::to_string(GetLastError());
        return stats;
    }
    std::vector<std::filesystem::path> directories;
    if ((rootAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        directories.push_back(root.path);
    } else {
        WIN32_FIND_DATAW data{};
        HANDLE single = FindFirstFileW(root.path.c_str(), &data);
        if (single == INVALID_HANDLE_VALUE) {
            stats.error = "scan_file_metadata_error:" + std::to_string(GetLastError());
            return stats;
        }
        FindClose(single);
        DiscoveredFile file;
        file.record = BuildRecord(root.path, data, root, scan_id);
        file.media_kind = ClassifyMedia(root.path);
        if (root.query_physical_location) file.physical_start_byte = QueryPhysicalStart(root.path);
        if (visitor(std::move(file))) ++stats.discovered_files;
        return stats;
    }

    while (!directories.empty() && !cancel_requested.load(std::memory_order_relaxed)) {
        const std::filesystem::path directory = std::move(directories.back());
        directories.pop_back();
        ++stats.discovered_directories;
        const std::filesystem::path pattern = directory / L"*";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(),
                                       FindExInfoBasic,
                                       &data,
                                       FindExSearchNameMatch,
                                       nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            ++stats.metadata_errors;
            continue;
        }
        do {
            if (cancel_requested.load(std::memory_order_relaxed)) break;
            if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0) continue;
            const std::filesystem::path child = directory / data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    ++stats.skipped_reparse_points;
                } else {
                    directories.push_back(child);
                }
                continue;
            }
            DiscoveredFile file;
            file.record = BuildRecord(child, data, root, scan_id);
            file.media_kind = ClassifyMedia(child);
            if (root.query_physical_location) file.physical_start_byte = QueryPhysicalStart(child);
            ++stats.discovered_files;
            if (!visitor(std::move(file))) {
                stats.cancelled = true;
                FindClose(find);
                return stats;
            }
        } while (FindNextFileW(find, &data));
        const DWORD findError = GetLastError();
        FindClose(find);
        if (findError != ERROR_NO_MORE_FILES && findError != ERROR_SUCCESS) ++stats.metadata_errors;
    }
    stats.cancelled = cancel_requested.load(std::memory_order_relaxed);
    return stats;
}

MediaKind NativeFileDiscovery::ClassifyMedia(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    static const std::unordered_set<std::wstring> imageExtensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".tif", L".tiff", L".heic", L".avif"};
    static const std::unordered_set<std::wstring> videoExtensions = {
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".flv", L".webm", L".m4v", L".ts", L".mts", L".m2ts"};
    static const std::unordered_set<std::wstring> audioExtensions = {
        L".mp3", L".flac", L".wav", L".aac", L".m4a", L".ogg", L".opus", L".wma", L".ape", L".alac"};
    if (imageExtensions.count(extension) != 0) return MediaKind::Image;
    if (videoExtensions.count(extension) != 0) return MediaKind::Video;
    if (audioExtensions.count(extension) != 0) return MediaKind::Audio;
    return MediaKind::Other;
}

}  // namespace videosc::dedup
