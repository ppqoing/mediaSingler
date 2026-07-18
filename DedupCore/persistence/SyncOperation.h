#pragma once

#include "../models/CoreModels.h"
#include "RocksStore.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief RocksDB 待同步消息类型。 */
enum class SyncOperationKind {
    UpsertShaFileData,
    UpsertFilePath,
    DeleteFilePath,
};

/**
 * @brief 一条可崩溃恢复、幂等重放的 MySQL 同步消息。
 *
 * 删除消息必须同时携带 path_id 和 expected_sha512，防止旧删除覆盖同一路径的新文件。
 */
struct SyncOperation {
    std::uint64_t sequence = 0;
    /** @brief 产生该操作的扫描批次；零表示旧版本或非扫描业务操作。 */
    std::uint64_t batch_scan_id = 0;
    SyncOperationKind kind = SyncOperationKind::UpsertFilePath;
    std::optional<FilePathRecord> file_path;
    std::optional<ShaFileData> sha_file_data;
    std::uint64_t delete_path_id = 0;
    std::optional<Sha512Digest> expected_sha512;
    std::uint32_t attempt_count = 0;
    std::int64_t next_attempt_utc_ms = 0;
    unsigned int last_native_error = 0;
};

/** @brief 同步消息 JSON 编解码；不包含 MySQL 密码或连接配置。 */
class SyncOperationCodec final {
public:
    static std::string Serialize(const SyncOperation& operation);
    static std::optional<SyncOperation> Deserialize(const std::string& value, std::string& error);
};

/**
 * @brief 基于 RocksDB SyncQueue Column Family 的有序持久化队列。
 *
 * 单进程内的 sequence 分配由互斥锁串行化，并与业务记录在同一个 RocksDB WriteBatch 中提交。
 */
class MySqlSyncQueue final {
public:
    explicit MySqlSyncQueue(RocksStore& store);

    /**
     * @brief 原子写入本地业务变更和一条同步消息。
     * @param local_mutations 已准备好的本地业务变更。
     * @param operation sequence 为零时自动分配。
     */
    RocksStatus Enqueue(const std::vector<RocksMutation>& local_mutations, SyncOperation& operation);

    /**
     * @brief 原子保存业务结果和当前扫描的待发布操作，不进入正式同步队列。
     * @param scan_id 当前扫描标识。
     * @param local_mutations 与同步意图同生共死的本地业务变更。
     * @param operation 待发布操作；同一内容或路径的重复操作会覆盖合并。
     * @param inserted_new_key 返回是否新增了一个暂存键。
     */
    RocksStatus Stage(std::uint64_t scan_id,
                      const std::vector<RocksMutation>& local_mutations,
                      SyncOperation operation,
                      bool& inserted_new_key);

    /** @brief 把当前扫描最多 maximum_items 条暂存操作原子发布到正式队列。 */
    RocksStatus PublishStaged(std::uint64_t scan_id,
                              std::size_t maximum_items,
                              std::size_t& published_items);

    /** @brief 启动恢复时把所有历史扫描的暂存操作无门槛发布到正式队列。 */
    RocksStatus PublishAllStaged(std::size_t maximum_items_per_batch,
                                 std::size_t& published_items);

    /** @brief 读取当前扫描仍未发布的操作数量。 */
    RocksStatus StagedCount(std::uint64_t scan_id, std::uint64_t& count) const;

    /** @brief 读取当前扫描已发布但尚未确认的操作数量。 */
    RocksStatus PendingCount(std::uint64_t scan_id, std::uint64_t& count) const;

    /** @brief 按 sequence 顺序读取有限批消息。 */
    RocksStatus ReadBatch(std::size_t maximum_items, std::vector<SyncOperation>& operations) const;

    /** @brief MySQL 提交成功后批量确认并删除消息。 */
    RocksStatus Acknowledge(const std::vector<std::uint64_t>& sequences);

    /** @brief 保存失败次数、原生错误码和下一次重试时间。 */
    RocksStatus SaveRetry(const SyncOperation& operation);

private:
    static std::string QueueKey(std::uint64_t sequence);
    static std::string StagePrefix(std::uint64_t scan_id);
    static std::string StageKey(std::uint64_t scan_id, const SyncOperation& operation);
    RocksStatus NextSequence(std::uint64_t& sequence, std::vector<RocksMutation>& mutations);

    RocksStore& store_;
    mutable std::mutex sequence_mutex_;
};

}  // namespace videosc::dedup
