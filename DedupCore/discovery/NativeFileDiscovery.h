#pragma once

#include "../models/CoreModels.h"
#include "../config/AppConfig.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace videosc::dedup {

/** @brief 已解析到单一存储目标的扫描根。 */
struct DiscoveryRoot {
    std::filesystem::path path;
    std::uint32_t priority = 0;
    std::wstring volume_guid;
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    /** @brief 扫描根是否为目录；false 表示单文件扫描根。 */
    bool is_directory = true;
    bool query_physical_location = false;
};

/** @brief 文件发现阶段的单条流式结果。 */
struct DiscoveredFile {
    FilePathRecord record;
    MediaKind media_kind = MediaKind::Other;
    std::optional<std::uint64_t> physical_start_byte;
};

/** @brief 单个扫描根的有界统计。 */
struct DiscoveryStats {
    std::uint64_t discovered_files = 0;
    std::uint64_t discovered_directories = 0;
    std::uint64_t skipped_reparse_points = 0;
    std::uint64_t metadata_errors = 0;
    bool cancelled = false;
    std::string error;
};

/**
 * @brief 基于 FindFirstFileExW LARGE_FETCH 的流式本地文件发现器。
 *
 * 只在内存保留待遍历目录栈，不保留全量文件列表；重叠路径由上层 RocksDB 路径键幂等去重。
 * 目录重解析点不递归，避免符号链接环和跨卷误分配。
 */
class NativeFileDiscovery final {
public:
    using FileVisitor = std::function<bool(DiscoveredFile&& file)>;

    /** @brief 查询扫描根的卷 GUID、物理存储目标和介质类型。 */
    static std::optional<DiscoveryRoot> PrepareRoot(const std::filesystem::path& path,
                                                    std::uint32_t priority,
                                                    bool hdd_extent_optimization,
                                                    std::string& error);

    /** @brief 流式遍历一个已准备扫描根。 */
    static DiscoveryStats Enumerate(const DiscoveryRoot& root,
                                    std::uint64_t scan_id,
                                    const std::atomic_bool& cancel_requested,
                                    const FileVisitor& visitor);

    /** @brief 按扩展名分类；音频仅参与 SHA-512，不进行内容 dHash。 */
    static MediaKind ClassifyMedia(const std::filesystem::path& path);
};

}  // namespace videosc::dedup
