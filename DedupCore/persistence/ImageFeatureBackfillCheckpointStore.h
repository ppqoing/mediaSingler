#pragma once

#include "RocksStore.h"

#include <cstdint>
#include <optional>
#include <string>

namespace videosc::dedup {

/** @brief 可恢复图片特征回填任务的持久化状态。 */
struct ImageFeatureBackfillCheckpoint {
    std::string algorithm_version;
    std::string last_sha512;
    std::uint64_t total_images = 0;
    std::uint64_t completed_images = 0;
    std::uint64_t failed_images = 0;
    std::uint64_t no_readable_path_images = 0;
    std::uint64_t timeout_images = 0;
    std::uint64_t decode_failed_images = 0;
    std::int64_t started_utc_ms = 0;
    std::int64_t updated_utc_ms = 0;
    bool finished = false;
};

/**
 * @brief 在 RocksDB Checkpoints 列族中保存图片特征回填状态。
 *
 * 本类型不执行媒体分析或 MySQL 写入，只维护一个版本化、可校验的当前任务快照。
 */
class ImageFeatureBackfillCheckpointStore final {
public:
    /** @param store 生命周期必须覆盖本存储对象。 */
    explicit ImageFeatureBackfillCheckpointStore(RocksStore& store) noexcept;

    /** @param checkpoint 待保存状态。 @return RocksDB 写入状态。 */
    RocksStatus Save(const ImageFeatureBackfillCheckpoint& checkpoint);

    /**
     * @brief 加载当前检查点。
     * @param error 解码失败时返回稳定错误。
     * @return 不存在时返回空且 error 为空。
     */
    std::optional<ImageFeatureBackfillCheckpoint> Load(std::string& error) const;

    /** @return 幂等删除当前检查点的状态。 */
    RocksStatus Clear();

private:
    RocksStore& store_;
};

}  // namespace videosc::dedup
