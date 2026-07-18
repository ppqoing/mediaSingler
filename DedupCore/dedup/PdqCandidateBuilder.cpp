#include "PdqCandidateBuilder.h"

#include "ImageSimilarityRules.h"
#include "PopcountKernel.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace videosc::dedup {
namespace {

/** @brief 一个压缩签名及其原始成员下标。 */
struct SignatureGroup {
    std::vector<std::uint32_t> members;
};

/** @brief 一筛的互斥召回路径；dHash 只补充 PDQ 未召回的水印候选。 */
enum class PrimaryRoute {
    None,
    Pdq,
    DHashFallback,
};

/** @brief 读取 PDQ 的一个大端 16 位分段。 */
std::uint16_t PdqSegment(const PdqHash256& hash, const std::size_t segment) noexcept {
    const std::size_t offset = segment * 2U;
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(hash[offset]) << 8U) |
                                      hash[offset + 1U]);
}

/** @brief 不依赖 CPU 指令集的 16 位位计数，仅用于 PDQ 分桶规范化。 */
std::uint32_t Popcount16(std::uint16_t value) noexcept {
    value = static_cast<std::uint16_t>(value - ((value >> 1U) & 0x5555U));
    value = static_cast<std::uint16_t>((value & 0x3333U) + ((value >> 2U) & 0x3333U));
    return static_cast<std::uint32_t>(
        ((((value + (value >> 4U)) & 0x0f0fU) * 0x0101U) >> 8U) & 0x1fU);
}

/** @brief 读取动态 dHash 精确分段；分段数大于最大距离即可保证召回。 */
std::uint16_t ExtractDHashSegment(const std::uint64_t hash,
                                  const std::size_t segment,
                                  const std::size_t segment_count) noexcept {
    const std::size_t baseBits = 64U / segment_count;
    const std::size_t widerSegments = 64U % segment_count;
    const std::size_t bitCount = baseBits + (segment < widerSegments ? 1U : 0U);
    const std::size_t firstBit = segment * baseBits + (std::min)(segment, widerSegments);
    const std::size_t shift = 64U - firstBit - bitCount;
    const std::uint64_t mask = (std::uint64_t{1} << bitCount) - 1U;
    return static_cast<std::uint16_t>((hash >> shift) & mask);
}

/** @brief 一个动态 dHash 分段实际使用的桶数量。 */
std::size_t DHashBucketCount(const std::size_t segment,
                             const std::size_t segment_count) noexcept {
    const std::size_t baseBits = 64U / segment_count;
    const std::size_t widerSegments = 64U % segment_count;
    const std::size_t bitCount = baseBits + (segment < widerSegments ? 1U : 0U);
    return std::size_t{1} << bitCount;
}

/** @brief 长宽比量化到十万分之一，用于压缩相同比例的不同分辨率图片。 */
std::uint32_t AspectGrade(const PdqCandidateRecord& record) noexcept {
    if (record.width == 0 || record.height == 0) return 0;
    const std::uint64_t scaled = static_cast<std::uint64_t>(record.width) * 100'000ULL;
    return static_cast<std::uint32_t>((scaled + record.height / 2U) / record.height);
}

/** @brief 生成包含 PDQ、长宽比分级和质量类别的二进制稳定签名。 */
std::string PdqSignatureKey(const PdqCandidateRecord& record,
                            const std::uint32_t minimum_quality) {
    std::string key(reinterpret_cast<const char*>(record.pdq.data()), record.pdq.size());
    const std::uint32_t aspect = AspectGrade(record);
    key.append(reinterpret_cast<const char*>(&aspect), sizeof(aspect));
    key.push_back(record.quality < minimum_quality ? '\1' : '\0');
    return key;
}

/** @brief 生成 dHash 回退索引签名；无 dHash 的记录不会进入该索引。 */
std::string DHashSignatureKey(const PdqCandidateRecord& record,
                              const std::uint32_t minimum_quality) {
    std::string key(reinterpret_cast<const char*>(&record.dhash), sizeof(record.dhash));
    const std::uint32_t aspect = AspectGrade(record);
    key.append(reinterpret_cast<const char*>(&aspect), sizeof(aspect));
    key.push_back(record.quality < minimum_quality ? '\1' : '\0');
    return key;
}

/** @brief 判断一对图片由 PDQ 主路径、dHash 水印回退或均不召回。 */
PrimaryRoute SelectPrimaryRoute(const PdqCandidateRecord& left,
                                const PdqCandidateRecord& right,
                                const ImageSimilarityConfig& config) noexcept {
    ShaFileData leftData;
    leftData.width = left.width;
    leftData.height = left.height;
    leftData.image_pdq_quality = left.quality;
    ShaFileData rightData;
    rightData.width = right.width;
    rightData.height = right.height;
    rightData.image_pdq_quality = right.quality;
    if (!ImageSimilarityRules::AspectRatiosCompatible(
            leftData, rightData, config.aspect_ratio_tolerance_percent)) {
        return PrimaryRoute::None;
    }
    ImageQualityClass qualityClass = ImageQualityClass::Standard;
    const ImageSimilarityThresholdProfile& profile =
        ImageSimilarityRules::SelectThresholdProfile(
            leftData, rightData, config, qualityClass);
    if (ImageSimilarityRules::PdqHammingDistance(
            left.pdq, right.pdq, config.force_scalar_kernels) <=
        profile.pdq_max_hamming_distance) {
        return PrimaryRoute::Pdq;
    }
    if (left.has_dhash && right.has_dhash &&
        PopcountKernel::Count64(
            left.dhash ^ right.dhash, config.force_scalar_kernels) <=
            profile.fallback_dhash_max_hamming_distance) {
        return PrimaryRoute::DHashFallback;
    }
    return PrimaryRoute::None;
}

/** @brief 把有序 map 的成员列表移动到连续签名表。 */
std::vector<SignatureGroup> MoveSignatureGroups(
    std::map<std::string, SignatureGroup>& grouped) {
    std::vector<SignatureGroup> result;
    result.reserve(grouped.size());
    for (auto& item : grouped) result.push_back(std::move(item.second));
    return result;
}

}  // namespace

PdqCandidateBuilder::PdqCandidateBuilder(ImageSimilarityConfig config)
    : config_(std::move(config)) {}

PdqCandidateBuildResult PdqCandidateBuilder::Build(
    const std::vector<PdqCandidateRecord>& records,
    const std::atomic_bool& cancel_requested,
    const CandidateVisitor& visitor) const {
    PdqCandidateBuildResult result;
    if (!visitor) {
        result.message = "pdq candidate visitor is required";
        return result;
    }
    if (records.empty()) {
        result.succeeded = true;
        result.message = "ok";
        return result;
    }
    if (records.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.message = "too many image candidate records";
        return result;
    }

    const std::uint64_t memoryBudget =
        static_cast<std::uint64_t>(config_.candidate_memory_mib) * 1024ULL * 1024ULL;
    const std::uint64_t conservativeRecordBytes =
        static_cast<std::uint64_t>(records.size()) *
        (sizeof(PdqCandidateRecord) + sizeof(std::uint32_t) * 2ULL + 192ULL);
    if (conservativeRecordBytes > memoryBudget) {
        result.peak_memory_bytes = conservativeRecordBytes;
        result.message = "image candidate signature memory budget exceeded";
        return result;
    }

    const std::uint64_t cancelStride =
        (std::max<std::uint64_t>)(1U, config_.candidate_cancel_check_stride);
    std::map<std::string, SignatureGroup> groupedPdq;
    std::map<std::string, SignatureGroup> groupedDHash;
    for (std::uint32_t index = 0; index < records.size(); ++index) {
        if (index % cancelStride == 0 &&
            cancel_requested.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.message = "cancelled";
            return result;
        }
        groupedPdq[PdqSignatureKey(records[index], config_.pdq_min_quality)]
            .members.push_back(index);
        if (records[index].has_dhash) {
            groupedDHash[DHashSignatureKey(records[index], config_.pdq_min_quality)]
                .members.push_back(index);
        }
    }
    std::vector<SignatureGroup> pdqSignatures = MoveSignatureGroups(groupedPdq);
    std::vector<SignatureGroup> dhashSignatures = MoveSignatureGroups(groupedDHash);
    groupedPdq.clear();
    groupedDHash.clear();

    const std::uint64_t fixedBytes =
        conservativeRecordBytes +
        static_cast<std::uint64_t>(pdqSignatures.size() + dhashSignatures.size()) *
            sizeof(SignatureGroup) +
        65536ULL * sizeof(std::uint32_t) +
        65537ULL * sizeof(std::uint64_t) +
        static_cast<std::uint64_t>(
            (std::max)(pdqSignatures.size(), dhashSignatures.size())) *
            sizeof(std::uint32_t);
    result.peak_memory_bytes = fixedBytes;
    if (fixedBytes > memoryBudget) {
        result.message = "image candidate flat-index memory budget exceeded";
        return result;
    }

    std::uint64_t comparisonCounter = 0;
    const auto emitPair = [&](const std::uint32_t leftIndex,
                              const std::uint32_t rightIndex,
                              const PrimaryRoute requiredRoute) {
        ++comparisonCounter;
        if (comparisonCounter % cancelStride == 0 &&
            cancel_requested.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.message = "cancelled";
            return false;
        }
        if (SelectPrimaryRoute(records[leftIndex], records[rightIndex], config_) !=
            requiredRoute) {
            return true;
        }
        if (result.candidate_pairs >= config_.candidate_max_pairs) {
            result.message = "image candidate pair budget exceeded";
            return false;
        }
        if (!visitor(records[leftIndex].digest, records[rightIndex].digest)) {
            result.message = "image candidate visitor failed";
            return false;
        }
        ++result.candidate_pairs;
        return true;
    };

    const auto expandIdenticalSignatures =
        [&](const std::vector<SignatureGroup>& signatures,
            const PrimaryRoute requiredRoute) {
            for (const SignatureGroup& signature : signatures) {
                const std::uint64_t members = signature.members.size();
                const std::uint64_t pairs =
                    members > 1 ? members * (members - 1) / 2U : 0;
                if (members > config_.hot_signature_max_members ||
                    pairs > config_.hot_signature_max_pairs) {
                    ++result.deferred_hot_signatures;
                    continue;
                }
                for (std::size_t left = 0; left < signature.members.size(); ++left) {
                    for (std::size_t right = left + 1;
                         right < signature.members.size();
                         ++right) {
                        if (!emitPair(signature.members[left],
                                      signature.members[right],
                                      requiredRoute)) {
                            return false;
                        }
                    }
                }
            }
            return true;
        };

    if (!expandIdenticalSignatures(pdqSignatures, PrimaryRoute::Pdq) ||
        !expandIdenticalSignatures(dhashSignatures, PrimaryRoute::DHashFallback)) {
        return result;
    }
    if (result.deferred_hot_signatures != 0) {
        result.message = "hot image signatures require manual review";
        return result;
    }

    std::vector<std::uint32_t> counts(65536);
    std::vector<std::uint64_t> offsets(65537);
    std::vector<std::uint32_t> postings(
        (std::max)(pdqSignatures.size(), dhashSignatures.size()));

    // PDQ <= 31：16 个 16 位分段中必有一个距离 <= 1。
    for (std::size_t segment = 0; segment < 16; ++segment) {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.message = "cancelled";
            return result;
        }
        std::fill(counts.begin(), counts.end(), 0U);
        for (const SignatureGroup& signature : pdqSignatures) {
            ++counts[PdqSegment(records[signature.members.front()].pdq, segment)];
        }
        offsets[0] = 0;
        for (std::size_t bucket = 0; bucket < 65536; ++bucket) {
            offsets[bucket + 1] = offsets[bucket] + counts[bucket];
        }
        std::vector<std::uint64_t> cursors(offsets.begin(), offsets.end() - 1);
        for (std::uint32_t signatureIndex = 0;
             signatureIndex < pdqSignatures.size();
             ++signatureIndex) {
            const std::uint16_t bucket = PdqSegment(
                records[pdqSignatures[signatureIndex].members.front()].pdq, segment);
            postings[cursors[bucket]++] = signatureIndex;
        }
        result.peak_memory_bytes = (std::max)(
            result.peak_memory_bytes,
            fixedBytes + static_cast<std::uint64_t>(cursors.size()) *
                             sizeof(std::uint64_t));
        if (result.peak_memory_bytes > memoryBudget) {
            result.message = "image candidate flat-index memory budget exceeded";
            return result;
        }

        const auto expandPdqSignaturePair =
            [&](const std::uint32_t leftSignature,
                const std::uint32_t rightSignature) {
                const PdqHash256& leftPdq =
                    records[pdqSignatures[leftSignature].members.front()].pdq;
                const PdqHash256& rightPdq =
                    records[pdqSignatures[rightSignature].members.front()].pdq;
                for (std::size_t previous = 0; previous < segment; ++previous) {
                    if (Popcount16(static_cast<std::uint16_t>(
                            PdqSegment(leftPdq, previous) ^
                            PdqSegment(rightPdq, previous))) <= 1) {
                        return true;
                    }
                }
                for (const std::uint32_t left :
                     pdqSignatures[leftSignature].members) {
                    for (const std::uint32_t right :
                         pdqSignatures[rightSignature].members) {
                        if (!emitPair(left, right, PrimaryRoute::Pdq)) return false;
                    }
                }
                return true;
            };

        for (std::uint32_t bucket = 0; bucket < 65536; ++bucket) {
            const std::uint64_t begin = offsets[bucket];
            const std::uint64_t end = offsets[bucket + 1];
            for (std::uint64_t left = begin; left < end; ++left) {
                for (std::uint64_t right = left + 1; right < end; ++right) {
                    if (!expandPdqSignaturePair(postings[left], postings[right])) {
                        return result;
                    }
                }
            }
            for (std::uint32_t bit = 0; bit < 16; ++bit) {
                const std::uint32_t neighbor = bucket ^ (1U << bit);
                if (neighbor <= bucket) continue;
                for (std::uint64_t left = begin; left < end; ++left) {
                    for (std::uint64_t right = offsets[neighbor];
                         right < offsets[neighbor + 1];
                         ++right) {
                        if (!expandPdqSignaturePair(postings[left], postings[right])) {
                            return result;
                        }
                    }
                }
            }
        }
    }

    // dHash 水印回退：n+1 个精确分段保证距离 <= n 的候选被召回。
    const std::size_t dhashSegmentCount = (std::max<std::size_t>)(
        5U,
        static_cast<std::size_t>(
            config_.standard_profile.fallback_dhash_max_hamming_distance) +
            1U);
    for (std::size_t segment = 0; segment < dhashSegmentCount; ++segment) {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.message = "cancelled";
            return result;
        }
        const std::size_t bucketCount =
            DHashBucketCount(segment, dhashSegmentCount);
        std::fill(counts.begin(), counts.begin() + bucketCount, 0U);
        for (const SignatureGroup& signature : dhashSignatures) {
            ++counts[ExtractDHashSegment(
                records[signature.members.front()].dhash,
                segment,
                dhashSegmentCount)];
        }
        offsets[0] = 0;
        for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
            offsets[bucket + 1] = offsets[bucket] + counts[bucket];
        }
        std::vector<std::uint64_t> cursors(
            offsets.begin(), offsets.begin() + bucketCount);
        for (std::uint32_t signatureIndex = 0;
             signatureIndex < dhashSignatures.size();
             ++signatureIndex) {
            const std::uint16_t bucket = ExtractDHashSegment(
                records[dhashSignatures[signatureIndex].members.front()].dhash,
                segment,
                dhashSegmentCount);
            postings[cursors[bucket]++] = signatureIndex;
        }

        const auto expandDHashSignaturePair =
            [&](const std::uint32_t leftSignature,
                const std::uint32_t rightSignature) {
                const std::uint64_t leftDHash =
                    records[dhashSignatures[leftSignature].members.front()].dhash;
                const std::uint64_t rightDHash =
                    records[dhashSignatures[rightSignature].members.front()].dhash;
                for (std::size_t previous = 0; previous < segment; ++previous) {
                    if (ExtractDHashSegment(leftDHash, previous, dhashSegmentCount) ==
                        ExtractDHashSegment(rightDHash, previous, dhashSegmentCount)) {
                        return true;
                    }
                }
                for (const std::uint32_t left :
                     dhashSignatures[leftSignature].members) {
                    for (const std::uint32_t right :
                         dhashSignatures[rightSignature].members) {
                        if (!emitPair(
                                left, right, PrimaryRoute::DHashFallback)) {
                            return false;
                        }
                    }
                }
                return true;
            };

        for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
            const std::uint64_t begin = offsets[bucket];
            const std::uint64_t end = offsets[bucket + 1];
            for (std::uint64_t left = begin; left < end; ++left) {
                for (std::uint64_t right = left + 1; right < end; ++right) {
                    if (!expandDHashSignaturePair(
                            postings[left], postings[right])) {
                        return result;
                    }
                }
            }
        }
    }

    result.succeeded = true;
    result.message = "ok";
    return result;
}

}  // namespace videosc::dedup
