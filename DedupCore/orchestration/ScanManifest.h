#pragma once

#include "../discovery/NativeFileDiscovery.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 一个物理盘对应的已完成 JSONL 清单描述。 */
struct ScanManifestFile {
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    std::filesystem::path path;
    std::uint64_t file_count = 0;
};

/**
 * @brief 当前扫描按物理盘持久化的 UTF-8 JSONL 文件集合。
 *
 * 发现线程只持有各盘独立写入器；后续规划和提交均流式读取，不恢复全量文件向量。
 */
class ScanManifest final {
public:
    using Visitor = std::function<bool(const ScanManifestFile& manifest, DiscoveredFile&& file)>;

    /**
     * @brief 创建扫描清单管理器。
     * @param data_directory RocksDB 所在 data 目录或其等价持久化根。
     * @param scan_id 当前扫描标识。
     */
    ScanManifest(std::filesystem::path data_directory, std::uint64_t scan_id);
    ~ScanManifest();

    ScanManifest(const ScanManifest&) = delete;
    ScanManifest& operator=(const ScanManifest&) = delete;

    /** @brief 删除同 scan_id 的未完成旧清单并创建新目录。 */
    bool Begin(std::string& error);

    /** @brief 把一条发现结果追加到所属物理盘临时清单。 */
    bool Append(const DiscoveredFile& file, StorageMediaType media_type, std::string& error);

    /** @brief 关闭全部写入器、原子发布每盘文件并写 manifest.json。 */
    bool Complete(std::string& error);

    /** @brief 顺序流式遍历全部已完成清单。 */
    bool ForEach(const Visitor& visitor, std::string& error) const;

    /** @brief 流式遍历一个指定物理盘清单，供多盘生产者并行提交。 */
    bool ForEachFile(const ScanManifestFile& manifest,
                     const Visitor& visitor,
                     std::string& error) const;

    /** @return 已完成清单的轻量描述副本。 */
    std::vector<ScanManifestFile> files() const;

    /** @brief MySQL 同步屏障完成后删除整个扫描清单目录。 */
    bool Cleanup(std::string& error);

    /** @return 当前扫描清单目录。 */
    const std::filesystem::path& directory() const noexcept;

private:
    class Writer;
    std::shared_ptr<Writer> GetOrCreateWriter(const DiscoveredFile& file,
                                              StorageMediaType media_type,
                                              std::string& error);

    std::filesystem::path directory_;
    std::uint64_t scan_id_ = 0;
    mutable std::mutex writers_mutex_;
    std::vector<std::shared_ptr<Writer>> writers_;
    bool completed_ = false;
};

}  // namespace videosc::dedup
