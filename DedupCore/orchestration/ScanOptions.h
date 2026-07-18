#pragma once

#include "../config/AppConfig.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace videosc::dedup {

/**
 * @brief 不包含任何密码材料的 MySQL 任务快照。
 *
 * 恢复任务时凭据由当前 DPAPI 配置注入；快照固定服务器、TLS、超时和批量语义。
 */
struct DatabaseTaskOptions {
    std::wstring profile_name = L"default";
    std::wstring host;
    std::uint16_t port = 3306;
    std::wstring database_name;
    std::wstring user_name;
    MySqlTlsMode tls_mode = MySqlTlsMode::Preferred;
    std::filesystem::path tls_ca_path;
    std::filesystem::path tls_certificate_path;
    std::filesystem::path tls_private_key_path;
    std::uint32_t connection_pool_size = 1;
    std::uint32_t connect_timeout_seconds = 10;
    std::uint32_t command_timeout_seconds = 60;
    std::uint32_t retry_interval_seconds = 5;
    std::uint32_t sync_batch_size = 1000;
};

/**
 * @brief 扫描创建时冻结的完整资源与路径快照。
 *
 * 该对象只暴露 const getter，运行中配置页面修改不会改变当前任务；固定算法边界由
 * algorithm_version 标识，不作为普通配置字段开放。
 */
class ScanOptions final {
public:
    /**
     * @brief 从已校验的用户配置冻结任务选项。
     * @param config 当前 GUI 配置。
     * @return 不包含明文或 DPAPI 密文的不可变任务选项。
     * @throws std::invalid_argument 配置存在阻止启动的错误时抛出。
     */
    static ScanOptions Freeze(const AppConfig& config, bool generate_similar_report = false);

    /** @return 有序扫描根目录；顺序代表路径保留优先级。 */
    const std::vector<std::filesystem::path>& scan_roots() const noexcept;
    /** @return 按介质分配和全局文件读取并发限制。 */
    const StorageConfig& storage() const noexcept;
    /** @return 全局计算线程和 FFmpeg 线程设置。 */
    const ComputeConfig& compute() const noexcept;
    /** @return 流式读取、队列、重试和超时设置。 */
    const IoConfig& io() const noexcept;
    /** @return 文件发现方式与 Everything 路径设置。 */
    const DiscoveryConfig& discovery() const noexcept;
    /** @return 不含密码的数据库任务设置。 */
    const DatabaseTaskOptions& database() const noexcept;
    /** @return 缩略图输出与缓存设置。 */
    const ThumbnailConfig& thumbnails() const noexcept;
    /** @return 图片 dHash 分桶数量和最终汉明距离阈值。 */
    const DHashSimilarityConfig& dhash_similarity() const noexcept;
    /** @return RocksDB 目录与内存设置。 */
    const RocksDbConfig& rocksdb() const noexcept;
    /** @return 滚动日志设置。 */
    const LoggingConfig& logging() const noexcept;
    /** @return 固定算法版本命名空间。 */
    const std::string& algorithm_version() const noexcept;
    /** @return 精确报告完成后是否自动生成 dHash 相似报告。 */
    bool generate_similar_report() const noexcept;

private:
    ScanOptions() = default;

    std::vector<std::filesystem::path> scan_roots_;
    StorageConfig storage_;
    ComputeConfig compute_;
    IoConfig io_;
    DiscoveryConfig discovery_;
    DatabaseTaskOptions database_;
    ThumbnailConfig thumbnails_;
    DHashSimilarityConfig dhash_similarity_;
    RocksDbConfig rocksdb_;
    LoggingConfig logging_;
    std::string algorithm_version_ = "media-dhash-v2";
    bool generate_similar_report_ = false;
};

}  // namespace videosc::dedup
