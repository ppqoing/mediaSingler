#pragma once

#include "../config/AppConfig.h"
#include "../models/CoreModels.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief PDQ 候选器所需的紧凑图片记录。 */
struct PdqCandidateRecord {
    Sha512Digest digest{};
    PdqHash256 pdq{};
    std::uint64_t dhash = 0;
    bool has_dhash = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t quality = 0;
};

/** @brief 一次 PDQ 候选构建的资源与完成状态。 */
struct PdqCandidateBuildResult {
    bool succeeded = false;
    bool cancelled = false;
    std::uint64_t candidate_pairs = 0;
    std::uint64_t deferred_hot_signatures = 0;
    std::uint64_t peak_memory_bytes = 0;
    std::string message;
};

/**
 * @brief 使用签名压缩和两遍扁平倒排枚举可靠 PDQ 候选。
 *
 * 签名只压缩索引工作，不替代直接验证。每个自动输出的成员对都经过长宽比与完整 PDQ
 * 距离判断；热门签名或全局资源超限时返回失败，调用方不得发布部分自动报告。
 */
class PdqCandidateBuilder final {
public:
    using CandidateVisitor = std::function<bool(const Sha512Digest&, const Sha512Digest&)>;

    /** @param config 报告启动时冻结的图片相似配置。 */
    explicit PdqCandidateBuilder(ImageSimilarityConfig config);

    /**
     * @brief 枚举全部规范化候选成员对。
     * @param records SHA 唯一图片记录。
     * @param cancel_requested 外部取消标志。
     * @param visitor 每个无序成员对只回调一次；返回 false 中止并报告失败。
     * @return 候选数量、资源峰值和失败原因。
     */
    PdqCandidateBuildResult Build(const std::vector<PdqCandidateRecord>& records,
                                  const std::atomic_bool& cancel_requested,
                                  const CandidateVisitor& visitor) const;

private:
    ImageSimilarityConfig config_;
};

}  // namespace videosc::dedup
