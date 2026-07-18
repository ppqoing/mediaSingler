#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 扫描协调器的可恢复阶段。 */
enum class ScanPhase {
    Idle,
    Discovering,
    Hashing,
    ExtractingMedia,
    Syncing,
    Matching,
    Deleting,
    Paused,
    CompletedLocal,
    CompletedSynchronized,
    Cancelled,
    Failed,
    Interrupted,
    Planning,
    FlushingSyncTail,
};

/** @brief 单个扫描根当前实际使用的文件发现后端。 */
enum class DiscoveryBackend {
    Pending,
    Everything,
    Native,
    EverythingThenNative,
};

/** @brief 单个扫描根在文件发现流水线中的实时阶段。 */
enum class DiscoveryRootPhase {
    Waiting,
    PreparingEverything,
    QueryingEverything,
    ProcessingEverythingResults,
    QueryingPhysicalLocation,
    ScanningNative,
    NativeFallback,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

/** @brief GUI 可直接展示的单扫描根发现状态，不暴露发现器内部对象。 */
struct DiscoveryRootProgress {
    std::filesystem::path root_path;
    DiscoveryBackend backend = DiscoveryBackend::Pending;
    DiscoveryRootPhase phase = DiscoveryRootPhase::Waiting;
    std::uint64_t discovered_files = 0;
    std::uint64_t elapsed_milliseconds = 0;
    std::wstring fallback_reason;
};

/** @brief 单个物理盘通道在一个 GUI 快照周期内的状态。 */
struct DiskProgress {
    std::wstring storage_target_key;
    std::wstring media_type;
    std::uint32_t configured_read_threads = 0;
    std::uint32_t allowed_read_threads = 0;
    std::uint32_t active_read_threads = 0;
    std::uint32_t cancel_pending_threads = 0;
    std::uint64_t queued_files = 0;
    std::uint64_t bytes_read = 0;
    double read_utilization_percent = 0.0;
    bool read_utilization_available = false;
    double throughput_mib_per_second = 0.0;
    std::uint64_t unreadable_files = 0;
    std::uint64_t timeout_files = 0;
    std::filesystem::path current_path;
};

/**
 * @brief 协调器每 100 至 250 ms 发布给 GUI 的不可变值快照。
 *
 * GUI 只消费该值对象，不遍历 RocksDB、读取工作队列或持有数据库事务。
 */
struct ProgressSnapshot {
    std::uint64_t scan_id = 0;
    ScanPhase phase = ScanPhase::Idle;
    /** @brief 用户已请求取消；协调线程可能仍在安全退出当前外部调用。 */
    bool cancellation_requested = false;
    std::uint64_t discovered_files = 0;
    /** @brief 本轮扫描中实际需要重新计算 SHA-512 的文件总数。 */
    std::uint64_t hash_total_files = 0;
    /** @brief 已返回哈希结果的任务数，包含成功、失败和超时。 */
    std::uint64_t hash_processed_files = 0;
    std::uint64_t hashed_files = 0;
    /** @brief 当前扫描按 SHA-512 去重后的媒体特征任务总数。 */
    std::uint64_t media_total_files = 0;
    /** @brief 媒体任务总数是否已完成统计；用于区分“正在统计”和“总数为零”。 */
    bool media_total_known = false;
    std::uint64_t media_processed_files = 0;
    /** @brief 本轮按 SHA-512 去重后最终未能生成完整图片特征的内容数。 */
    std::uint64_t image_feature_failed_contents = 0;
    std::uint64_t failed_files = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t configured_compute_threads = 0;
    std::uint32_t active_compute_threads = 0;
    std::uint32_t allowed_compute_threads = 0;
    std::uint32_t active_file_reads = 0;
    double system_cpu_percent = 0.0;
    std::uint64_t queued_compute_tasks = 0;
    std::uint64_t mysql_staged_operations = 0;
    std::uint64_t mysql_pending_operations = 0;
    std::int64_t mysql_last_success_utc_ms = 0;
    bool rocksdb_writable = false;
    bool mysql_connected = false;
    bool mysql_planning_degraded = false;
    bool local_scan_complete = false;
    bool shared_sync_complete = false;
    std::wstring latest_error;
    /** @brief 文件发现阶段的非致命警告（如 Everything 报错已回退 Native）。GUI 弹窗显示。 */
    std::wstring discovery_warning;
    std::vector<DiscoveryRootProgress> discovery_roots;
    std::vector<DiskProgress> disks;
};

}  // namespace videosc::dedup
