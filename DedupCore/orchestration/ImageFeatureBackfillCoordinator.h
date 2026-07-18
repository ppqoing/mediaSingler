#pragma once

#include "../config/AppConfig.h"
#include "../persistence/ImageFeatureBackfillCheckpointStore.h"
#include "../persistence/MySqlClient.h"
#include "../persistence/MySqlReadRepository.h"
#include "../persistence/RocksStore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace videosc::dedup {

/** @brief 图片特征历史回填的线程安全进度快照。 */
struct ImageFeatureBackfillProgress {
    std::uint64_t total_images = 0;
    std::uint64_t initially_complete_images = 0;
    std::uint64_t completed_images = 0;
    std::uint64_t failed_images = 0;
    std::uint64_t no_readable_path_images = 0;
    std::uint64_t timeout_images = 0;
    std::uint64_t decode_failed_images = 0;
    std::uint64_t remaining_images = 0;
    std::string current_sha512;
};

/** @brief 一次图片特征历史回填的最终状态。 */
struct ImageFeatureBackfillResult {
    bool succeeded = false;
    bool cancelled = false;
    bool complete = false;
    ImageFeatureCompletenessSnapshot completeness;
    /** @brief 返回时的最终处理统计；失败和取消也保留已完成现场。 */
    ImageFeatureBackfillProgress final_progress;
    std::string message;
};

/**
 * @brief 对共享库唯一图片 SHA 执行可取消、幂等、可恢复的 V1 特征回填。
 *
 * 协调器复用 VideoSc 单次图片分析接口；MySQL 成功提交后才写本地内容缓存和检查点。
 * 路径故障会按稳定顺序尝试同一 SHA 的备用 active 路径。
 */
class ImageFeatureBackfillCoordinator final {
public:
    using ProgressCallback = std::function<void(const ImageFeatureBackfillProgress&)>;

    /**
     * @param store 已打开的 RocksDB。
     * @param client 线程安全 MySQL 客户端。
     * @param algorithm_version 当前媒体算法版本。
     * @param compute FFmpeg 单任务线程配置。
     * @param io 媒体无进展超时配置。
     * @param sync_batch_size MySQL 与本地批量提交大小。
     */
    ImageFeatureBackfillCoordinator(RocksStore& store,
                                    MySqlClient& client,
                                    std::string algorithm_version,
                                    ComputeConfig compute,
                                    IoConfig io,
                                    std::uint32_t sync_batch_size);

    /**
     * @brief 执行或恢复回填，直到全部目标处理、取消或持久化失败。
     * @param cancel_requested 外部取消标志。
     * @param progress_callback 节流由调用方决定的进度回调。
     * @return 任务结果和最终完整性快照。
     */
    ImageFeatureBackfillResult Run(
        const std::atomic_bool& cancel_requested,
        const ProgressCallback& progress_callback = {});

private:
    RocksStore& store_;
    MySqlClient& client_;
    std::string algorithm_version_;
    ComputeConfig compute_;
    IoConfig io_;
    std::uint32_t sync_batch_size_ = 1;
};

}  // namespace videosc::dedup
