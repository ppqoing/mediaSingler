#include "ConfigValidator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace videosc::dedup {
namespace {

/** @brief 添加一个数值越界错误。 */
template <typename T>
void RequireRange(std::vector<ValidationIssue>& issues,
                  const char* field,
                  const T value,
                  const T minimum,
                  const T maximum,
                  const wchar_t* message) {
    if (value < minimum || value > maximum) {
        issues.push_back({field, message, ValidationSeverity::Error});
    }
}

/**
 * @brief 校验一套图片三级筛选阈值。
 * @param issues 输出问题集合。
 * @param field_prefix JSON 字段前缀。
 * @param profile 待校验阈值。
 */
void ValidateImageProfile(std::vector<ValidationIssue>& issues,
                          const std::string& field_prefix,
                          const ImageSimilarityThresholdProfile& profile) {
    const auto field = [&](const char* suffix) { return field_prefix + '.' + suffix; };
    RequireRange(issues, field("pdq_max_hamming_distance").c_str(),
                 profile.pdq_max_hamming_distance, 0U, 31U,
                 L"PDQ 最大汉明距离必须在 0 到 31 之间。");
    RequireRange(issues, field("fallback_dhash_max_hamming_distance").c_str(),
                 profile.fallback_dhash_max_hamming_distance, 0U, 15U,
                 L"水印回退 dHash 最大汉明距离必须在 0 到 15 之间。");
    RequireRange(issues, field("zoned_phash_tile_max_distance").c_str(),
                 profile.zoned_phash_tile_max_distance, 0U, 64U,
                 L"分区 pHash 单区距离必须在 0 到 64 之间。");
    RequireRange(issues, field("zoned_phash_min_passing_tiles").c_str(),
                 profile.zoned_phash_min_passing_tiles, 1U, 16U,
                 L"分区 pHash 通过区数量必须在 1 到 16 之间。");
    RequireRange(issues, field("zoned_phash_max_ignored_tiles").c_str(),
                 profile.zoned_phash_max_ignored_tiles, 0U, 15U,
                 L"分区 pHash 忽略区数量必须在 0 到 15 之间。");
    if (profile.zoned_phash_min_passing_tiles + profile.zoned_phash_max_ignored_tiles < 16U) {
        issues.push_back({field("zoned_phash_min_passing_tiles"),
                          L"通过区与最大忽略区之和不能小于 16。",
                          ValidationSeverity::Error});
    }
    RequireRange(issues, field("zoned_phash_trimmed_mean_max").c_str(),
                 profile.zoned_phash_trimmed_mean_max, 0U, 64U,
                 L"分区 pHash 裁剪均值上限必须在 0 到 64 之间。");
    RequireRange(issues, field("structural_global_edge_min_millionths").c_str(),
                 profile.structural_global_edge_min_millionths, 0U, 1'000'000U,
                 L"结构全局边缘阈值必须在 0 到 1000000 之间。");
    RequireRange(issues, field("structural_trimmed_block_min_millionths").c_str(),
                 profile.structural_trimmed_block_min_millionths, 0U, 1'000'000U,
                 L"结构块裁剪均值阈值必须在 0 到 1000000 之间。");
    RequireRange(issues, field("structural_block_pass_score_millionths").c_str(),
                 profile.structural_block_pass_score_millionths, 0U, 1'000'000U,
                 L"结构块通过阈值必须在 0 到 1000000 之间。");
    RequireRange(issues, field("structural_min_passing_percent_millionths").c_str(),
                 profile.structural_min_passing_percent_millionths, 0U, 1'000'000U,
                 L"结构块通过比例必须在 0 到 1000000 之间。");
}

}  // namespace

std::vector<ValidationIssue> ConfigValidator::Validate(const AppConfig& config) {
    std::vector<ValidationIssue> issues;
    if (config.schema_version != kCurrentConfigSchemaVersion) {
        issues.push_back({"schema_version", L"配置版本不是当前程序支持的版本。", ValidationSeverity::Error});
    }

    RequireRange(issues, "compute.worker_threads", config.compute.worker_threads, 1U, 256U,
                 L"总计算线程数必须在 1 到 256 之间。");
    RequireRange(issues,
                 "compute.cpu_target_percent",
                 config.compute.cpu_target_percent,
                 1U,
                 100U,
                 L"系统 CPU 目标必须在 1% 到 100% 之间。");
    RequireRange(issues,
                 "compute.ffmpeg_threads_per_task",
                 config.compute.ffmpeg_threads_per_task,
                 1U,
                 32U,
                 L"FFmpeg 单任务线程数必须在 1 到 32 之间。");
    RequireRange(issues, "io.read_block_kib", config.io.read_block_kib, 64U, 16384U,
                 L"读取块大小必须在 64 KiB 到 16384 KiB 之间。");
    if (config.io.read_block_kib % 64 != 0) {
        issues.push_back({"io.read_block_kib", L"读取块大小必须是 64 KiB 的整数倍。", ValidationSeverity::Error});
    }
    RequireRange(issues,
                 "io.per_disk_queue_capacity",
                 config.io.per_disk_queue_capacity,
                 1U,
                 100000U,
                 L"每盘队列容量必须在 1 到 100000 之间。");
    RequireRange(issues, "io.hdd_sort_window", config.io.hdd_sort_window, 1U, 100000U,
                 L"HDD 排序窗口必须在 1 到 100000 之间。");
    RequireRange(issues, "io.normal_block_retries", config.io.normal_block_retries, 0U, 20U,
                 L"原块重试次数必须在 0 到 20 之间。");
    RequireRange(issues, "io.small_block_retries", config.io.small_block_retries, 0U, 20U,
                 L"小块重试次数必须在 0 到 20 之间。");
    RequireRange(issues, "io.small_block_kib", config.io.small_block_kib, 4U, 1024U,
                 L"坏块小块尺寸必须在 4 KiB 到 1024 KiB 之间。");
    if (config.io.small_block_kib > config.io.read_block_kib) {
        issues.push_back({"io.small_block_kib", L"坏块小块尺寸不能大于正常读取块。", ValidationSeverity::Error});
    }
    RequireRange(issues,
                 "io.no_progress_timeout_seconds",
                 config.io.no_progress_timeout_seconds,
                 1U,
                 3600U,
                 L"无读取进展超时必须在 1 秒到 3600 秒之间。");
    RequireRange(issues,
                 "discovery.query_page_size",
                 config.discovery.query_page_size,
                 128U,
                 100000U,
                 L"Everything 分页数量必须在 128 到 100000 之间。");
    RequireRange(issues,
                 "discovery.launch_timeout_seconds",
                 config.discovery.launch_timeout_seconds,
                 1U,
                 600U,
                 L"Everything 启动超时必须在 1 秒到 600 秒之间。");
    RequireRange(issues,
                 "discovery.db_load_timeout_seconds",
                 config.discovery.db_load_timeout_seconds,
                 1U,
                 3600U,
                 L"Everything 数据库加载超时必须在 1 秒到 3600 秒之间。");
    RequireRange(issues,
                 "discovery.poll_interval_milliseconds",
                 config.discovery.poll_interval_milliseconds,
                 10U,
                 5000U,
                 L"Everything 轮询间隔必须在 10 到 5000 毫秒之间。");

    RequireRange(issues,
                 "storage.max_concurrent_file_reads",
                 config.storage.max_concurrent_file_reads,
                 1U,
                 256U,
                 L"文件读取并发总上限必须在 1 到 256 之间。");
    RequireRange(issues,
                 "storage.hdd_read_threads_per_disk",
                 config.storage.hdd_read_threads_per_disk,
                 1U,
                 64U,
                 L"单块 HDD 读取线程数必须在 1 到 64 之间。");
    RequireRange(issues,
                 "storage.ssd_read_threads_per_disk",
                 config.storage.ssd_read_threads_per_disk,
                 1U,
                 64U,
                 L"单块 SSD 读取线程数必须在 1 到 64 之间。");
    RequireRange(issues,
                 "storage.disk_read_target_percent",
                 config.storage.disk_read_target_percent,
                 10U,
                 100U,
                 L"物理盘读取占用目标必须在 10% 到 100% 之间。");
    if (config.storage.hdd_read_threads_per_disk > config.storage.max_concurrent_file_reads ||
        config.storage.ssd_read_threads_per_disk > config.storage.max_concurrent_file_reads) {
        issues.push_back({"storage.max_concurrent_file_reads",
                          L"单块硬盘读取线程数不能超过文件读取并发总上限。",
                          ValidationSeverity::Error});
    }
    const std::uint64_t read_buffer_mib =
        static_cast<std::uint64_t>(config.storage.max_concurrent_file_reads) *
        config.io.read_block_kib / 1024U;
    if (read_buffer_mib > config.thumbnails.memory_limit_mib) {
        issues.push_back({"storage.max_concurrent_file_reads",
                          L"读取线程缓冲估算已超过缩略图内存预算，请核对总内存占用。",
                          ValidationSeverity::Warning});
    }

    if (config.database.host.empty()) {
        issues.push_back({"database.host", L"MySQL 主机不能为空。", ValidationSeverity::Error});
    }
    if (config.database.port == 0) {
        issues.push_back({"database.port", L"MySQL 端口必须在 1 到 65535 之间。", ValidationSeverity::Error});
    }
    if (config.database.database_name.empty()) {
        issues.push_back({"database.database_name", L"MySQL 数据库名不能为空。", ValidationSeverity::Error});
    }
    if (config.database.user_name.empty()) {
        issues.push_back({"database.user_name", L"MySQL 用户名不能为空。", ValidationSeverity::Error});
    }
    RequireRange(issues,
                 "database.connection_pool_size",
                 config.database.connection_pool_size,
                 1U,
                 128U,
                 L"MySQL 连接池大小必须在 1 到 128 之间。");
    RequireRange(issues,
                 "database.connect_timeout_seconds",
                 config.database.connect_timeout_seconds,
                 1U,
                 300U,
                 L"MySQL 连接超时必须在 1 秒到 300 秒之间。");
    RequireRange(issues,
                 "database.command_timeout_seconds",
                 config.database.command_timeout_seconds,
                 1U,
                 3600U,
                 L"MySQL 命令超时必须在 1 秒到 3600 秒之间。");
    RequireRange(issues,
                 "database.retry_interval_seconds",
                 config.database.retry_interval_seconds,
                 1U,
                 3600U,
                 L"MySQL 重试间隔必须在 1 秒到 3600 秒之间。");
    RequireRange(issues,
                 "database.sync_batch_size",
                 config.database.sync_batch_size,
                 1U,
                 100000U,
                 L"MySQL 同步批量必须在 1 到 100000 之间。");

    if (config.thumbnails.root_directory.empty()) {
        issues.push_back({"thumbnails.root_directory", L"缩略图目录不能为空。", ValidationSeverity::Error});
    }
    RequireRange(issues,
                 "thumbnails.video_cell_long_edge",
                 config.thumbnails.video_cell_long_edge,
                 64U,
                 4096U,
                 L"视频拼图单格长边必须在 64 到 4096 像素之间。");
    RequireRange(issues,
                 "thumbnails.image_preview_long_edge",
                 config.thumbnails.image_preview_long_edge,
                 64U,
                 4096U,
                 L"图片预览长边必须在 64 到 4096 像素之间。");
    RequireRange(issues, "thumbnails.cache_entries", config.thumbnails.cache_entries, 1U, 100000U,
                 L"缩略图缓存条目必须在 1 到 100000 之间。");
    RequireRange(issues,
                 "thumbnails.memory_limit_mib",
                 config.thumbnails.memory_limit_mib,
                 16U,
                 65536U,
                 L"缩略图内存上限必须在 16 MiB 到 65536 MiB 之间。");
    RequireRange(issues,
                 "thumbnails.gpu_memory_limit_mib",
                 config.thumbnails.gpu_memory_limit_mib,
                 16U,
                 65536U,
                 L"缩略图显存上限必须在 16 MiB 到 65536 MiB 之间。");
    RequireRange(issues,
                 "dhash_similarity.image_max_hamming_distance",
                 config.dhash_similarity.image_max_hamming_distance,
                 0U,
                 15U,
                 L"图片 dHash 最大汉明距离必须在 0 到 15 之间。");
    RequireRange(issues,
                 "dhash_similarity.validation_worker_threads",
                 config.dhash_similarity.validation_worker_threads,
                 1U,
                 256U,
                 L"dHash 汉明距离校验线程数必须在 1 到 256 之间。");
    RequireRange(issues,
                 "dhash_similarity.video_max_average_hamming_distance",
                 config.dhash_similarity.video_max_average_hamming_distance,
                 0U,
                 15U,
                 L"视频 6 帧平均汉明距离必须在 0 到 15 之间。");
    RequireRange(issues,
                 "dhash_similarity.image_aspect_ratio_tolerance_percent",
                 config.dhash_similarity.image_aspect_ratio_tolerance_percent,
                 0U,
                 100U,
                 L"图片长宽比容差必须在 0% 到 100% 之间。");
    RequireRange(issues,
                 "image_similarity.aspect_ratio_tolerance_percent",
                 config.image_similarity.aspect_ratio_tolerance_percent,
                 0U,
                 10U,
                 L"图片三级筛选长宽比容差必须在 0% 到 10% 之间。");
    RequireRange(issues,
                 "image_similarity.pdq_min_quality",
                 config.image_similarity.pdq_min_quality,
                 0U,
                 100U,
                 L"PDQ 最低质量分必须在 0 到 100 之间。");
    ValidateImageProfile(issues, "image_similarity.standard_profile",
                         config.image_similarity.standard_profile);
    ValidateImageProfile(issues, "image_similarity.low_quality_profile",
                         config.image_similarity.low_quality_profile);
    const ImageSimilarityThresholdProfile& standard = config.image_similarity.standard_profile;
    const ImageSimilarityThresholdProfile& low = config.image_similarity.low_quality_profile;
    if (low.pdq_max_hamming_distance > standard.pdq_max_hamming_distance ||
        low.fallback_dhash_max_hamming_distance >
            standard.fallback_dhash_max_hamming_distance ||
        low.zoned_phash_tile_max_distance > standard.zoned_phash_tile_max_distance ||
        low.zoned_phash_min_passing_tiles < standard.zoned_phash_min_passing_tiles ||
        low.zoned_phash_max_ignored_tiles > standard.zoned_phash_max_ignored_tiles ||
        low.zoned_phash_trimmed_mean_max > standard.zoned_phash_trimmed_mean_max ||
        low.structural_global_edge_min_millionths < standard.structural_global_edge_min_millionths ||
        low.structural_trimmed_block_min_millionths < standard.structural_trimmed_block_min_millionths ||
        low.structural_block_pass_score_millionths < standard.structural_block_pass_score_millionths ||
        low.structural_min_passing_percent_millionths < standard.structural_min_passing_percent_millionths) {
        issues.push_back({"image_similarity.low_quality_profile",
                          L"低质量图片配置的每一项都必须不弱于标准图片配置。",
                          ValidationSeverity::Error});
    }
    RequireRange(issues,
                 "image_similarity.report_validation_worker_threads",
                 config.image_similarity.report_validation_worker_threads,
                 1U,
                 256U,
                 L"图片报告校验线程数必须在 1 到 256 之间。");
    RequireRange(issues,
                 "image_similarity.structural_worker_threads",
                 config.image_similarity.structural_worker_threads,
                 1U,
                 64U,
                 L"图片结构直验线程数必须在 1 到 64 之间。");
    RequireRange(issues,
                 "image_similarity.structural_cache_mib",
                 config.image_similarity.structural_cache_mib,
                 16U,
                 65536U,
                 L"图片结构缓存必须在 16 MiB 到 65536 MiB 之间。");
    RequireRange(issues, "image_similarity.candidate_memory_mib",
                 config.image_similarity.candidate_memory_mib, 16U, 65536U,
                 L"候选内存预算必须在 16 MiB 到 65536 MiB 之间。");
    RequireRange(issues, "image_similarity.candidate_temp_mib",
                 config.image_similarity.candidate_temp_mib, 64U, 1'048'576U,
                 L"候选临时空间预算必须在 64 MiB 到 1048576 MiB 之间。");
    RequireRange(issues, "image_similarity.candidate_max_pairs",
                 config.image_similarity.candidate_max_pairs, std::uint64_t{1},
                 std::uint64_t{10'000'000'000}, L"候选对上限必须在 1 到 100 亿之间。");
    RequireRange(issues, "image_similarity.hot_signature_max_members",
                 config.image_similarity.hot_signature_max_members, 2U, 1'000'000U,
                 L"热门签名成员上限必须在 2 到 1000000 之间。");
    RequireRange(issues, "image_similarity.hot_signature_max_pairs",
                 config.image_similarity.hot_signature_max_pairs, std::uint64_t{1},
                 std::uint64_t{10'000'000'000}, L"热门签名直接对上限必须在 1 到 100 亿之间。");
    RequireRange(issues, "image_similarity.candidate_write_batch_size",
                 config.image_similarity.candidate_write_batch_size, 1U, 65536U,
                 L"候选写批次必须在 1 到 65536 之间。");
    RequireRange(issues, "image_similarity.candidate_cancel_check_stride",
                 config.image_similarity.candidate_cancel_check_stride, 1U, 1'048'576U,
                 L"候选取消检查粒度必须在 1 到 1048576 之间。");
    if (config.report_selection.image_dhash_distance_exclusive_limit.has_value()) {
        RequireRange(issues,
                     "report_selection.image_dhash_distance_exclusive_limit",
                     *config.report_selection.image_dhash_distance_exclusive_limit,
                     0U,
                     64U,
                     L"图片选择距离上限必须在 0 到 64 之间。");
    }
    if (config.report_selection.video_dhash_average_distance_exclusive_limit.has_value()) {
        const double limit = *config.report_selection.video_dhash_average_distance_exclusive_limit;
        if (!std::isfinite(limit) || limit < 0.0 || limit > 64.0) {
            issues.push_back({"report_selection.video_dhash_average_distance_exclusive_limit",
                              L"视频选择平均距离上限必须是 0 到 64 之间的有限数值。",
                              ValidationSeverity::Error});
        }
    }

    if (config.rocksdb.directory.empty()) {
        issues.push_back({"rocksdb.directory", L"RocksDB 目录不能为空。", ValidationSeverity::Error});
    }
    RequireRange(issues, "rocksdb.block_cache_mib", config.rocksdb.block_cache_mib, 16U, 65536U,
                 L"RocksDB 块缓存必须在 16 MiB 到 65536 MiB 之间。");
    RequireRange(issues, "rocksdb.write_buffer_mib", config.rocksdb.write_buffer_mib, 4U, 4096U,
                 L"RocksDB 写缓冲必须在 4 MiB 到 4096 MiB 之间。");
    if (config.logging.directory.empty()) {
        issues.push_back({"logging.directory", L"日志目录不能为空。", ValidationSeverity::Error});
    }
    if (config.logging.execution_directory.empty()) {
        issues.push_back({"logging.execution_directory", L"执行日志目录不能为空。", ValidationSeverity::Error});
    }
    RequireRange(issues, "logging.rotate_file_mib", config.logging.rotate_file_mib, 1U, 4096U,
                 L"单个日志文件大小必须在 1 MiB 到 4096 MiB 之间。");
    RequireRange(issues, "logging.rotate_count", config.logging.rotate_count, 1U, 1000U,
                 L"日志滚动数量必须在 1 到 1000 之间。");
    RequireRange(issues, "logging.retention_days", config.logging.retention_days, 1U, 3650U,
                 L"日志保留天数必须在 1 到 3650 之间。");
    return issues;
}

bool ConfigValidator::HasErrors(const std::vector<ValidationIssue>& issues) {
    return std::any_of(issues.begin(), issues.end(), [](const ValidationIssue& issue) {
        return issue.severity == ValidationSeverity::Error;
    });
}

}  // namespace videosc::dedup
