#pragma once

#include "ProgressSnapshot.h"
#include "ScanManifest.h"
#include "ScanOptions.h"
#include "../persistence/MySqlReadRepository.h"
#include "../persistence/RocksStore.h"
#include "../persistence/ScanCheckpointStore.h"
#include "../persistence/SyncOperation.h"
#include "../scheduling/DiskHashScheduler.h"
#include "../discovery/NativeFileDiscovery.h"
#include "../discovery/EverythingFileDiscovery.h"
#include "../logging/ExecutionLogger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace videosc::dedup {

/**
 * @brief 判断图片特征分析失败后是否应在当前路径执行唯一一次重试。
 * @param status_code VideoSc 媒体分析状态码。
 * @param attempted_count 当前路径已经完成的分析次数。
 * @param cancellation_requested 外部是否已经请求取消。
 * @return 仅首次分析超时且未取消时返回 true。
 */
bool ShouldRetryImageFeatureAnalysis(int status_code,
                                     std::uint32_t attempted_count,
                                     bool cancellation_requested) noexcept;

/**
 * @brief 单个扫描任务的发现、SHA-512、媒体提取和本地持久化协调器。
 *
 * RocksStore 和 MySqlClient 必须在 Start 前初始化且生命周期覆盖协调器。扫描规划先读本地，
 * MySQL 可用时批量补齐；计算结果分批暂存并由独立 MySqlSyncService 异步同步。
 */
class ScanCoordinator final {
public:
    ScanCoordinator(ScanOptions options,
                    RocksStore& store,
                    MySqlSyncQueue& sync_queue,
                    MySqlClient& mysql_client,
                    std::function<void()> sync_wake);
    ~ScanCoordinator();

    ScanCoordinator(const ScanCoordinator&) = delete;
    ScanCoordinator& operator=(const ScanCoordinator&) = delete;

    /** @brief 启动新任务或使用指定 scan_id 恢复任务。 */
    bool Start(std::optional<std::uint64_t> resume_scan_id = std::nullopt);

    /** @brief 请求取消；正在等待的 Overlapped I/O 和 FFmpeg 回调会收到通知。 */
    void Cancel();

    /** @brief 等待任务线程结束。 */
    void Wait();

    /** @return 当前任务是否仍在运行。 */
    bool is_running() const noexcept;

    /** @brief 返回 GUI 可安全读取的不可变进度副本。 */
    ProgressSnapshot snapshot() const;

    /** @brief 最终同步屏障完成后标记检查点并删除本轮清单。 */
    bool FinalizeSynchronized(std::string& error);

    /** @return 本轮是否在精确报告后继续生成 dHash 报告。 */
    bool generate_similar_report() const noexcept;

private:
    void WorkerMain(std::uint64_t scan_id);
    /** @brief 阶段1：把发现结果写入对应物理盘 JSONL 清单。 */
    bool CollectDiscovered(DiscoveredFile&& file);
    /** @brief 联合 RocksDB/MySQL 规划每条清单记录的缺失能力。 */
    bool PlanManifestFiles(std::uint64_t scan_id);
    /** @brief 阶段2：流式读取每盘清单并提交需要 SHA-512 的任务。 */
    void SubmitDiscoveredJobs();
    void HandleHashCompleted(FileHashOutcome outcome);
    bool MarkUnseenPaths(std::uint64_t scan_id);
    bool ProcessMediaPhase();
    void ProcessMediaTask(std::string task_key, std::string task_value);
    /** @brief 原子暂存业务变更和同步操作，并按阈值发布完整批次。 */
    bool StageOperation(const std::vector<RocksMutation>& mutations, SyncOperation operation);
    /** @brief 发布达到阈值的完整批次，final_flush=true 时同时发布尾批。 */
    bool PublishAvailableBatches(bool final_flush);
    /** @brief 从 RocksDB 计数器刷新当前扫描暂存和待同步数量。 */
    bool RefreshSyncProgress();
    void SaveCheckpoint(ScanPhase phase);
    void SetFailure(const std::wstring& message);
    void InitializeDiscoveryProgress(const std::vector<DiscoveryRoot>& roots);
    void UpdateDiscoveryProgress(std::uint32_t root_priority,
                                 DiscoveryBackend backend,
                                 DiscoveryRootPhase phase,
                                 const std::wstring& fallback_reason = {});
    void CompleteDiscoveryProgress(std::uint32_t root_priority, DiscoveryRootPhase phase);
    void IncrementDiscoveryProgress(std::uint32_t root_priority);

    ScanOptions options_;
    RocksStore& store_;
    MySqlSyncQueue& sync_queue_;
    MySqlReadRepository mysql_reader_;
    std::function<void()> sync_wake_;
    ScanCheckpointStore checkpoint_store_;
    ExecutionLogger execution_logger_;
    std::unique_ptr<DiskHashScheduler> hash_scheduler_;
    std::thread worker_;
    std::atomic_bool running_{false};
    std::atomic_bool cancel_requested_{false};
    std::atomic_bool fatal_error_{false};
    mutable std::mutex progress_mutex_;
    ProgressSnapshot progress_;
    std::vector<std::chrono::steady_clock::time_point> discovery_started_at_;
    std::mutex discovery_record_mutex_;
    mutable std::mutex scheduler_control_mutex_;
    /** @brief 每盘介质类型，用于阶段1 HDD 电梯排序判断。 */
    std::map<std::wstring, StorageMediaType> disk_media_types_;
    std::unique_ptr<ScanManifest> manifest_;
    std::uint64_t active_scan_id_ = 0;
    std::mutex publish_mutex_;
    /** @brief 文件发现阶段的非致命警告（Everything 报错等），通过 snapshot 传给 GUI 弹窗。 */
    mutable std::mutex discovery_warning_mutex_;
    std::wstring discovery_warning_;
};

}  // namespace videosc::dedup
