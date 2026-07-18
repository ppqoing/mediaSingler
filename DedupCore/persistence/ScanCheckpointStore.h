#pragma once

#include "../orchestration/ProgressSnapshot.h"
#include "RocksStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/**
 * @brief 可在进程崩溃后恢复的扫描检查点。
 *
 * 单个正在读取的文件不保存 BCrypt 内部状态；completed_files 之前已提交的文件可复用，当前文件从头重算。
 */
struct ScanCheckpoint {
    std::uint32_t record_version = 1;
    std::uint64_t scan_id = 0;
    ScanPhase phase = ScanPhase::Idle;
    std::string scan_options_json;
    std::string discovery_cursor;
    std::uint64_t discovered_files = 0;
    std::uint64_t completed_files = 0;
    std::uint64_t failed_files = 0;
    std::uint64_t media_completed_files = 0;
    /** @brief 已完成媒体任务中最终未能生成完整图片特征的唯一内容数。 */
    std::uint64_t image_feature_failed_contents = 0;
    std::uint64_t next_sync_sequence = 0;
    std::int64_t updated_utc_ms = 0;
};

/** @brief ScanCheckpoint 读取结果，区分不存在和损坏。 */
struct ScanCheckpointLoadResult {
    RocksStatus status;
    std::optional<ScanCheckpoint> checkpoint;
};

/**
 * @brief 使用 RocksDB Checkpoints Column Family 保存任务恢复点。
 *
 * SaveWithMutations 可把“单文件完成记录 + 检查点推进”放入同一 WriteBatch，防止崩溃后出现进度领先数据。
 */
class ScanCheckpointStore final {
public:
    /**
     * @brief 绑定已打开的 RocksStore。
     * @param store 生命周期必须覆盖本对象。
     */
    explicit ScanCheckpointStore(RocksStore& store);

    /** @brief 同步保存一个检查点。 */
    RocksStatus Save(const ScanCheckpoint& checkpoint);

    /**
     * @brief 原子保存业务变更和检查点。
     * @param checkpoint 新检查点。
     * @param mutations 已完成文件、同步队列等同批变更。
     */
    RocksStatus SaveWithMutations(const ScanCheckpoint& checkpoint,
                                  std::vector<RocksMutation> mutations);

    /** @brief 按 scan_id 读取检查点。 */
    ScanCheckpointLoadResult Load(std::uint64_t scan_id) const;

    /**
     * @brief 列出可恢复的活动、暂停或中断任务。
     * @param maximum_items 最大返回数量。
     * @param checkpoints 输出按 RocksDB 键顺序排列的检查点。
     */
    RocksStatus ListResumable(std::size_t maximum_items,
                              std::vector<ScanCheckpoint>& checkpoints) const;

    /** @brief 把现有任务同步标记为 Interrupted。 */
    RocksStatus MarkInterrupted(std::uint64_t scan_id);

private:
    RocksStore& store_;
};

}  // namespace videosc::dedup
