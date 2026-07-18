#pragma once

#include "DHashSimilarity.h"
#include "ExactDuplicateReader.h"
#include "../models/CoreModels.h"
#include "../persistence/RocksStore.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 可持久化、可分页的报告类别。 */
enum class DuplicateReportKind {
    Exact,
    Similar,
};

/** @brief 视觉 dHash 完整性分类；静态视频只影响参与比较，不影响完整性。 */
enum class VisualDHashStatus {
    Valid,
    InvalidImage,
    MissingVideo,
    ZeroVideoFrame,
    UnsupportedMedia,
};

/**
 * @brief 按报告规则检查图片或视频 dHash 是否完整。
 * @param data MySQL 或 RocksDB 中的媒体内容记录。
 * @return 完整时返回 Valid，否则返回可用于跳过统计的具体原因。
 */
VisualDHashStatus ClassifyVisualDHash(const ShaFileData& data) noexcept;

/** @brief 报告生成过程诊断级别；局部媒体问题使用 Warning。 */
enum class DuplicateReportDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

/**
 * @brief 一条不改变报告结果的结构化诊断事件。
 *
 * 回调接收者可将其显示到实时日志窗口，但不得从回调抛异常中断报告生成。
 */
struct DuplicateReportDiagnostic {
    DuplicateReportDiagnosticSeverity severity = DuplicateReportDiagnosticSeverity::Info;
    std::string stage;
    std::string operation;
    std::uint32_t status_code = 0;
    std::int64_t native_error = 0;
    /** @brief 图片 SHA、候选对或可读路径等定位信息。 */
    std::string subject;
    std::string message;
};

/** @brief 报告生成阶段的轻量进度快照。 */
struct DuplicateReportProgress {
    /** @brief 当前报告代号，用于隔离连续启动的报告任务。 */
    std::uint64_t generation_id = 0;
    /** @brief 当前阶段序号；视觉相似报告使用 1 至 9。 */
    std::uint32_t stage_index = 0;
    /** @brief 当前报告的阶段总数；视觉相似报告固定为 9。 */
    std::uint32_t stage_count = 0;
    /** @brief 当前阶段已检查的项目数，包含正常跳过的项目。 */
    std::uint64_t stage_processed = 0;
    /** @brief 当前阶段稳定总量；仅在 stage_total_known 为 true 时有效。 */
    std::uint64_t stage_total = 0;
    /** @brief 当前阶段是否具有可用于百分比计算的可靠总量。 */
    bool stage_total_known = false;
    /** @brief 已从 MySQL 读取并保存的视觉内容累计数。 */
    std::uint64_t processed_contents = 0;
    /** @brief 已从 MySQL 读取的 active 路径累计数。 */
    std::uint64_t processed_paths = 0;
    /** @brief 已保存的相似边累计数。 */
    std::uint64_t matched_pairs = 0;
    /** @brief 分桶召回后规范化去重的候选签名对总数。 */
    std::uint64_t candidate_pairs_total = 0;
    /** @brief 已完成视频 dHash 或图片分区 pHash 二筛的候选对数。 */
    std::uint64_t validated_candidate_pairs = 0;
    /** @brief 真实距离满足相似条件的候选签名对数。 */
    std::uint64_t accepted_similarity_pairs = 0;
    /** @brief 命中初筛但未通过二筛或三筛的候选对数。 */
    std::uint64_t rejected_bucket_collisions = 0;
    /** @brief 当前报告阶段冻结的校验工作线程数。 */
    std::uint32_t configured_validation_threads = 0;
    /** @brief 当前正在执行候选校验的工作线程数。 */
    std::uint32_t active_validation_threads = 0;
    /** @brief 已写入最终报告的相似组累计数。 */
    std::uint64_t emitted_groups = 0;
    /** @brief 当前阶段稳定键，由 GUI 映射为中文名称。 */
    std::string stage;
};

/** @brief 一次报告生成的最终状态。 */
struct DuplicateReportResult {
    bool succeeded = false;
    bool cancelled = false;
    std::uint64_t generation_id = 0;
    std::uint64_t group_count = 0;
    /** @brief 因图片三级特征或视频 dHash 无效而跳过的视觉内容总数。 */
    std::uint64_t skipped_invalid_visuals = 0;
    /** @brief 跳过的无效图片内容数。 */
    std::uint64_t skipped_invalid_images = 0;
    /** @brief 跳过的无效视频内容数。 */
    std::uint64_t skipped_invalid_videos = 0;
    /** @brief 使用低质量严格阈值完成一筛判定的图片候选对数量。 */
    std::uint64_t low_quality_pairs_evaluated = 0;
    /** @brief 低质量候选中通过二筛并进入结构三筛的数量。 */
    std::uint64_t low_quality_pairs_secondary_passed = 0;
    /** @brief 低质量候选中最终通过结构三筛的数量。 */
    std::uint64_t low_quality_pairs_structure_accepted = 0;
    /** @brief 结构三筛无法完成读取或解码的候选对数量。 */
    std::uint64_t structural_io_failures = 0;
    /** @brief 结构三筛中至少一侧读取超时的候选对数量。 */
    std::uint64_t structural_timeouts = 0;
    /** @brief 结构已加载但原生比较函数未能完成的候选对数量。 */
    std::uint64_t structural_compute_failures = 0;
    std::string message;
};

/** @brief 不解码成员详情即可用于全局排序和虚拟列表定位的轻量组摘要。 */
struct ReportGroupSummary {
    std::uint64_t ordinal = 0;
    std::uint64_t group_id = 0;
    std::uint64_t member_count = 0;
    std::uint64_t reclaimable_bytes = 0;
    /** 相似报告是否保存了可排序的平均汉明距离。 */
    bool has_average_hamming_distance = false;
    /** 规范化相似证据的算术平均值。 */
    double average_hamming_distance = 0.0;
};

/** @brief 已发布视觉相似报告的规则快照，用于按版本解释图片三级规则与视频 dHash 规则。 */
struct SimilarReportMetadata {
    std::uint32_t report_schema_version = 4;
    std::uint32_t image_max_hamming_distance = 4;
    std::uint32_t video_max_average_hamming_distance = 5;
    std::uint32_t image_aspect_ratio_tolerance_percent = 10;
    std::uint32_t validation_worker_threads = 4;
    ImageSimilarityConfig image_similarity;
    std::uint32_t structural_worker_threads = 2;
    std::uint32_t structural_cache_mib = 256;
    bool image_uses_three_stage_verification = true;
    /** 报告作用域中的唯一图片 SHA 数量。 */
    std::uint64_t image_scope_total = 0;
    /** 具备当前 V1 图片特征的唯一 SHA 数量。 */
    std::uint64_t image_features_complete = 0;
    /** 因缺失、过期或损坏特征未进入报告的唯一 SHA 数量。 */
    std::uint64_t image_features_incomplete = 0;
    /** 是否因无效视觉内容或候选校验失败而发布了部分作用域报告。 */
    bool partial_scope_published = false;
    /** 因热门签名超过可靠展开上限而延迟的签名数量。 */
    std::uint64_t deferred_hot_signatures = 0;
    /** 候选工作数据实际峰值字节数。 */
    std::uint64_t candidate_peak_bytes = 0;
    /** 本次运行生成的规范化候选对数量。 */
    std::uint64_t candidate_pairs = 0;
    std::int64_t generated_utc_ms = 0;
    std::string media_algorithm_version;
    std::string image_bucket_index_version = "pdq16-r1-plus-dhash-dynamic-v3";
    std::string video_bucket_index_version = "video-bucket-v2";
    std::string image_grouping_rule = "image-three-stage-complete-link-v2";
    std::string video_grouping_rule = "video-average-complete-link-disjoint-v1";
    std::string image_aspect_ratio_rule = "relative-ratio-1pct-v1";
    std::string image_relation_rule = "image-pdq-structural-cross-group-v2";
    std::string image_primary_rule = "pdq-256-or-dhash-watermark-fallback-v2";
    std::string image_secondary_rule = "zoned-phash-4x4-v1";
    std::string image_tertiary_rule = "gray-sobel-block-structure-v1";
    std::string popcount_path;
};

/** @brief 跳过视觉内容的具体原因；Count 为计数哨兵，必须保持最后。 */
enum class SkippedVisualContentReason : std::uint8_t {
    InvalidImage = 0,
    MissingVideoDHash = 1,
    ZeroVideoFrame = 2,
    UnsupportedMedia = 3,
    StructuralIoFailure = 4,
    StructuralTimeout = 5,
    StructuralComputeFailure = 6,
    Count,  // 计数哨兵，必须保持最后
};

/** @brief 报告生成期因特征或结构校验失败而跳过的一条视觉内容记录。 */
struct SkippedVisualContentRecord {
    std::string primary_sha512;
    /** @brief 仅结构三筛失败时有值。 */
    std::string secondary_sha512;
    MediaKind media_kind = MediaKind::Other;
    SkippedVisualContentReason reason = SkippedVisualContentReason::InvalidImage;
    std::uint64_t active_path_count = 0;
    /** @brief 最多 4 条。 */
    std::vector<std::wstring> sample_paths;
};

/** @brief 指定代跳过视觉内容记录的总数与按原因计数。 */
struct SkippedVisualContentStats {
    std::uint64_t total = 0;
    std::array<std::uint64_t,
               static_cast<std::size_t>(SkippedVisualContentReason::Count)> by_reason{};
};

/**
 * @brief 判断视觉相似报告是否具备执行永久删除所需的证据质量。
 * @param metadata 已成功加载并校验的报告规则快照。
 * @return V4 且启用三级校验的报告返回 true。被跳过的视觉内容不进入分组，
 *         不影响已入组成员的成对证据，因此不再参与删除可信判定。
 */
bool IsSimilarReportEligibleForPermanentDeletion(const SimilarReportMetadata& metadata) noexcept;

/**
 * @brief 一个严格主组指向其他主组的图片直接相似签名关系。
 *
 * 关系按内容代表保存，不按活动文件路径展开，避免热门内容形成平方级记录。
 */
struct SimilarImageRelationSummary {
    std::uint64_t ordinal = 0;
    std::uint64_t current_group_id = 0;
    std::uint64_t neighbor_group_id = 0;
    std::string current_representative_sha512;
    std::string neighbor_representative_sha512;
    std::uint64_t current_image_dhash = 0;
    std::uint64_t neighbor_image_dhash = 0;
    std::uint8_t hamming_distance = 0;
    std::uint64_t neighbor_active_member_count = 0;
    bool neighbor_group_in_main_report = false;
};

/**
 * @brief RocksDB 分页报告存储。
 *
 * 新一代报告全部写完后才原子发布为 active，GUI 因此不会看到半成品。旧代数据带 generation_id
 * 命名空间，不会和当前报告混合。
 */
class DuplicateReportStore final {
public:
    explicit DuplicateReportStore(RocksStore& store);

    /** @brief 保存指定序号的完整重复组。 */
    RocksStatus SaveGroup(DuplicateReportKind kind,
                          std::uint64_t generation_id,
                          std::uint64_t ordinal,
                          const DuplicateGroup& group);

    /** @brief 原子发布新一代报告及组总数。 */
    RocksStatus Publish(DuplicateReportKind kind,
                        std::uint64_t generation_id,
                        std::uint64_t group_count);

    /**
     * @brief 删除指定类型的全部重复报告及异常退出遗留的报告工作数据。
     * @param kind 要清理的报告类型。
     * @return 删除成功或原本不存在时返回成功，否则返回 RocksDB 错误。
     *
     * 该操作只处理 `report/<kind>/` 命名空间，不删除媒体、哈希、扫描或 MySQL 数据。
     */
    RocksStatus DeleteAll(DuplicateReportKind kind);

    /** @brief 启动时删除未发布报告 generation 和相似报告 work 临时键。 */
    RocksStatus CleanupInterruptedWork();

    /** @brief 查询当前已完整发布的报告代号；没有报告时返回空。 */
    std::optional<std::uint64_t> ActiveGeneration(DuplicateReportKind kind) const;

    /** @brief 查询指定代的重复组总数。 */
    std::optional<std::uint64_t> GroupCount(DuplicateReportKind kind,
                                            std::uint64_t generation_id) const;

    /** @brief 按序号加载一页，内存只随 page_size 和单组成员数增长。 */
    RocksStatus LoadPage(DuplicateReportKind kind,
                         std::uint64_t generation_id,
                         std::uint64_t offset,
                         std::size_t page_size,
                         std::vector<DuplicateGroup>& groups) const;

    RocksStatus LoadSummaries(DuplicateReportKind kind,
                              std::uint64_t generation_id,
                              std::vector<ReportGroupSummary>& summaries);

    RocksStatus LoadGroup(DuplicateReportKind kind,
                          std::uint64_t generation_id,
                          std::uint64_t ordinal,
                          DuplicateGroup& group) const;

    /**
     * @brief 保存视觉相似报告规则快照。
     * @param generation_id 尚未发布的报告代号。
     * @param metadata 生成任务启动时冻结的规则。
     * @return RocksDB 写入状态。
     */
    RocksStatus SaveSimilarMetadata(std::uint64_t generation_id,
                                    const SimilarReportMetadata& metadata);

    /**
     * @brief 加载已发布或指定代的视觉相似报告规则快照。
     * @param generation_id 报告代号。
     * @param metadata 输出规则快照。
     * @return 缺少元数据时返回 not_found，调用方应提示重新生成旧报告。
     */
    RocksStatus LoadSimilarMetadata(std::uint64_t generation_id,
                                    SimilarReportMetadata& metadata) const;

    /** @brief 批量保存指定代的跳过视觉内容记录；空列表直接成功。随报告代前缀清理。
     * 每代仅应写入一次；同代重复写入会按序号覆盖并可能残留陈旧尾部记录，失败时已写批次随报告代清理机制回收。 */
    RocksStatus SaveSkippedContents(std::uint64_t generation_id,
                                    const std::vector<SkippedVisualContentRecord>& records);

    /** @brief 按写入序号分页加载跳过记录。 */
    RocksStatus LoadSkippedContents(std::uint64_t generation_id,
                                    std::uint64_t offset,
                                    std::size_t maximum_items,
                                    std::vector<SkippedVisualContentRecord>& records) const;

    /** @brief 统计指定代跳过记录总数与按原因计数。 */
    RocksStatus CountSkippedVisualContents(std::uint64_t generation_id,
                                           SkippedVisualContentStats& stats) const;

    /**
     * @brief 向指定严格组追加一条跨组图片相似关系。
     * @param generation_id 尚未发布的相似报告代号。
     * @param group_id 当前方向的严格组 ID。
     * @param relation 当前组到相邻组的关系摘要。
     * @return 写入关系和递增计数的原子状态。
     */
    RocksStatus SaveImageRelation(std::uint64_t generation_id,
                                  std::uint64_t group_id,
                                  const SimilarImageRelationSummary& relation);

    /** @return 指定严格组的跨组图片关系总数；不存在时返回零。 */
    std::uint64_t ImageRelationCount(std::uint64_t generation_id,
                                     std::uint64_t group_id) const;

    /**
     * @brief 按固定序号范围加载跨组图片关系。
     * @param generation_id 报告代号。
     * @param group_id 当前严格组 ID。
     * @param offset 起始关系序号。
     * @param maximum_items 最多加载条数。
     * @param relations 输出关系；每项 ordinal 为绝对序号。
     * @return RocksDB 或解码状态。
     */
    RocksStatus LoadImageRelations(std::uint64_t generation_id,
                                   std::uint64_t group_id,
                                   std::uint64_t offset,
                                   std::size_t maximum_items,
                                   std::vector<SimilarImageRelationSummary>& relations) const;

    /**
     * @brief 为一个图片视觉签名追加活动文件成员。
     * @param generation_id 尚未发布的报告代号。
     * @param representative_sha512 该视觉签名的稳定代表 SHA-512 十六进制。
     * @param member 活动路径成员。
     * @return 写入成员和递增计数的原子状态。
     */
    RocksStatus SaveImageSignatureMember(std::uint64_t generation_id,
                                         const std::string& representative_sha512,
                                         const DuplicateMember& member);

    /** @return 指定图片签名的活动成员数；不存在时返回零。 */
    std::uint64_t ImageSignatureMemberCount(
        std::uint64_t generation_id,
        const std::string& representative_sha512) const;

    /**
     * @brief 按路径 ID 顺序加载图片签名活动成员范围。
     * @param generation_id 报告代号。
     * @param representative_sha512 视觉签名代表 SHA-512 十六进制。
     * @param offset 起始成员序号。
     * @param maximum_items 最多加载条数。
     * @param members 输出活动成员。
     * @return RocksDB 或成员解码状态。
     */
    RocksStatus LoadImageSignatureMembers(
        std::uint64_t generation_id,
        const std::string& representative_sha512,
        std::uint64_t offset,
        std::size_t maximum_items,
        std::vector<DuplicateMember>& members) const;

private:
    RocksStore& store_;
};

/**
 * @brief 生成 SHA-512 精确报告和视觉相似报告。
 *
 * 精确报告从 MySQL 流式读取；相似报告顺序扫描 RocksDB 内容表，并通过分桶候选索引避免
 * 全量两两比较。候选组采用完整链接校验，组内任意两个不同内容都必须直接满足相似条件。
 */
class DuplicateReportGenerator final {
public:
    using ProgressCallback = std::function<void(const DuplicateReportProgress&)>;
    using DiagnosticCallback = std::function<void(const DuplicateReportDiagnostic&)>;

    DuplicateReportGenerator(
        RocksStore& store,
        std::string algorithm_version,
        DHashSimilarityConfig dhash_similarity = {},
        ImageSimilarityConfig image_similarity = {},
        ComputeConfig compute = {},
        IoConfig io = {},
        StorageConfig storage = {});

    /** @brief 自动读取 MySQL 当前有效映射并生成 SHA-512 精确重复报告。 */
    DuplicateReportResult GenerateExact(MySqlClient& client,
                                        const std::atomic_bool& cancel_requested,
                                        const ProgressCallback& progress_callback = {});

    /** @brief 从 MySQL 全量视觉内容执行候选筛选、复核和相似组报告生成。 */
    DuplicateReportResult GenerateSimilar(MySqlClient& client,
                                          const std::atomic_bool& cancel_requested,
                                          const ProgressCallback& progress_callback = {},
                                          const DiagnosticCallback& diagnostic_callback = {});

private:
    RocksStore& store_;
    std::string algorithm_version_;
    DHashSimilarityConfig dhash_similarity_;
    ImageSimilarityConfig image_similarity_;
    ComputeConfig compute_;
    IoConfig io_;
    StorageConfig storage_;
};

}  // namespace videosc::dedup
