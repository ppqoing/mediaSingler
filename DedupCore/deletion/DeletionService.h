#pragma once

#include "../config/AppConfig.h"
#include "../models/CoreModels.h"
#include "../persistence/RocksStore.h"
#include "../persistence/SyncOperation.h"
#include "../scheduling/FileHasher.h"
#include "../logging/ExecutionLogger.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 每组保留成员的选择策略。 */
enum class KeepPolicy {
    Newest,
    Oldest,
    Smallest,
    Largest,
    HighestQuality,
    PathPriority,
};

/**
 * @brief 一组可同时启用的保留规则；字段顺序不代表执行顺序，比较器使用固定安全优先级。
 */
struct RetentionPolicySet {
    bool path_priority = false;
    bool highest_quality = true;
    bool newest = false;
    bool oldest = false;
    bool largest = false;
    bool smallest = false;

    /** @brief 至少存在一个启用规则时返回 true。 */
    bool any() const noexcept;

    /**
     * @brief 从旧单策略构造兼容策略集合。
     * @param policy 旧调用方指定的唯一策略。
     * @return 只启用对应规则的集合。
     */
    static RetentionPolicySet FromSingle(KeepPolicy policy) noexcept;
};

/** @brief 删除规划的确定结果，避免用空 optional 混淆不同安全失败原因。 */
enum class DeletionPlanStatus {
    Succeeded,
    InvalidPolicy,
    InsufficientMembers,
    TargetStorageNotFound,
    NoDeletionCandidates,
    UnsafeWholeGroupSelection,
};

/** @brief 删除规划、诊断原因和目标磁盘匹配数量。 */
struct DeletionPlanResult {
    DeletionPlanStatus status = DeletionPlanStatus::NoDeletionCandidates;
    std::optional<DuplicateGroup> plan;
    std::uint64_t matched_target_members = 0;
    std::string message;

    /** @brief 规划成功且包含安全计划时返回 true。 */
    bool succeeded() const noexcept {
        return status == DeletionPlanStatus::Succeeded && plan.has_value();
    }
};

/** @brief 单个永久删除成员的执行结果。 */
struct DeletionItemResult {
    std::uint64_t path_id = 0;
    std::filesystem::path path;
    bool deleted = false;
    bool mapping_delete_queued = false;
    std::uint32_t system_error = 0;
    std::string message;
};

/** @brief 一个删除批次的汇总结果。 */
struct DeletionBatchResult {
    std::uint64_t batch_id = 0;
    std::uint64_t retained_path_id = 0;
    std::vector<DeletionItemResult> items;
    std::uint64_t deleted_files = 0;
    std::uint64_t reclaimed_bytes = 0;
    bool preflight_succeeded = false;
};

/** @brief 只负责选择，不执行删除；选择与“删除选中文件”保持分离。 */
class DeletionPlanner final {
public:
    /**
     * @brief 使用固定优先级多策略生成至少保留一个成员的选择结果。
     * @param group 当前重复组。
     * @param policies 可同时启用的保留策略快照。
     * @param target_storage_target 指定时只选择该磁盘成员删除；匹配忽略首尾空白和大小写。
     * @return 包含明确状态和诊断原因的规划结果。
     */
    static DeletionPlanResult SelectDetailed(
        const DuplicateGroup& group,
        const RetentionPolicySet& policies,
        const std::optional<std::wstring>& target_storage_target = std::nullopt);

    /**
     * @brief 兼容旧单策略调用并生成至少保留一个成员的选择结果。
     * @param group 当前重复组。
     * @param keep_policy 唯一保留策略。
     * @param target_storage_target 指定时优先把该磁盘所有重复副本选中；若全组都在该盘仍保留一个。
     * @return 成功时返回计划，失败返回空值。
     */
    static std::optional<DuplicateGroup> Select(const DuplicateGroup& group,
                                                KeepPolicy keep_policy,
                                                const std::optional<std::wstring>& target_storage_target = std::nullopt);
};

/** @brief 线程安全滚动 UTF-8 文本操作日志；删除审计不写数据库。 */
class OperationLogger final {
public:
    explicit OperationLogger(LoggingConfig config);

    /** @brief 创建目录并验证日志可写；失败时永久删除批次不得开始。 */
    bool EnsureWritable(std::string& error);

    /** @brief 写入并刷盘一条删除或映射排队事件。 */
    bool Write(std::uint64_t batch_id,
               std::uint64_t group_id,
               const DuplicateMember& member,
               const std::string& action,
               bool succeeded,
               std::uint32_t system_error,
               const std::string& message);

private:
    bool RotateIfNeeded();

    LoggingConfig config_;
    ExecutionLogger execution_logger_;
    std::mutex mutex_;
};

/**
 * @brief 永久删除执行器。
 *
 * 先复核保留成员，再逐个复核待删文件的完整 SHA-512。每个文件独立写入删除意图；文件删除成功后，
 * 立即在同一安全边界内提交本地路径映射删除和 MySQL 条件删除消息，再开始下一个文件。
 */
class DeletionExecutor final {
public:
    using ProgressCallback = std::function<void(const DeletionItemResult& item,
                                                 const DeletionBatchResult& batch)>;
    using StopCallback = std::function<bool()>;
    DeletionExecutor(RocksStore& store,
                     MySqlSyncQueue& sync_queue,
                     std::shared_ptr<IFileHasher> hasher,
                     OperationLogger& logger);

    /**
     * @brief 执行已经由用户确认且至少保留一个成员的删除计划。
     * @param plan 已确认删除计划。
     * @param progress 单个文件完成安全边界后的进度回调。
     * @param should_stop 在开始下一个文件前检查；当前文件不会被中途截断。
     * @return 已完成文件的批次结果。
     */
    DeletionBatchResult Execute(const DuplicateGroup& plan,
                                const ProgressCallback& progress = {},
                                const StopCallback& should_stop = {});

    /** @brief 启动时恢复“文件已删但映射尚未排队”的崩溃窗口。 */
    RocksStatus RecoverPendingDeletes();

private:
    RocksStore& store_;
    MySqlSyncQueue& sync_queue_;
    std::shared_ptr<IFileHasher> hasher_;
    OperationLogger& logger_;
};

}  // namespace videosc::dedup
