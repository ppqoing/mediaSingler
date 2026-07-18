#pragma once

#include "FileHasher.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 物理盘通道的并发、队列和 HDD 排序配置。 */
struct DiskChannelOptions {
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    std::uint32_t read_threads = 1;
    std::uint32_t queue_capacity = 1024;
    bool hdd_extent_optimization = true;
    std::uint32_t hdd_sort_window = 4096;
};

/** @brief 进入物理盘有界队列的单文件任务。 */
struct FileHashJob {
    std::uint64_t job_id = 0;
    std::uint64_t scan_id = 0;
    std::filesystem::path path;
    std::wstring storage_target_key;
    std::optional<std::uint64_t> physical_start_byte;
    FilePathRecord discovered_record;
    MediaKind media_kind = MediaKind::Other;
};

/** @brief 工作者完成后交给 RocksDB 写入层的值对象。 */
struct FileHashOutcome {
    FileHashJob job;
    FileHashResult result;
};

/** @brief 单物理盘通道的瞬时进度，不暴露内部队列对象。 */
struct DiskChannelSnapshot {
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    std::uint32_t configured_threads = 0;
    std::uint32_t allowed_threads = 0;
    std::uint32_t active_threads = 0;
    std::uint64_t queued_files = 0;
    std::uint64_t completed_files = 0;
    std::uint64_t failed_files = 0;
    std::uint64_t timeout_files = 0;
    std::uint64_t bytes_read = 0;
    double disk_read_utilization_percent = 0.0;
    bool disk_utilization_available = false;
};

/** @brief SHA-512 阶段的全局读取、计算并发与 CPU 快照。 */
struct SchedulerComputeSnapshot {
    std::uint32_t active_file_reads = 0;
    std::uint32_t active_compute_threads = 0;
    std::uint32_t allowed_compute_threads = 0;
    std::uint32_t maximum_compute_threads = 0;
    double system_cpu_percent = 0.0;
};

/**
 * @brief 跨物理盘并行、盘内有界限流的 SHA-512 调度器。
 *
 * 每个 StorageTargetKey 拥有独立队列和读取线程；HDD 在有界窗口内按近似物理位置执行电梯选择；
 * 所有通道还共享全局计算并发闸门。关闭提交后可等待自然清空，取消只跳过未开始任务并通知在途哈希。
 */
class DiskHashScheduler final {
public:
    using CompletionCallback = std::function<void(FileHashOutcome)>;

    /**
     * @brief 创建尚未启动的调度器。
     * @param hasher 线程安全的文件哈希实现。
     * @param maximum_concurrent_computations 所有物理盘共享的最大计算并发。
     * @throws std::invalid_argument 依赖为空或并发为零时抛出。
     */
    DiskHashScheduler(std::shared_ptr<IFileHasher> hasher,
                      std::uint32_t maximum_concurrent_computations,
                      std::uint32_t maximum_concurrent_file_reads,
                      bool adaptive_computations,
                      std::uint32_t cpu_target_percent,
                      bool adaptive_file_reads = false,
                      std::uint32_t disk_read_target_percent = 90);
    ~DiskHashScheduler();

    DiskHashScheduler(const DiskHashScheduler&) = delete;
    DiskHashScheduler& operator=(const DiskHashScheduler&) = delete;

    /**
     * @brief 创建物理盘通道并启动工作线程。
     * @param channels 唯一 StorageTargetKey 配置。
     * @param completion 每个任务完成或被取消时在工作线程调用。
     * @throws std::logic_error 重复启动时抛出。
     * @throws std::invalid_argument 通道配置非法或调度键重复时抛出。
     */
    void Start(std::vector<DiskChannelOptions> channels, CompletionCallback completion);

    /**
     * @brief 向对应物理盘有界队列提交任务，队列满时形成背压。
     * @param job 文件任务，storage_target_key 必须已配置。
     * @param timeout_milliseconds 等待队列空间的最长时间，零表示只尝试一次。
     * @return 已入队返回 true；超时、关闭、取消或未知通道返回 false。
     */
    bool Submit(FileHashJob job, std::uint32_t timeout_milliseconds);

    /** @brief 禁止后续提交，现有队列自然处理完毕。 */
    void CloseSubmissions();

    /**
     * @brief 等待所有已接收任务完成。
     * @param timeout_milliseconds 最长等待时间。
     * @return 任务归零返回 true，超时返回 false。
     */
    bool WaitUntilIdle(std::uint32_t timeout_milliseconds);

    /** @brief 非阻塞请求取消；清空未开始队列并通知在途哈希，但不等待工作线程退出。 */
    void RequestCancel();

    /** @brief 请求取消在途任务、取消未开始任务并回收所有工作线程。 */
    void CancelAndJoin();

    /** @brief 自然处理完已接收任务并回收工作线程。 */
    void Join();

    /** @return 当前每个物理盘的有界进度快照。 */
    std::vector<DiskChannelSnapshot> GetSnapshots() const;

    /** @return 当前全局文件读取和自适应计算状态。 */
    SchedulerComputeSnapshot GetComputeSnapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videosc::dedup
