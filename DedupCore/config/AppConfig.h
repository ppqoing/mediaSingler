#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/**
 * @brief 当前程序支持的 JSON 配置模式版本。
 *
 * 配置迁移必须逐版本执行。读取到更高版本时，旧程序只报告错误，禁止覆盖原文件。
 */
inline constexpr std::uint32_t kCurrentConfigSchemaVersion = 5;

/** @brief 物理存储介质类型，用于选择默认读取并发和 HDD 调度策略。 */
enum class StorageMediaType {
    Unknown,
    Hdd,
    Ssd,
};

/** @brief MySQL TLS 校验模式。 */
enum class MySqlTlsMode {
    Disabled,
    Preferred,
    Required,
    VerifyCa,
    VerifyIdentity,
};

/** @brief 缩略图输出格式。 */
enum class ThumbnailFormat {
    Jpeg,
    Png,
};

/** @brief 文件发现方式。 */
enum class DiscoveryMethod {
    Native,     ///< FindFirstFileExW 递归遍历（回退方案）
    Everything, ///< Everything64.dll SDK 索引查询（默认）
};

/** @brief 文件发现配置。 */
struct DiscoveryConfig {
    DiscoveryMethod method = DiscoveryMethod::Everything;
    std::filesystem::path everything_dll_path;  ///< 空=自动检测 third_party\everything_sdk\Everything64.dll
    std::filesystem::path everything_exe_path;  ///< 空=自动检测 third_party\es\Everything.exe
    std::uint32_t query_page_size = 4096;        ///< 单次 Everything IPC 分页结果数。
    std::uint32_t launch_timeout_seconds = 30;  ///< 等待 Everything IPC 窗口出现的秒数。
    std::uint32_t db_load_timeout_seconds = 120; ///< 等待 Everything 索引数据库加载的秒数。
    std::uint32_t poll_interval_milliseconds = 200; ///< 启动与数据库就绪轮询间隔。
};

/** @brief 旧配置文件的逐盘读取线程记录；仅用于兼容读取，不再参与调度。 */
struct DiskReadThreadConfig {
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    std::uint32_t read_threads = 1;
};

/** @brief 扫描路径配置；数组顺序同时表示删除保留时的路径优先级。 */
struct PathConfig {
    std::vector<std::filesystem::path> scan_roots;
};

/** @brief 按介质自动分配的物理盘读取并发配置。 */
struct StorageConfig {
    std::uint32_t max_concurrent_file_reads = 8;
    std::uint32_t hdd_read_threads_per_disk = 1;
    std::uint32_t ssd_read_threads_per_disk = 2;
    /** 是否按物理盘实时读取占用动态增加并发许可。 */
    bool adaptive_read_threads = true;
    /** 单块物理盘读取占用目标；达到目标后停止增加该盘许可。 */
    std::uint32_t disk_read_target_percent = 90;
};

/** @brief 全局计算、自适应 CPU 目标与单任务 FFmpeg 并发配置。 */
struct ComputeConfig {
    std::uint32_t worker_threads = 4;
    std::uint32_t ffmpeg_threads_per_task = 1;
    bool adaptive_worker_threads = true;
    std::uint32_t cpu_target_percent = 90;
};

/** @brief 流式读取、队列、HDD 排序和坏块恢复配置。 */
struct IoConfig {
    std::uint32_t read_block_kib = 1024;
    std::uint32_t per_disk_queue_capacity = 1024;
    bool hdd_extent_optimization = true;
    std::uint32_t hdd_sort_window = 4096;
    std::uint32_t normal_block_retries = 2;
    std::uint32_t small_block_retries = 2;
    std::uint32_t small_block_kib = 64;
    std::uint32_t no_progress_timeout_seconds = 60;
};

/**
 * @brief MySQL 连接、同步和备份配置。
 *
 * password 仅存在于进程内存，序列化时只能写入 password_protected。若 DPAPI 解密失败，
 * password_decryption_failed 为 true，保存逻辑必须保留原密文，直到用户输入新密码。
 */
struct DatabaseConfig {
    std::wstring host = L"127.0.0.1";
    std::uint16_t port = 3306;
    std::wstring database_name = L"videosc";
    std::wstring user_name = L"root";
    std::wstring password;
    std::string password_protected;
    bool password_decryption_failed = false;
    MySqlTlsMode tls_mode = MySqlTlsMode::Disabled;
    std::filesystem::path tls_ca_path;
    std::filesystem::path tls_certificate_path;
    std::filesystem::path tls_private_key_path;
    std::uint32_t connection_pool_size = 4;
    std::uint32_t connect_timeout_seconds = 10;
    std::uint32_t command_timeout_seconds = 60;
    std::uint32_t retry_interval_seconds = 5;
    std::uint32_t sync_batch_size = 1000;
    std::filesystem::path mysqldump_path = L"mysqldump.exe";
    std::filesystem::path backup_directory;
};

/** @brief 2x3 视频拼图、图片预览和 GUI 缓存预算。 */
struct ThumbnailConfig {
    std::filesystem::path root_directory;
    ThumbnailFormat format = ThumbnailFormat::Jpeg;
    std::uint32_t video_cell_long_edge = 256;
    std::uint32_t image_preview_long_edge = 512;
    std::uint32_t cache_entries = 256;
    std::uint32_t memory_limit_mib = 512;
    std::uint32_t gpu_memory_limit_mib = 512;
};

/** @brief 图片、视频 dHash 候选分桶与最终汉明距离阈值配置。 */
struct DHashSimilarityConfig {
    /** 最大允许汉明距离；候选索引固定切分为 image_max_hamming_distance + 1 段。 */
    std::uint32_t image_max_hamming_distance = 4;
    /** 视频 6 帧汉明距离平均值的最大允许值。 */
    std::uint32_t video_max_average_hamming_distance = 5;
    /** 图片长宽比相对差异容许百分比；超过后不执行 dHash 比较。 */
    std::uint32_t image_aspect_ratio_tolerance_percent = 10;
    /** dHash 候选真实汉明距离校验的独立固定工作线程数。 */
    std::uint32_t validation_worker_threads = 4;
};

/**
 * @brief 一套图片三级筛选阈值。
 *
 * 最大距离越小越严格，最小结构分和最小通过区数量越大越严格。结构分使用百万分整数，
 * 避免 JSON 浮点往返和 CPU 舍入影响阈值边界。
 */
struct ImageSimilarityThresholdProfile {
    std::uint32_t pdq_max_hamming_distance = 31;
    /** PDQ 超限时允许进入二筛的兼容 dHash 最大距离。 */
    std::uint32_t fallback_dhash_max_hamming_distance = 4;
    std::uint32_t zoned_phash_tile_max_distance = 10;
    std::uint32_t zoned_phash_min_passing_tiles = 12;
    std::uint32_t zoned_phash_max_ignored_tiles = 4;
    std::uint32_t zoned_phash_trimmed_mean_max = 8;
    std::uint32_t structural_global_edge_min_millionths = 780000;
    std::uint32_t structural_trimmed_block_min_millionths = 930000;
    std::uint32_t structural_block_pass_score_millionths = 900000;
    std::uint32_t structural_min_passing_percent_millionths = 750000;
};

/**
 * @brief 图片“同一底图”三级筛选、候选预算、结构缓存和报告并发配置。
 *
 * 低于 pdq_min_quality 的图片仍参与筛选，但必须应用 low_quality_profile。该配置不处理
 * 旋转、裁剪、局部重复或语义相似图片。
 */
struct ImageSimilarityConfig {
    std::uint32_t aspect_ratio_tolerance_percent = 1;
    std::uint32_t pdq_min_quality = 50;
    ImageSimilarityThresholdProfile standard_profile;
    ImageSimilarityThresholdProfile low_quality_profile = {
        24, 2, 8, 14, 2, 6, 850000, 955000, 930000, 875000};
    std::uint32_t report_validation_worker_threads = 4;
    std::uint32_t structural_worker_threads = 2;
    std::uint32_t structural_cache_mib = 256;
    /** 候选紧凑记录与扁平索引允许占用的最大内存，单位 MiB。 */
    std::uint32_t candidate_memory_mib = 512;
    /** 本次报告候选工作数据允许占用的最大临时空间，单位 MiB。 */
    std::uint32_t candidate_temp_mib = 4096;
    /** 一个报告允许生成的规范化候选对上限。 */
    std::uint64_t candidate_max_pairs = 50'000'000;
    /** 相同压缩签名允许自动展开的最大成员数。 */
    std::uint32_t hot_signature_max_members = 2048;
    /** 单个压缩签名允许自动验证的直接成员对上限。 */
    std::uint64_t hot_signature_max_pairs = 2'000'000;
    /** 候选 RocksDB WriteBatch 的最大变更数。 */
    std::uint32_t candidate_write_batch_size = 4096;
    /** 候选内层循环最多执行多少次比较后必须检查取消。 */
    std::uint32_t candidate_cancel_check_stride = 4096;
    /** 已废弃兼容字段；报告始终跳过无法完成特征的图片，不再据此阻断。 */
    bool require_complete_features = false;
    /** 已废弃兼容字段；新报告始终允许发布带安全标记的部分作用域。 */
    bool allow_partial_reports = true;
    bool force_scalar_kernels = false;
};

/**
 * @brief dHash 报告选择阶段的额外安全距离上限。
 *
 * 空值表示使用当前报告 generation 中冻结的生成阈值；该配置不改变报告生成规则。
 */
struct ReportSelectionConfig {
    /** 图片候选必须严格小于该距离；空值使用报告图片阈值。 */
    std::optional<std::uint32_t> image_dhash_distance_exclusive_limit;
    /** 视频 6 帧平均距离必须严格小于该值；空值使用报告视频阈值。 */
    std::optional<double> video_dhash_average_distance_exclusive_limit;
};

/** @brief RocksDB 持久化目录和内存预算。 */
struct RocksDbConfig {
    std::filesystem::path directory;
    std::uint32_t block_cache_mib = 512;
    std::uint32_t write_buffer_mib = 128;
};

/** @brief 运行、坏块和永久删除操作的滚动文件日志配置。 */
struct LoggingConfig {
    std::filesystem::path directory;
    /** 应用任务轨迹与失败记录目录；与异常、崩溃和删除审计目录分离。 */
    std::filesystem::path execution_directory;
    std::uint32_t rotate_file_mib = 64;
    std::uint32_t rotate_count = 10;
    std::uint32_t retention_days = 30;
};

/** @brief 去重工具完整用户配置。 */
struct AppConfig {
    std::uint32_t schema_version = kCurrentConfigSchemaVersion;
    PathConfig paths;
    StorageConfig storage;
    ComputeConfig compute;
    IoConfig io;
    DiscoveryConfig discovery;
    DatabaseConfig database;
    ThumbnailConfig thumbnails;
    DHashSimilarityConfig dhash_similarity;
    ImageSimilarityConfig image_similarity;
    ReportSelectionConfig report_selection;
    RocksDbConfig rocksdb;
    LoggingConfig logging;

    /**
     * @brief 基于安装目录创建可直接展示和保存的默认配置。
     * @param install_directory 可执行文件所在目录。
     * @return 带有缩略图、RocksDB、日志和备份默认路径的配置。
     */
    static AppConfig CreateDefault(const std::filesystem::path& install_directory);
};

/**
 * @brief 获取当前进程可执行文件所在目录。
 * @return 绝对安装目录。
 * @throws std::runtime_error 无法读取模块路径时抛出。
 */
std::filesystem::path GetApplicationDirectory();

}  // namespace videosc::dedup
