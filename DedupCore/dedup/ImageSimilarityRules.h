#pragma once

#include "../config/AppConfig.h"
#include "../models/CoreModels.h"

#include <array>
#include <cstdint>

namespace videosc::dedup {

/** @brief 分区 pHash 二筛的定量证据。 */
struct ZonedPHashEvidence {
    std::array<std::uint8_t, 16> tile_distances{};
    std::uint32_t passing_tiles = 0;
    std::uint32_t ignored_tiles = 0;
    std::uint32_t trimmed_distance_sum = 0;
    std::uint32_t retained_tiles = 0;
    bool accepted = false;
};

/**
 * @brief 图片三级筛选中的纯计算规则。
 *
 * 本类型不访问 RocksDB、MySQL、文件或 GUI；调用方负责候选索引、结构解码与成组。
 */
class ImageSimilarityRules final {
public:
    /** @return 图片是否具备当前 V1 PDQ、质量分和 16 个分区 pHash。 */
    static bool HasCompleteFeatures(const ShaFileData& data) noexcept;

    /** @return 两张未裁剪图片的长宽比是否落在配置容差内。 */
    static bool AspectRatiosCompatible(const ShaFileData& left,
                                       const ShaFileData& right,
                                       std::uint32_t tolerance_percent) noexcept;

    /** @return 两个 PDQ-256 的完整汉明距离，范围 0～256。 */
    static std::uint32_t PdqHammingDistance(const PdqHash256& left,
                                            const PdqHash256& right,
                                            bool force_scalar = false) noexcept;

    /**
     * @brief 执行 4×4 分区 pHash 二筛并返回全部 16 区距离。
     * @param left 左图片内容。
     * @param right 右图片内容。
     * @param profile 本图片对实际应用的冻结阈值配置。
     * @param force_scalar 是否强制使用标量位计数。
     * @return 包含通过区、忽略区和裁剪距离和的证据。
     */
    static ZonedPHashEvidence CompareZonedPHashes(const ShaFileData& left,
                                                  const ShaFileData& right,
                                                  const ImageSimilarityThresholdProfile& profile,
                                                  bool force_scalar = false) noexcept;

    /**
     * @brief 根据两侧 PDQ 质量选择标准或低质量严格配置。
     * @param left 左图片内容。
     * @param right 右图片内容。
     * @param config 图片相似配置。
     * @param quality_class 输出实际阈值类别。
     * @return 应用于当前图片对的阈值引用。
     */
    static const ImageSimilarityThresholdProfile& SelectThresholdProfile(
        const ShaFileData& left,
        const ShaFileData& right,
        const ImageSimilarityConfig& config,
        ImageQualityClass& quality_class) noexcept;

    /** @return 当前进程运行时是否选择硬件 POPCNT 路径。 */
    static bool UsesHardwarePopcnt() noexcept;
};

}  // namespace videosc::dedup
