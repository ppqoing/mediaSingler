#include "DHashSimilarity.h"
#include "PopcountKernel.h"


#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace videosc::dedup {
namespace {

constexpr std::uint64_t kDurationBucketMilliseconds = 2000;

/** @brief 以大端序追加 uint64，保证 RocksDB 字典序与数值序一致。 */
void AppendBigEndian(std::string& key, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

/** @brief SHA-512 转二进制字符串，用于单次候选查询去重。 */
std::string DigestKey(const Sha512Digest& digest) {
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

/** @brief 校验算法版本只包含可安全放入二进制键前缀的短 ASCII。 */
bool IsSafeVersion(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.';
    });
}

bool HasValidVideoDHashes(const ShaFileData& data) noexcept {
    return data.has_video_dhashes &&
           std::all_of(data.video_dhashes.begin(), data.video_dhashes.end(), [](const std::uint64_t hash) {
               return hash != 0;
           });
}

}  // namespace

std::uint8_t DHashRules::HammingDistance(const std::uint64_t left, const std::uint64_t right) noexcept {
    return static_cast<std::uint8_t>(PopcountKernel::Count64(left ^ right));
}

bool DHashRules::ImagesAreSimilar(const std::uint64_t left,
                                  const std::uint64_t right,
                                  const std::uint32_t maximum_distance) noexcept {
    return HammingDistance(left, right) <= maximum_distance;
}

bool DHashRules::IsStaticVisual(const std::array<std::uint64_t, 6>& hashes) noexcept {
    std::uint32_t total = 0;
    for (std::size_t left = 0; left < hashes.size(); ++left) {
        for (std::size_t right = left + 1; right < hashes.size(); ++right) {
            total += HammingDistance(hashes[left], hashes[right]);
        }
    }
    return total < 5U * 15U;
}

std::optional<SimilarityEvidence> DHashRules::CompareVideos(
    const ShaFileData& left,
    const ShaFileData& right,
    const std::uint64_t left_path_id,
    const std::uint64_t right_path_id,
    const std::uint32_t maximum_average_distance) noexcept {
    if (left.media_kind != MediaKind::Video || right.media_kind != MediaKind::Video ||
        !HasValidVideoDHashes(left) || !HasValidVideoDHashes(right) ||
        left.static_visual || right.static_visual) {
        return std::nullopt;
    }
    const std::int64_t durationDifference =
        left.video_duration_ms >= right.video_duration_ms
            ? left.video_duration_ms - right.video_duration_ms
            : right.video_duration_ms - left.video_duration_ms;
    if (durationDifference > kMaximumVideoDurationDifferenceMs) return std::nullopt;

    SimilarityEvidence evidence;
    evidence.left_path_id = left_path_id;
    evidence.right_path_id = right_path_id;
    evidence.duration_difference_ms = durationDifference;
    evidence.compared_frame_count = static_cast<std::uint32_t>(kVideoFrameCount);
    std::uint32_t total = 0;
    for (std::size_t index = 0; index < kVideoFrameCount; ++index) {
        evidence.frame_distances[index] = HammingDistance(left.video_dhashes[index], right.video_dhashes[index]);
        total += evidence.frame_distances[index];
    }
    if (total > maximum_average_distance * kVideoFrameCount) return std::nullopt;
    evidence.average_hamming_distance = static_cast<double>(total) / static_cast<double>(kVideoFrameCount);
    return evidence;
}

bool DHashRules::ImageAspectRatiosCompatible(const std::uint32_t left_width,
                                             const std::uint32_t left_height,
                                             const std::uint32_t right_width,
                                             const std::uint32_t right_height,
                                             const std::uint32_t tolerance_percent) noexcept {
    if (left_width == 0 || left_height == 0 || right_width == 0 || right_height == 0) return false;
    const double leftRatio = static_cast<double>(left_width) / static_cast<double>(left_height);
    const double rightRatio = static_cast<double>(right_width) / static_cast<double>(right_height);
    const double denominator = (std::max)(leftRatio, rightRatio);
    if (denominator <= 0.0) return false;
    const double differencePercent = std::abs(leftRatio - rightRatio) / denominator * 100.0;
    return differencePercent <= static_cast<double>(tolerance_percent);
}

std::vector<DHashSegment> DHashRules::Split(const std::uint64_t hash,
                                            const std::uint32_t maximum_distance) {
    if (maximum_distance > 63) throw std::invalid_argument("Image dHash distance is too large");
    const std::uint32_t segmentCount = maximum_distance + 1;
    const std::uint32_t baseWidth = 64 / segmentCount;
    const std::uint32_t widerSegments = 64 % segmentCount;
    std::vector<DHashSegment> segments;
    segments.reserve(segmentCount);
    std::uint32_t remaining = 64;
    for (std::uint32_t index = 0; index < segmentCount; ++index) {
        const std::uint32_t width = baseWidth + (index < widerSegments ? 1U : 0U);
        remaining -= width;
        const std::uint64_t mask = width == 64 ? std::numeric_limits<std::uint64_t>::max()
                                               : (1ULL << width) - 1ULL;
        segments.push_back({static_cast<std::uint8_t>(index),
                            static_cast<std::uint8_t>(width),
                            (hash >> remaining) & mask});
    }
    return segments;
}

std::array<DHashSegmentPair, 15> DHashRules::SplitPairs(const std::uint64_t hash) noexcept {
    constexpr std::array<std::uint8_t, 6> widths = {11, 11, 11, 11, 10, 10};
    std::array<DHashSegment, 6> segments{};
    std::uint32_t remaining = 64;
    for (std::size_t index = 0; index < widths.size(); ++index) {
        remaining -= widths[index];
        const std::uint64_t mask = (1ULL << widths[index]) - 1ULL;
        segments[index] = {static_cast<std::uint8_t>(index), widths[index], (hash >> remaining) & mask};
    }
    std::array<DHashSegmentPair, 15> pairs{};
    std::size_t output = 0;
    for (std::size_t first = 0; first < segments.size(); ++first) {
        for (std::size_t second = first + 1; second < segments.size(); ++second) {
            pairs[output++] = {segments[first].index,
                               segments[second].index,
                               segments[first].bit_count,
                               segments[second].bit_count,
                               segments[first].value,
                               segments[second].value};
        }
    }
    return pairs;
}

std::vector<VideoDHashSegment> DHashRules::SplitVideo(
    const std::array<std::uint64_t, kVideoFrameCount>& hashes,
    const std::uint32_t maximum_average_distance) {
    if (maximum_average_distance > 15) {
        throw std::invalid_argument("Video dHash average distance is too large");
    }
    constexpr std::uint32_t totalBits = static_cast<std::uint32_t>(kVideoFrameCount * 64U);
    const std::uint32_t segmentCount = maximum_average_distance *
                                           static_cast<std::uint32_t>(kVideoFrameCount) +
                                       1U;
    const std::uint32_t baseWidth = totalBits / segmentCount;
    const std::uint32_t widerSegments = totalBits % segmentCount;
    std::vector<VideoDHashSegment> segments;
    segments.reserve(segmentCount);
    std::uint32_t bitOffset = 0;
    for (std::uint32_t index = 0; index < segmentCount; ++index) {
        const std::uint32_t width = baseWidth + (index < widerSegments ? 1U : 0U);
        std::uint64_t value = 0;
        for (std::uint32_t localBit = 0; localBit < width; ++localBit) {
            const std::uint32_t globalBit = bitOffset + localBit;
            const std::uint32_t frame = globalBit / 64U;
            const std::uint32_t frameBit = globalBit % 64U;
            value = (value << 1U) | ((hashes[frame] >> (63U - frameBit)) & 1ULL);
        }
        segments.push_back({static_cast<std::uint16_t>(index),
                            static_cast<std::uint8_t>(width),
                            value});
        bitOffset += width;
    }
    return segments;
}

DHashCandidateIndex::DHashCandidateIndex(RocksStore& store,
                                         std::string algorithm_version,
                                         std::string index_namespace,
                                         const std::uint32_t image_maximum_hamming_distance,
                                         const std::uint32_t video_maximum_average_hamming_distance)
    : store_(store),
      algorithm_version_(std::move(algorithm_version)),
      index_namespace_(std::move(index_namespace)),
      image_maximum_hamming_distance_(image_maximum_hamming_distance),
      video_maximum_average_hamming_distance_(video_maximum_average_hamming_distance) {
    if (!IsSafeVersion(algorithm_version_)) throw std::invalid_argument("Invalid dHash algorithm version");
    if (!index_namespace_.empty() && !IsSafeVersion(index_namespace_)) {
        throw std::invalid_argument("Invalid dHash index namespace");
    }
    if (image_maximum_hamming_distance_ > 15) {
        throw std::invalid_argument("Invalid image dHash maximum distance");
    }
    if (video_maximum_average_hamming_distance_ > 15) {
        throw std::invalid_argument("Invalid video dHash maximum average distance");
    }
}

std::string DHashCandidateIndex::ImagePrefix(const DHashSegment& segment) const {
    std::string key = index_namespace_;
    if (!key.empty()) key.push_back('\0');
    key += algorithm_version_;
    key.push_back('\0');
    key.push_back('I');
    key.push_back(2);  // 图片动态单段桶索引格式版本。
    key.push_back(static_cast<char>(image_maximum_hamming_distance_));
    key.push_back(static_cast<char>(segment.index));
    key.push_back(static_cast<char>(segment.bit_count));
    AppendBigEndian(key, segment.value);
    return key;
}

std::string DHashCandidateIndex::VideoPrefix(const VideoDHashSegment& segment,
                                             const std::uint64_t duration_bucket) const {
    std::string key = index_namespace_;
    if (!key.empty()) key.push_back('\0');
    key += algorithm_version_;
    key.push_back('\0');
    key.push_back('V');
    key.push_back(2);  // 视频 384 位动态单段桶索引格式版本。
    key.push_back(static_cast<char>(video_maximum_average_hamming_distance_));
    key.push_back(static_cast<char>((segment.index >> 8U) & 0xFFU));
    key.push_back(static_cast<char>(segment.index & 0xFFU));
    key.push_back(static_cast<char>(segment.bit_count));
    AppendBigEndian(key, duration_bucket);
    AppendBigEndian(key, segment.value);
    return key;
}

std::string DHashCandidateIndex::ImageIndexKey(std::string prefix, const std::uint64_t image_dhash) {
    AppendBigEndian(prefix, image_dhash);
    return prefix;
}

std::string DHashCandidateIndex::VideoIndexKey(std::string prefix, const ShaFileData& video) {
    AppendBigEndian(prefix, static_cast<std::uint64_t>((std::max<std::int64_t>)(0, video.video_duration_ms)));
    for (const std::uint64_t hash : video.video_dhashes) AppendBigEndian(prefix, hash);
    return prefix;
}

RocksStatus DHashCandidateIndex::AddImage(const ShaFileData& data) {
    if (data.media_kind != MediaKind::Image || !data.image_dhash.has_value() || *data.image_dhash == 0) {
        return {false, "image_dhash_required"};
    }
    std::vector<RocksMutation> mutations;
    const std::vector<DHashSegment> segments =
        DHashRules::Split(*data.image_dhash, image_maximum_hamming_distance_);
    mutations.reserve(segments.size());
    for (const DHashSegment& segment : segments) {
        mutations.push_back({RocksColumnFamily::ImageDhashIndex,
                             ImageIndexKey(ImagePrefix(segment), *data.image_dhash),
                             DigestKey(data.sha512)});
    }
    return store_.WriteBatch(mutations, false);
}

RocksStatus DHashCandidateIndex::AddVideo(const ShaFileData& data) {
    if (data.media_kind != MediaKind::Video || !HasValidVideoDHashes(data)) {
        return {false, "video_dhashes_required"};
    }
    if (data.static_visual) return {true, {}};
    const std::uint64_t durationBucket =
        static_cast<std::uint64_t>((std::max<std::int64_t>)(0, data.video_duration_ms)) /
        kDurationBucketMilliseconds;
    std::vector<RocksMutation> mutations;
    const std::vector<VideoDHashSegment> segments = DHashRules::SplitVideo(
        data.video_dhashes, video_maximum_average_hamming_distance_);
    mutations.reserve(segments.size());
    for (const VideoDHashSegment& segment : segments) {
        mutations.push_back({RocksColumnFamily::VideoDhashIndex,
                             VideoIndexKey(VideoPrefix(segment, durationBucket), data),
                             DigestKey(data.sha512)});
    }
    return store_.WriteBatch(mutations, false);
}

RocksStatus DHashCandidateIndex::FindImageCandidates(const std::uint64_t image_dhash,
                                                     const Sha512Digest& self,
                                                     const CandidateVisitor& visitor) const {
    if (!visitor) return {false, "candidate_visitor_required"};
    std::unordered_set<std::string> seen;
    const std::string selfKey = DigestKey(self);
    for (const DHashSegment& segment : DHashRules::Split(image_dhash, image_maximum_hamming_distance_)) {
        const std::string prefix = ImagePrefix(segment);
        const RocksStatus status = store_.ForEachPrefix(
            RocksColumnFamily::ImageDhashIndex,
            prefix,
            0,
            [&](const std::string_view key, const std::string_view value) {
                if (key.size() != prefix.size() + sizeof(std::uint64_t) || value.size() != self.size()) return true;
                const std::string signature(key.substr(prefix.size()));
                if (!seen.insert(signature).second) return true;
                const std::string digest(value);
                if (digest == selfKey) return true;
                Sha512Digest candidate{};
                std::copy(digest.begin(), digest.end(), reinterpret_cast<char*>(candidate.data()));
                return visitor(candidate);
            });
        if (!status.succeeded) return status;
    }
    return {true, {}};
}

RocksStatus DHashCandidateIndex::FindVideoCandidates(const ShaFileData& video,
                                                     const CandidateVisitor& visitor) const {
    if (!visitor) return {false, "candidate_visitor_required"};
    if (video.media_kind != MediaKind::Video || !HasValidVideoDHashes(video) || video.static_visual) {
        return {true, {}};
    }
    const std::uint64_t durationBucket =
        static_cast<std::uint64_t>((std::max<std::int64_t>)(0, video.video_duration_ms)) /
        kDurationBucketMilliseconds;
    const std::uint64_t firstBucket = durationBucket == 0 ? 0 : durationBucket - 1;
    const std::uint64_t lastBucket = durationBucket + 1;
    const std::string selfKey = DigestKey(video.sha512);
    std::unordered_set<std::string> seen;
    const std::vector<VideoDHashSegment> segments = DHashRules::SplitVideo(
        video.video_dhashes, video_maximum_average_hamming_distance_);
    for (const VideoDHashSegment& segment : segments) {
        for (std::uint64_t bucket = firstBucket; bucket <= lastBucket; ++bucket) {
                const std::string prefix = VideoPrefix(segment, bucket);
                const RocksStatus status = store_.ForEachPrefix(
                    RocksColumnFamily::VideoDhashIndex,
                    prefix,
                    0,
                    [&](const std::string_view key, const std::string_view value) {
                        constexpr std::size_t signatureBytes = sizeof(std::uint64_t) * 7;
                        if (key.size() != prefix.size() + signatureBytes || value.size() != video.sha512.size()) {
                            return true;
                        }
                        const std::string signature(key.substr(prefix.size()));
                        if (!seen.insert(signature).second) return true;
                        const std::string digest(value);
                        if (digest == selfKey) return true;
                        Sha512Digest candidate{};
                        std::copy(digest.begin(), digest.end(), reinterpret_cast<char*>(candidate.data()));
                        return visitor(candidate);
                    });
            if (!status.succeeded) return status;
            if (bucket == std::numeric_limits<std::uint64_t>::max()) break;
        }
    }
    return {true, {}};
}

}  // namespace videosc::dedup
