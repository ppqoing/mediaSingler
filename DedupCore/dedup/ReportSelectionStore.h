#pragma once

#include "DHashSimilarity.h"
#include "../config/AppConfig.h"
#include "../models/CoreModels.h"
#include "../persistence/RocksStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

enum class DuplicateReportKind;

/** @brief 一个已通过安全距离复核的持久化待删除成员。 */
struct ReportSelectionMember {
    std::uint64_t path_id = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t retained_path_id = 0;
    bool has_measured_distance = false;
    double measured_distance = 0.0;
    double exclusive_limit = 0.0;
};

/** @brief 当前报告选择的轻量、可跨重启汇总。 */
struct ReportSelectionSnapshot {
    std::uint64_t selection_generation = 0;
    std::uint64_t source_report_generation = 0;
    std::uint64_t selected_file_count = 0;
    std::uint64_t selected_total_bytes = 0;
    std::uint64_t selected_group_count = 0;
    std::int64_t updated_utc_ms = 0;
};

/** @brief 单个候选相对保留成员的 dHash 安全判断结果。 */
struct ReportSelectionDecision {
    bool allowed = false;
    bool has_measured_distance = false;
    double measured_distance = 0.0;
    double exclusive_limit = 0.0;
    std::string reason;
};

/** @brief 图片、视频选择安全规则；精确 SHA-512 组不应用 dHash 上限。 */
class ReportSelectionRules final {
public:
    static ReportSelectionDecision Evaluate(
        const DuplicateGroup& group,
        const DuplicateMember& retained,
        const DuplicateMember& candidate,
        const ReportSelectionConfig& selection_config,
        std::uint32_t report_image_maximum_distance,
        std::uint32_t report_video_maximum_average_distance,
        bool image_report_already_three_stage_verified = false) noexcept;
};

/**
 * @brief 按报告类型和 generation 隔离的 RocksDB 选择事实来源。
 *
 * 单组变更用一个 WriteBatch 同步更新成员、组摘要和全局摘要；全报告选择先写未发布
 * selection generation，全部成功后再原子切换 active 指针，强杀不会暴露半成品。
 */
class ReportSelectionStore final {
public:
    explicit ReportSelectionStore(RocksStore& store);

    /** @brief 读取当前 active 选择摘要；尚无选择时返回全零快照。 */
    RocksStatus LoadSnapshot(DuplicateReportKind kind,
                             std::uint64_t report_generation,
                             ReportSelectionSnapshot& snapshot) const;

    /** @brief 读取指定组的全部已选成员；没有 active 选择时返回空列表。 */
    RocksStatus LoadGroup(DuplicateReportKind kind,
                          std::uint64_t report_generation,
                          std::uint64_t group_id,
                          std::vector<ReportSelectionMember>& members) const;

    /**
     * @brief 流式枚举当前 active selection 中有选择的组，不依赖 GUI 报告摘要。
     * @param kind 报告类型。
     * @param report_generation 选择所依附的报告 generation。
     * @param callback 每个组选中摘要调用一次；返回 false 可提前停止。
     * @return RocksDB 枚举状态；尚无 active 选择视为成功且不回调。
     */
    RocksStatus ForEachSelectedGroup(
        DuplicateReportKind kind,
        std::uint64_t report_generation,
        const std::function<bool(std::uint64_t group_id)>& callback) const;

    /** @brief 原子替换当前 active selection 中一个组的选择。 */
    RocksStatus SetGroup(DuplicateReportKind kind,
                         std::uint64_t report_generation,
                         std::uint64_t group_id,
                         const std::vector<ReportSelectionMember>& members,
                         ReportSelectionSnapshot& snapshot);

    /** @brief 创建未发布的 selection generation，供全报告后台任务逐组写入。 */
    RocksStatus BeginReplacement(DuplicateReportKind kind,
                                 std::uint64_t report_generation,
                                 std::uint64_t selection_generation);

    /** @brief 向未发布 generation 写入一个组，并累加其暂存摘要。 */
    RocksStatus SaveReplacementGroup(DuplicateReportKind kind,
                                     std::uint64_t report_generation,
                                     std::uint64_t selection_generation,
                                     std::uint64_t group_id,
                                     const std::vector<ReportSelectionMember>& members);

    /** @brief 完成暂存摘要后原子发布 selection generation。 */
    RocksStatus PublishReplacement(DuplicateReportKind kind,
                                   std::uint64_t report_generation,
                                   std::uint64_t selection_generation,
                                   ReportSelectionSnapshot& snapshot);

    /** @brief 删除未发布或失败的选择 generation。 */
    RocksStatus DiscardReplacement(DuplicateReportKind kind,
                                   std::uint64_t report_generation,
                                   std::uint64_t selection_generation);

    /** @brief 删除指定报告 generation 的全部选择状态。 */
    RocksStatus Clear(DuplicateReportKind kind, std::uint64_t report_generation);

    /** @brief 删除指定报告下未发布的 selection generation，保留 active 完整选择。 */
    RocksStatus CleanupInterruptedStaging(DuplicateReportKind kind,
                                          std::uint64_t report_generation);

private:
    RocksStore& store_;
};

}  // namespace videosc::dedup
