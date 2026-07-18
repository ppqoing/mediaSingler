#include "ImageSimilarityRules.h"
#include "PopcountKernel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace videosc::dedup {

bool ImageSimilarityRules::HasCompleteFeatures(const ShaFileData& data) noexcept {
    return data.media_kind == MediaKind::Image && data.image_pdq_hash.has_value() &&
           data.image_pdq_quality.has_value() && data.has_image_zoned_phashes &&
           data.image_perceptual_algorithm_version == 1 &&
           data.image_structural_algorithm_version == 1;
}

bool ImageSimilarityRules::AspectRatiosCompatible(const ShaFileData& left,
                                                  const ShaFileData& right,
                                                  const std::uint32_t tolerance_percent) noexcept {
    if (left.width == 0 || left.height == 0 || right.width == 0 || right.height == 0) return false;
    const long double left_ratio = static_cast<long double>(left.width) / left.height;
    const long double right_ratio = static_cast<long double>(right.width) / right.height;
    const long double maximum_ratio = (std::max)(left_ratio, right_ratio);
    if (maximum_ratio <= 0.0L) return false;
    const long double relative_difference = std::abs(left_ratio - right_ratio) / maximum_ratio * 100.0L;
    return relative_difference <= static_cast<long double>(tolerance_percent);
}

std::uint32_t ImageSimilarityRules::PdqHammingDistance(const PdqHash256& left,
                                                       const PdqHash256& right,
                                                       const bool force_scalar) noexcept {
    std::uint32_t distance = 0;
    for (std::size_t offset = 0; offset < left.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t left_word = 0;
        std::uint64_t right_word = 0;
        memcpy(&left_word, left.data() + offset, sizeof(left_word));
        memcpy(&right_word, right.data() + offset, sizeof(right_word));
        distance += PopcountKernel::Count64(left_word ^ right_word, force_scalar);
    }
    return distance;
}

ZonedPHashEvidence ImageSimilarityRules::CompareZonedPHashes(
    const ShaFileData& left,
    const ShaFileData& right,
    const ImageSimilarityThresholdProfile& profile,
    const bool force_scalar) noexcept {
    ZonedPHashEvidence evidence;
    if (!left.has_image_zoned_phashes || !right.has_image_zoned_phashes) return evidence;

    for (std::size_t tile = 0; tile < evidence.tile_distances.size(); ++tile) {
        const std::uint32_t distance = PopcountKernel::Count64(
            left.image_zoned_phashes[tile] ^ right.image_zoned_phashes[tile],
            force_scalar);
        evidence.tile_distances[tile] = static_cast<std::uint8_t>(distance);
        if (distance <= profile.zoned_phash_tile_max_distance) ++evidence.passing_tiles;
        const std::uint32_t remaining = static_cast<std::uint32_t>(
            evidence.tile_distances.size() - tile - 1);
        if (evidence.passing_tiles + remaining < profile.zoned_phash_min_passing_tiles) {
            // 即使剩余分区全部通过也无法达到最低数量，避免继续做无意义位计数与排序。
            return evidence;
        }
    }

    auto sorted = evidence.tile_distances;
    std::sort(sorted.begin(), sorted.end());
    evidence.retained_tiles = (std::min)(profile.zoned_phash_min_passing_tiles, 16U);
    evidence.ignored_tiles = 16U - evidence.retained_tiles;
    for (std::size_t tile = 0; tile < evidence.retained_tiles; ++tile) {
        evidence.trimmed_distance_sum += sorted[tile];
    }
    const bool trimmedMeanAccepted = evidence.retained_tiles != 0 &&
                                     evidence.trimmed_distance_sum <=
                                         profile.zoned_phash_trimmed_mean_max * evidence.retained_tiles;
    evidence.accepted = evidence.passing_tiles >= profile.zoned_phash_min_passing_tiles &&
                        evidence.ignored_tiles <= profile.zoned_phash_max_ignored_tiles &&
                        trimmedMeanAccepted;
    return evidence;
}

const ImageSimilarityThresholdProfile& ImageSimilarityRules::SelectThresholdProfile(
    const ShaFileData& left,
    const ShaFileData& right,
    const ImageSimilarityConfig& config,
    ImageQualityClass& quality_class) noexcept {
    const bool lowQuality = !left.image_pdq_quality.has_value() ||
                            !right.image_pdq_quality.has_value() ||
                            *left.image_pdq_quality < config.pdq_min_quality ||
                            *right.image_pdq_quality < config.pdq_min_quality;
    quality_class = lowQuality ? ImageQualityClass::LowQuality : ImageQualityClass::Standard;
    return lowQuality ? config.low_quality_profile : config.standard_profile;
}

bool ImageSimilarityRules::UsesHardwarePopcnt() noexcept {
    return PopcountKernel::HasHardwarePopcnt();
}

}  // namespace videosc::dedup
