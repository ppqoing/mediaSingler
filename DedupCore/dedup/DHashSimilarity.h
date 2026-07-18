#pragma once

#include "../models/CoreModels.h"
#include "../persistence/RocksStore.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief dHash 的一个多索引哈希分段。 */
struct DHashSegment {
    std::uint8_t index = 0;
    std::uint8_t bit_count = 0;
    std::uint64_t value = 0;
};

/** @brief 六段中一个两两联合索引键；半径 4 时至少有一对分段完全一致。 */
struct DHashSegmentPair {
    std::uint8_t first_index = 0;
    std::uint8_t second_index = 0;
    std::uint8_t first_bit_count = 0;
    std::uint8_t second_bit_count = 0;
    std::uint64_t first_value = 0;
    std::uint64_t second_value = 0;
};

/** @brief 将视频 6 帧 384 位签名按配置总距离动态切分得到的连续分段。 */
struct VideoDHashSegment {
    std::uint16_t index = 0;
    std::uint8_t bit_count = 0;
    std::uint64_t value = 0;
};

/** @brief dHash 真实距离和分段规则。 */
class DHashRules final {
public:
    static constexpr std::uint32_t kDefaultImageMaximumHammingDistance = 4;
    static constexpr std::int64_t kMaximumVideoDurationDifferenceMs = 2000;
    static constexpr std::size_t kVideoFrameCount = 6;

    /** @brief 使用硬件/编译器位计数计算 64 位汉明距离。 */
    static std::uint8_t HammingDistance(std::uint64_t left, std::uint64_t right) noexcept;

    /**
     * @brief 按配置阈值执行图片真实汉明距离判断。
     * @param left 左侧 64 位 dHash。
     * @param right 右侧 64 位 dHash。
     * @param maximum_distance 最大允许汉明距离，包含边界。
     * @return 距离小于等于 maximum_distance 时返回 true。
     */
    static bool ImagesAreSimilar(
        std::uint64_t left,
        std::uint64_t right,
        std::uint32_t maximum_distance = kDefaultImageMaximumHammingDistance) noexcept;

    /** @brief 六帧内部 15 组平均距离严格小于 5 时视为静态画面。 */
    static bool IsStaticVisual(const std::array<std::uint64_t, 6>& hashes) noexcept;

    /**
     * @brief 校验时长和六组真实距离，匹配时返回可解释证据。
     *
     * 静态画面视频只参与 SHA-512 精确去重，因此直接返回空。
     */
    static std::optional<SimilarityEvidence> CompareVideos(
        const ShaFileData& left,
        const ShaFileData& right,
        std::uint64_t left_path_id = 0,
        std::uint64_t right_path_id = 0,
        std::uint32_t maximum_average_distance = 5) noexcept;

    /**
     * @brief 判断两张图片的长宽比相对差异是否在配置容差内。
     * @param left_width 左图宽度。
     * @param left_height 左图高度。
     * @param right_width 右图宽度。
     * @param right_height 右图高度。
     * @param tolerance_percent 最大相对差异百分比，包含边界。
     * @return 尺寸有效且长宽比差异不超过容差时返回 true。
     */
    static bool ImageAspectRatiosCompatible(std::uint32_t left_width,
                                            std::uint32_t left_height,
                                            std::uint32_t right_width,
                                            std::uint32_t right_height,
                                            std::uint32_t tolerance_percent) noexcept;

    /**
     * @brief 将 64 位图片 dHash 均匀切为 maximum_distance + 1 个连续非空段。
     * @param hash 图片 dHash。
     * @param maximum_distance 配置的最大汉明距离，必须不超过 63。
     * @return 按高位到低位排列的动态分段。
     */
    static std::vector<DHashSegment> Split(
        std::uint64_t hash,
        std::uint32_t maximum_distance = kDefaultImageMaximumHammingDistance);

    /** @brief 切分为 11、11、11、11、10、10 位并生成全部 15 个两段联合键。 */
    static std::array<DHashSegmentPair, 15> SplitPairs(std::uint64_t hash) noexcept;

    /**
     * @brief 将 6 帧视频 dHash 视为 384 位连续签名并切成 6n+1 个分段。
     * @param hashes 按采样帧顺序排列的 6 个 dHash。
     * @param maximum_average_distance 允许的最大六帧平均距离 n。
     * @return 按高位到低位排列的动态分段；距离不超过 n 时至少一段完全相等。
     */
    static std::vector<VideoDHashSegment> SplitVideo(
        const std::array<std::uint64_t, kVideoFrameCount>& hashes,
        std::uint32_t maximum_average_distance);
};

/**
 * @brief RocksDB 多索引哈希候选索引。
 *
 * 每个唯一图片签名按配置 n 写 n+1 个单段桶；每个唯一非静态视频签名按 6n+1 段写入并带 2 秒时长桶。
 * 索引键以完整视觉签名结尾，重复内容会覆盖同一键，而不会让热门相同 dHash 形成巨型倒排链。
 * 图片距离不超过 n 时，n+1 段中至少一段完全一致；调用方仍必须加载内容记录后执行
 * DHashRules 的真实距离校验。视频用 384 位总距离上限 6n 的鸽巢分段保证召回。
 */
class DHashCandidateIndex final {
public:
    using CandidateVisitor = std::function<bool(const Sha512Digest& sha512)>;

    DHashCandidateIndex(RocksStore& store,
                        std::string algorithm_version,
                        std::string index_namespace = {},
                        std::uint32_t image_maximum_hamming_distance =
                            DHashRules::kDefaultImageMaximumHammingDistance,
                        std::uint32_t video_maximum_average_hamming_distance = 5);

    RocksStatus AddImage(const ShaFileData& data);
    RocksStatus AddVideo(const ShaFileData& data);

    RocksStatus FindImageCandidates(std::uint64_t image_dhash,
                                    const Sha512Digest& self,
                                    const CandidateVisitor& visitor) const;

    RocksStatus FindVideoCandidates(const ShaFileData& video,
                                    const CandidateVisitor& visitor) const;

private:
    std::string ImagePrefix(const DHashSegment& segment) const;
    std::string VideoPrefix(const VideoDHashSegment& segment,
                            std::uint64_t duration_bucket) const;
    static std::string ImageIndexKey(std::string prefix, std::uint64_t image_dhash);
    static std::string VideoIndexKey(std::string prefix, const ShaFileData& video);

    RocksStore& store_;
    std::string algorithm_version_;
    std::string index_namespace_;
    std::uint32_t image_maximum_hamming_distance_ =
        DHashRules::kDefaultImageMaximumHammingDistance;
    std::uint32_t video_maximum_average_hamming_distance_ = 5;
};

}  // namespace videosc::dedup
