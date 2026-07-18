#include "DuplicateReportService.h"

#include "../concurrency/TaskThreadPool.h"
#include "../models/CoreModelCodec.h"
#include "../persistence/MySqlReadRepository.h"
#include "ImageSimilarityRules.h"
#include "PdqCandidateBuilder.h"
#include "StructuralVerificationCache.h"
#include "StructuralVerificationScheduler.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace videosc::dedup {
namespace {

constexpr std::uint8_t kGroupCodecVersion = 5;
constexpr std::uint8_t kSummaryCodecVersion = 2;
constexpr std::uint8_t kEvidenceCodecVersion = 4;
constexpr std::uint8_t kSimilarMetadataCodecVersion = 4;
constexpr std::uint8_t kImageRelationCodecVersion = 1;
constexpr std::uint32_t kMaximumGroupItems = 10'000'000;

/** @brief 当前毫秒与进程内序号组合，避免同一毫秒的报告代号冲突。 */
std::uint64_t NextGenerationId() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(milliseconds) * 1000ULL + (sequence.fetch_add(1) % 1000ULL);
}

/** @brief 把 UTF-16 严格转换为 UTF-8。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) throw std::runtime_error("Cannot encode report string as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode report string as UTF-8");
    }
    return result;
}

/** @brief 把 UTF-8 严格转换为 UTF-16。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) throw std::runtime_error("Cannot decode report UTF-8 string");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode report UTF-8 string");
    }
    return result;
}

/** @brief 以小端序追加数值。 */
template <typename Value>
void Append(std::string& output, const Value value) {
    static_assert(std::is_integral_v<Value> || std::is_floating_point_v<Value>);
    output.append(reinterpret_cast<const char*>(&value), sizeof(Value));
}

/** @brief 追加定长 SHA-512。 */
void AppendDigest(std::string& output, const Sha512Digest& digest) {
    output.append(reinterpret_cast<const char*>(digest.data()), digest.size());
}

/** @brief 追加有界 UTF-8 字符串。 */
void AppendString(std::string& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Report string is too long");
    }
    Append(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

/** @brief 重复报告二进制有界读取器。 */
class Reader final {
public:
    explicit Reader(const std::string& value) : value_(value) {}

    template <typename Value>
    Value Read() {
        static_assert(std::is_integral_v<Value> || std::is_floating_point_v<Value>);
        Require(sizeof(Value));
        Value result{};
        std::memcpy(&result, value_.data() + offset_, sizeof(Value));
        offset_ += sizeof(Value);
        return result;
    }

    Sha512Digest ReadDigest() {
        Sha512Digest digest{};
        Require(digest.size());
        std::memcpy(digest.data(), value_.data() + offset_, digest.size());
        offset_ += digest.size();
        return digest;
    }

    std::string ReadString() {
        const std::uint32_t size = Read<std::uint32_t>();
        Require(size);
        std::string result(value_.data() + offset_, size);
        offset_ += size;
        return result;
    }

    void RequireEnd() const {
        if (offset_ != value_.size()) throw std::runtime_error("Report record has trailing bytes");
    }

private:
    void Require(const std::size_t size) const {
        if (size > value_.size() - offset_) throw std::runtime_error("Report record is truncated");
    }

    const std::string& value_;
    std::size_t offset_ = 0;
};

/** @brief 序列化报告成员。 */
void AppendMember(std::string& output, const DuplicateMember& member) {
    Append(output, member.path_id);
    AppendDigest(output, member.content_sha512);
    Append(output, member.size_bytes);
    Append(output, member.width);
    Append(output, member.height);
    Append(output, member.bitrate);
    Append(output, member.last_write_time_utc_ms);
    Append(output, member.scan_root_priority);
    AppendString(output, WideToUtf8(member.path.wstring()));
    AppendString(output, WideToUtf8(member.storage_target_key));
    AppendString(output, WideToUtf8(member.thumbnail_path.wstring()));
    Append(output, static_cast<std::uint8_t>(member.media_kind));
    Append(output, static_cast<std::uint8_t>(member.image_dhash.has_value() ? 1 : 0));
    Append(output, member.image_dhash.value_or(0));
    Append(output, static_cast<std::uint8_t>(member.has_video_dhashes ? 1 : 0));
    for (const std::uint64_t hash : member.video_dhashes) Append(output, hash);
}

/** @brief 反序列化报告成员。 */
DuplicateMember ReadMember(Reader& reader, const std::uint8_t codec_version) {
    DuplicateMember member;
    member.path_id = reader.Read<std::uint64_t>();
    member.content_sha512 = reader.ReadDigest();
    member.size_bytes = reader.Read<std::uint64_t>();
    member.width = reader.Read<std::uint32_t>();
    member.height = reader.Read<std::uint32_t>();
    member.bitrate = reader.Read<std::uint64_t>();
    member.last_write_time_utc_ms = reader.Read<std::int64_t>();
    member.scan_root_priority = reader.Read<std::uint32_t>();
    member.path = Utf8ToWide(reader.ReadString());
    member.storage_target_key = Utf8ToWide(reader.ReadString());
    member.thumbnail_path = Utf8ToWide(reader.ReadString());
    if (codec_version >= 2) {
        const std::uint8_t mediaKind = reader.Read<std::uint8_t>();
        if (mediaKind > static_cast<std::uint8_t>(MediaKind::Audio)) {
            throw std::runtime_error("Invalid report media kind");
        }
        member.media_kind = static_cast<MediaKind>(mediaKind);
        const std::uint8_t hasImage = reader.Read<std::uint8_t>();
        if (hasImage > 1) throw std::runtime_error("Invalid report image dHash flag");
        const std::uint64_t imageDhash = reader.Read<std::uint64_t>();
        if (hasImage != 0) member.image_dhash = imageDhash;
        const std::uint8_t hasVideo = reader.Read<std::uint8_t>();
        if (hasVideo > 1) throw std::runtime_error("Invalid report video dHash flag");
        member.has_video_dhashes = hasVideo != 0;
        for (std::uint64_t& hash : member.video_dhashes) hash = reader.Read<std::uint64_t>();
    }
    return member;
}

/** @brief 序列化汉明距离证据。 */
void AppendEvidence(std::string& output, const SimilarityEvidence& evidence) {
    Append(output, evidence.left_path_id);
    Append(output, evidence.right_path_id);
    output.append(reinterpret_cast<const char*>(evidence.frame_distances.data()), evidence.frame_distances.size());
    Append(output, evidence.compared_frame_count);
    Append(output, evidence.average_hamming_distance);
    Append(output, evidence.duration_difference_ms);
    Append(output, static_cast<std::uint8_t>(evidence.image_quality_class));
    Append(output, evidence.image_pdq_hamming_distance);
    Append(output, evidence.image_dhash_hamming_distance);
    Append(output, static_cast<std::uint8_t>(evidence.image_used_dhash_fallback ? 1 : 0));
    Append(output, evidence.image_zoned_passing_tiles);
    Append(output, evidence.image_zoned_ignored_tiles);
    Append(output, evidence.image_zoned_trimmed_distance_sum);
    Append(output, evidence.image_zoned_retained_tiles);
    Append(output, evidence.image_global_edge_zncc_millionths);
    Append(output, evidence.image_trimmed_block_score_millionths);
    Append(output, evidence.image_passing_block_percent_millionths);
}

/** @brief 反序列化视觉证据；旧报告只包含 dHash 字段。 */
SimilarityEvidence ReadEvidence(Reader& reader,
                                  const bool has_image_three_stage_fields,
                                  const bool has_quality_class,
                                  const bool has_dhash_fallback) {
    SimilarityEvidence evidence;
    evidence.left_path_id = reader.Read<std::uint64_t>();
    evidence.right_path_id = reader.Read<std::uint64_t>();
    for (std::uint8_t& distance : evidence.frame_distances) distance = reader.Read<std::uint8_t>();
    evidence.compared_frame_count = reader.Read<std::uint32_t>();
    evidence.average_hamming_distance = reader.Read<double>();
    evidence.duration_difference_ms = reader.Read<std::int64_t>();
    if (has_quality_class) {
        const std::uint8_t qualityClass = reader.Read<std::uint8_t>();
        if (qualityClass > static_cast<std::uint8_t>(ImageQualityClass::LowQuality)) {
            throw std::runtime_error("Invalid image quality class");
        }
        evidence.image_quality_class = static_cast<ImageQualityClass>(qualityClass);
    }
    if (has_image_three_stage_fields) {
        evidence.image_pdq_hamming_distance = reader.Read<std::uint16_t>();
        if (has_dhash_fallback) {
            evidence.image_dhash_hamming_distance = reader.Read<std::uint8_t>();
            const std::uint8_t usedFallback = reader.Read<std::uint8_t>();
            if (usedFallback > 1) throw std::runtime_error("Invalid image dHash fallback flag");
            evidence.image_used_dhash_fallback = usedFallback != 0;
        }
        evidence.image_zoned_passing_tiles = reader.Read<std::uint8_t>();
        evidence.image_zoned_ignored_tiles = reader.Read<std::uint8_t>();
        evidence.image_zoned_trimmed_distance_sum = reader.Read<std::uint16_t>();
        evidence.image_zoned_retained_tiles = reader.Read<std::uint8_t>();
        evidence.image_global_edge_zncc_millionths = reader.Read<std::uint32_t>();
        evidence.image_trimmed_block_score_millionths = reader.Read<std::uint32_t>();
        evidence.image_passing_block_percent_millionths = reader.Read<std::uint32_t>();
    }
    return evidence;
}

/** @brief 将完整组编码为带版本的紧凑二进制。 */
std::string SerializeGroup(const DuplicateGroup& group) {
    if (group.members.size() > kMaximumGroupItems || group.evidence.size() > kMaximumGroupItems ||
        group.selected_for_deletion.size() > kMaximumGroupItems) {
        throw std::runtime_error("Duplicate group is too large");
    }
    std::string output;
    output.reserve(256 + group.members.size() * 256);
    Append(output, kGroupCodecVersion);
    Append(output, group.group_id);
    Append(output, static_cast<std::uint8_t>(group.kind));
    Append(output, group.reclaimable_bytes);
    Append(output, static_cast<std::uint8_t>(group.retained_path_id.has_value() ? 1 : 0));
    Append(output, group.retained_path_id.value_or(0));
    AppendString(output, group.algorithm_version);
    Append(output, static_cast<std::uint32_t>(group.members.size()));
    for (const DuplicateMember& member : group.members) AppendMember(output, member);
    Append(output, static_cast<std::uint32_t>(group.evidence.size()));
    for (const SimilarityEvidence& evidence : group.evidence) AppendEvidence(output, evidence);
    Append(output, static_cast<std::uint32_t>(group.selected_for_deletion.size()));
    for (const std::uint64_t pathId : group.selected_for_deletion) Append(output, pathId);
    return output;
}

/** @brief 解码完整重复组并拒绝截断、越界枚举或尾随数据。 */
std::optional<DuplicateGroup> DeserializeGroup(const std::string& value, std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t codecVersion = reader.Read<std::uint8_t>();
        if (codecVersion == 1) {
            throw std::runtime_error(
                "报告版本过旧，不包含媒体类型和 dHash，请重新生成重复报告。");
        }
        if (codecVersion != 2 && codecVersion != 3 && codecVersion != 4 &&
            codecVersion != kGroupCodecVersion) {
            throw std::runtime_error("Unsupported report group");
        }
        DuplicateGroup group;
        group.group_id = reader.Read<std::uint64_t>();
        const std::uint8_t kind = reader.Read<std::uint8_t>();
        if (kind > static_cast<std::uint8_t>(DuplicateGroupKind::SimilarVideo)) {
            throw std::runtime_error("Invalid duplicate group kind");
        }
        group.kind = static_cast<DuplicateGroupKind>(kind);
        group.reclaimable_bytes = reader.Read<std::uint64_t>();
        const std::uint8_t hasRetained = reader.Read<std::uint8_t>();
        const std::uint64_t retained = reader.Read<std::uint64_t>();
        if (hasRetained > 1) throw std::runtime_error("Invalid retained member flag");
        if (hasRetained != 0) group.retained_path_id = retained;
        group.algorithm_version = reader.ReadString();
        const std::uint32_t memberCount = reader.Read<std::uint32_t>();
        if (memberCount > kMaximumGroupItems) throw std::runtime_error("Duplicate group member count is too large");
        group.members.reserve(memberCount);
        for (std::uint32_t index = 0; index < memberCount; ++index) {
            group.members.push_back(ReadMember(reader, codecVersion));
        }
        const std::uint32_t evidenceCount = reader.Read<std::uint32_t>();
        if (evidenceCount > kMaximumGroupItems) throw std::runtime_error("Duplicate group evidence count is too large");
        group.evidence.reserve(evidenceCount);
        for (std::uint32_t index = 0; index < evidenceCount; ++index) {
            group.evidence.push_back(ReadEvidence(
                reader, codecVersion >= 3, codecVersion >= 4, codecVersion >= 5));
        }
        const std::uint32_t selectedCount = reader.Read<std::uint32_t>();
        if (selectedCount > kMaximumGroupItems) throw std::runtime_error("Duplicate group selection count is too large");
        group.selected_for_deletion.reserve(selectedCount);
        for (std::uint32_t index = 0; index < selectedCount; ++index) {
            group.selected_for_deletion.push_back(reader.Read<std::uint64_t>());
        }
        reader.RequireEnd();
        error.clear();
        return group;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

/** @brief 以固定宽度十六进制编码 uint64，保持 RocksDB 字典序与数值序一致。 */
std::string SerializeSummary(const DuplicateGroup& group) {
    std::string output;
    output.reserve(2 + sizeof(std::uint64_t) * 3 + sizeof(double));
    Append(output, kSummaryCodecVersion);
    Append(output, group.group_id);
    Append(output, static_cast<std::uint64_t>(group.members.size()));
    Append(output, group.reclaimable_bytes);
    const bool hasAverage = group.kind != DuplicateGroupKind::ExactSha512;
    double average = 0.0;
    if (hasAverage && !group.evidence.empty()) {
        for (const SimilarityEvidence& evidence : group.evidence) {
            if (!std::isfinite(evidence.average_hamming_distance)) {
                throw std::runtime_error("Invalid similarity evidence average distance");
            }
            average += evidence.average_hamming_distance;
        }
        average /= static_cast<double>(group.evidence.size());
    }
    Append(output, static_cast<std::uint8_t>(hasAverage ? 1 : 0));
    Append(output, average);
    return output;
}

std::optional<ReportGroupSummary> DeserializeSummary(const std::uint64_t ordinal,
                                                       const std::string& value,
                                                       std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t version = reader.Read<std::uint8_t>();
        if (version != 1 && version != kSummaryCodecVersion) {
            throw std::runtime_error("Unsupported report summary");
        }
        ReportGroupSummary summary;
        summary.ordinal = ordinal;
        summary.group_id = reader.Read<std::uint64_t>();
        summary.member_count = reader.Read<std::uint64_t>();
        summary.reclaimable_bytes = reader.Read<std::uint64_t>();
        if (version >= 2) {
            const std::uint8_t hasAverage = reader.Read<std::uint8_t>();
            if (hasAverage > 1) throw std::runtime_error("Invalid report summary distance flag");
            summary.has_average_hamming_distance = hasAverage != 0;
            summary.average_hamming_distance = reader.Read<double>();
            if (!std::isfinite(summary.average_hamming_distance) ||
                summary.average_hamming_distance < 0.0 ||
                summary.average_hamming_distance > 64.0) {
                throw std::runtime_error("Invalid report summary average distance");
            }
        }
        reader.RequireEnd();
        error.clear();
        return summary;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

/** @brief 编码一套图片三级阈值。 */
void AppendImageProfile(std::string& output, const ImageSimilarityThresholdProfile& profile) {
    Append(output, profile.pdq_max_hamming_distance);
    Append(output, profile.fallback_dhash_max_hamming_distance);
    Append(output, profile.zoned_phash_tile_max_distance);
    Append(output, profile.zoned_phash_min_passing_tiles);
    Append(output, profile.zoned_phash_max_ignored_tiles);
    Append(output, profile.zoned_phash_trimmed_mean_max);
    Append(output, profile.structural_global_edge_min_millionths);
    Append(output, profile.structural_trimmed_block_min_millionths);
    Append(output, profile.structural_block_pass_score_millionths);
    Append(output, profile.structural_min_passing_percent_millionths);
}

/** @brief 解码一套图片三级阈值。 */
ImageSimilarityThresholdProfile ReadImageProfile(Reader& reader) {
    ImageSimilarityThresholdProfile profile;
    profile.pdq_max_hamming_distance = reader.Read<std::uint32_t>();
    profile.fallback_dhash_max_hamming_distance = reader.Read<std::uint32_t>();
    profile.zoned_phash_tile_max_distance = reader.Read<std::uint32_t>();
    profile.zoned_phash_min_passing_tiles = reader.Read<std::uint32_t>();
    profile.zoned_phash_max_ignored_tiles = reader.Read<std::uint32_t>();
    profile.zoned_phash_trimmed_mean_max = reader.Read<std::uint32_t>();
    profile.structural_global_edge_min_millionths = reader.Read<std::uint32_t>();
    profile.structural_trimmed_block_min_millionths = reader.Read<std::uint32_t>();
    profile.structural_block_pass_score_millionths = reader.Read<std::uint32_t>();
    profile.structural_min_passing_percent_millionths = reader.Read<std::uint32_t>();
    return profile;
}

/** @brief 校验报告内单套图片阈值的有界范围。 */
bool IsValidImageProfile(const ImageSimilarityThresholdProfile& profile) noexcept {
    return profile.pdq_max_hamming_distance <= 31 &&
           profile.fallback_dhash_max_hamming_distance <= 15 &&
           profile.zoned_phash_tile_max_distance <= 64 &&
           profile.zoned_phash_min_passing_tiles >= 1 &&
           profile.zoned_phash_min_passing_tiles <= 16 &&
           profile.zoned_phash_max_ignored_tiles <= 15 &&
           profile.zoned_phash_min_passing_tiles + profile.zoned_phash_max_ignored_tiles >= 16 &&
           profile.zoned_phash_trimmed_mean_max <= 64 &&
           profile.structural_global_edge_min_millionths <= 1'000'000 &&
           profile.structural_trimmed_block_min_millionths <= 1'000'000 &&
           profile.structural_block_pass_score_millionths <= 1'000'000 &&
           profile.structural_min_passing_percent_millionths <= 1'000'000;
}

/** @brief 低质量图片配置必须逐项不弱于标准配置。 */
bool IsLowQualityProfileStrict(const ImageSimilarityThresholdProfile& standard,
                               const ImageSimilarityThresholdProfile& low) noexcept {
    return low.pdq_max_hamming_distance <= standard.pdq_max_hamming_distance &&
           low.fallback_dhash_max_hamming_distance <=
               standard.fallback_dhash_max_hamming_distance &&
           low.zoned_phash_tile_max_distance <= standard.zoned_phash_tile_max_distance &&
           low.zoned_phash_min_passing_tiles >= standard.zoned_phash_min_passing_tiles &&
           low.zoned_phash_max_ignored_tiles <= standard.zoned_phash_max_ignored_tiles &&
           low.zoned_phash_trimmed_mean_max <= standard.zoned_phash_trimmed_mean_max &&
           low.structural_global_edge_min_millionths >= standard.structural_global_edge_min_millionths &&
           low.structural_trimmed_block_min_millionths >=
               standard.structural_trimmed_block_min_millionths &&
           low.structural_block_pass_score_millionths >=
               standard.structural_block_pass_score_millionths &&
           low.structural_min_passing_percent_millionths >=
               standard.structural_min_passing_percent_millionths;
}

/** @brief 编码相似报告规则快照。 */
std::string SerializeSimilarMetadata(const SimilarReportMetadata& metadata) {
    std::string output;
    Append(output, kSimilarMetadataCodecVersion);
    Append(output, metadata.report_schema_version);
    Append(output, metadata.image_max_hamming_distance);
    Append(output, metadata.video_max_average_hamming_distance);
    Append(output, metadata.image_aspect_ratio_tolerance_percent);
    Append(output, metadata.validation_worker_threads);
    Append(output, metadata.generated_utc_ms);
    AppendString(output, metadata.media_algorithm_version);
    AppendString(output, metadata.image_bucket_index_version);
    AppendString(output, metadata.video_bucket_index_version);
    AppendString(output, metadata.image_grouping_rule);
    AppendString(output, metadata.video_grouping_rule);
    AppendString(output, metadata.image_aspect_ratio_rule);
    AppendString(output, metadata.image_relation_rule);
    const ImageSimilarityConfig& image = metadata.image_similarity;
    Append(output, image.aspect_ratio_tolerance_percent);
    Append(output, image.pdq_min_quality);
    AppendImageProfile(output, image.standard_profile);
    AppendImageProfile(output, image.low_quality_profile);
    Append(output, metadata.structural_worker_threads);
    Append(output, metadata.structural_cache_mib);
    Append(output, image.candidate_memory_mib);
    Append(output, image.candidate_temp_mib);
    Append(output, image.candidate_max_pairs);
    Append(output, image.hot_signature_max_members);
    Append(output, image.hot_signature_max_pairs);
    Append(output, image.candidate_write_batch_size);
    Append(output, image.candidate_cancel_check_stride);
    Append(output, static_cast<std::uint8_t>(image.require_complete_features ? 1 : 0));
    Append(output, static_cast<std::uint8_t>(image.allow_partial_reports ? 1 : 0));
    Append(output, static_cast<std::uint8_t>(metadata.image_uses_three_stage_verification ? 1 : 0));
    Append(output, metadata.image_scope_total);
    Append(output, metadata.image_features_complete);
    Append(output, metadata.image_features_incomplete);
    Append(output, static_cast<std::uint8_t>(metadata.partial_scope_published ? 1 : 0));
    Append(output, metadata.deferred_hot_signatures);
    Append(output, metadata.candidate_peak_bytes);
    Append(output, metadata.candidate_pairs);
    AppendString(output, metadata.image_primary_rule);
    AppendString(output, metadata.image_secondary_rule);
    AppendString(output, metadata.image_tertiary_rule);
    AppendString(output, metadata.popcount_path);
    return output;
}

/** @brief 解码相似报告规则快照并拒绝未知版本。 */
std::optional<SimilarReportMetadata> DeserializeSimilarMetadata(const std::string& value,
                                                                 std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t codecVersion = reader.Read<std::uint8_t>();
        if (codecVersion != 2 && codecVersion != 3 && codecVersion != kSimilarMetadataCodecVersion) {
            throw std::runtime_error("Unsupported similar report metadata");
        }
        SimilarReportMetadata metadata;
        metadata.report_schema_version = reader.Read<std::uint32_t>();
        metadata.image_max_hamming_distance = reader.Read<std::uint32_t>();
        metadata.video_max_average_hamming_distance = reader.Read<std::uint32_t>();
        metadata.image_aspect_ratio_tolerance_percent = reader.Read<std::uint32_t>();
        metadata.validation_worker_threads = reader.Read<std::uint32_t>();
        metadata.generated_utc_ms = reader.Read<std::int64_t>();
        metadata.media_algorithm_version = reader.ReadString();
        metadata.image_bucket_index_version = reader.ReadString();
        metadata.video_bucket_index_version = reader.ReadString();
        metadata.image_grouping_rule = reader.ReadString();
        metadata.video_grouping_rule = reader.ReadString();
        metadata.image_aspect_ratio_rule = reader.ReadString();
        metadata.image_relation_rule = reader.ReadString();
        if (codecVersion == 3) {
            ImageSimilarityConfig& image = metadata.image_similarity;
            image.aspect_ratio_tolerance_percent = reader.Read<std::uint32_t>();
            image.standard_profile.pdq_max_hamming_distance = reader.Read<std::uint32_t>();
            image.pdq_min_quality = reader.Read<std::uint32_t>();
            image.standard_profile.zoned_phash_tile_max_distance = reader.Read<std::uint32_t>();
            image.standard_profile.zoned_phash_min_passing_tiles = reader.Read<std::uint32_t>();
            image.standard_profile.zoned_phash_max_ignored_tiles = reader.Read<std::uint32_t>();
            image.standard_profile.zoned_phash_trimmed_mean_max = reader.Read<std::uint32_t>();
            image.standard_profile.structural_global_edge_min_millionths = reader.Read<std::uint32_t>();
            image.standard_profile.structural_trimmed_block_min_millionths = reader.Read<std::uint32_t>();
            image.standard_profile.structural_block_pass_score_millionths = reader.Read<std::uint32_t>();
            image.standard_profile.structural_min_passing_percent_millionths = reader.Read<std::uint32_t>();
            metadata.structural_worker_threads = reader.Read<std::uint32_t>();
            metadata.structural_cache_mib = reader.Read<std::uint32_t>();
            const std::uint8_t threeStage = reader.Read<std::uint8_t>();
            if (threeStage > 1) throw std::runtime_error("Invalid image verification mode");
            metadata.image_uses_three_stage_verification = threeStage != 0;
            metadata.image_primary_rule = reader.ReadString();
            metadata.image_secondary_rule = reader.ReadString();
            metadata.image_tertiary_rule = reader.ReadString();
            metadata.popcount_path = reader.ReadString();
        } else if (codecVersion == 4) {
            ImageSimilarityConfig& image = metadata.image_similarity;
            image.aspect_ratio_tolerance_percent = reader.Read<std::uint32_t>();
            image.pdq_min_quality = reader.Read<std::uint32_t>();
            image.standard_profile = ReadImageProfile(reader);
            image.low_quality_profile = ReadImageProfile(reader);
            metadata.structural_worker_threads = reader.Read<std::uint32_t>();
            metadata.structural_cache_mib = reader.Read<std::uint32_t>();
            image.candidate_memory_mib = reader.Read<std::uint32_t>();
            image.candidate_temp_mib = reader.Read<std::uint32_t>();
            image.candidate_max_pairs = reader.Read<std::uint64_t>();
            image.hot_signature_max_members = reader.Read<std::uint32_t>();
            image.hot_signature_max_pairs = reader.Read<std::uint64_t>();
            image.candidate_write_batch_size = reader.Read<std::uint32_t>();
            image.candidate_cancel_check_stride = reader.Read<std::uint32_t>();
            const std::uint8_t requireComplete = reader.Read<std::uint8_t>();
            const std::uint8_t allowPartial = reader.Read<std::uint8_t>();
            const std::uint8_t threeStage = reader.Read<std::uint8_t>();
            if (requireComplete > 1 || allowPartial > 1 || threeStage > 1) {
                throw std::runtime_error("Invalid image report mode flags");
            }
            image.require_complete_features = requireComplete != 0;
            image.allow_partial_reports = allowPartial != 0;
            metadata.image_uses_three_stage_verification = threeStage != 0;
            metadata.image_scope_total = reader.Read<std::uint64_t>();
            metadata.image_features_complete = reader.Read<std::uint64_t>();
            metadata.image_features_incomplete = reader.Read<std::uint64_t>();
            const std::uint8_t partialConfirmed = reader.Read<std::uint8_t>();
            if (partialConfirmed > 1) throw std::runtime_error("Invalid partial scope flag");
            metadata.partial_scope_published = partialConfirmed != 0;
            metadata.deferred_hot_signatures = reader.Read<std::uint64_t>();
            metadata.candidate_peak_bytes = reader.Read<std::uint64_t>();
            metadata.candidate_pairs = reader.Read<std::uint64_t>();
            metadata.image_primary_rule = reader.ReadString();
            metadata.image_secondary_rule = reader.ReadString();
            metadata.image_tertiary_rule = reader.ReadString();
            metadata.popcount_path = reader.ReadString();
        } else {
            metadata.image_uses_three_stage_verification = false;
        }
        reader.RequireEnd();
        const bool legacyRules = codecVersion == 2 &&
            metadata.report_schema_version == 2 &&
            metadata.image_max_hamming_distance <= 15 &&
            metadata.image_bucket_index_version == "image-bucket-v2" &&
            metadata.image_grouping_rule == "image-complete-link-disjoint-v2" &&
            metadata.image_aspect_ratio_rule == "relative-ratio-v1" &&
            metadata.image_relation_rule == "image-signature-cross-group-v1";
        const bool currentRules = codecVersion == 3 &&
            metadata.report_schema_version == 3 &&
            metadata.image_uses_three_stage_verification &&
            metadata.image_similarity.standard_profile.pdq_max_hamming_distance <= 31 &&
            metadata.image_bucket_index_version == "pdq-16x16-neighbor-v1" &&
            metadata.image_grouping_rule == "image-three-stage-complete-link-v1" &&
            metadata.image_aspect_ratio_rule == "relative-ratio-1pct-v1" &&
            metadata.image_relation_rule == "image-pdq-structural-cross-group-v1";
        const bool v4Rules = codecVersion == 4 &&
            metadata.report_schema_version == 4 &&
            metadata.image_uses_three_stage_verification &&
            metadata.image_bucket_index_version == "pdq16-r1-plus-dhash-dynamic-v3" &&
            metadata.image_grouping_rule == "image-three-stage-complete-link-v2" &&
            metadata.image_aspect_ratio_rule == "relative-ratio-1pct-v1" &&
            metadata.image_relation_rule == "image-pdq-structural-cross-group-v2" &&
            metadata.image_primary_rule == "pdq-256-or-dhash-watermark-fallback-v2" &&
            metadata.image_secondary_rule == "zoned-phash-4x4-v1" &&
            metadata.image_tertiary_rule == "gray-sobel-block-structure-v1" &&
            metadata.image_similarity.aspect_ratio_tolerance_percent <= 10 &&
            metadata.image_similarity.pdq_min_quality <= 100 &&
            IsValidImageProfile(metadata.image_similarity.standard_profile) &&
            IsValidImageProfile(metadata.image_similarity.low_quality_profile) &&
            IsLowQualityProfileStrict(metadata.image_similarity.standard_profile,
                                      metadata.image_similarity.low_quality_profile) &&
            metadata.image_similarity.candidate_memory_mib >= 16 &&
            metadata.image_similarity.candidate_temp_mib >= 64 &&
            metadata.image_similarity.candidate_max_pairs != 0 &&
            metadata.image_similarity.hot_signature_max_members >= 2 &&
            metadata.image_similarity.hot_signature_max_pairs != 0 &&
            metadata.deferred_hot_signatures == 0 &&
            metadata.image_scope_total == metadata.image_features_complete +
                                              metadata.image_features_incomplete &&
            (metadata.image_features_incomplete == 0 || metadata.partial_scope_published);
        if ((!legacyRules && !currentRules && !v4Rules) ||
            metadata.video_max_average_hamming_distance > 15 ||
            metadata.image_aspect_ratio_tolerance_percent > 100 ||
            metadata.validation_worker_threads == 0 ||
            metadata.validation_worker_threads > 256 ||
            metadata.video_bucket_index_version != "video-bucket-v2" ||
            metadata.video_grouping_rule != "video-average-complete-link-disjoint-v1" ||
            metadata.structural_worker_threads == 0 ||
            metadata.structural_worker_threads > 256) {
            throw std::runtime_error("Unsupported strict similarity report rules");
        }
        error.clear();
        return metadata;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

/** @brief 编码一个方向的跨严格组图片签名关系。 */
std::string SerializeImageRelation(const SimilarImageRelationSummary& relation) {
    std::string output;
    Append(output, kImageRelationCodecVersion);
    Append(output, relation.current_group_id);
    Append(output, relation.neighbor_group_id);
    AppendString(output, relation.current_representative_sha512);
    AppendString(output, relation.neighbor_representative_sha512);
    Append(output, relation.current_image_dhash);
    Append(output, relation.neighbor_image_dhash);
    Append(output, relation.hamming_distance);
    Append(output, relation.neighbor_active_member_count);
    Append(output, static_cast<std::uint8_t>(relation.neighbor_group_in_main_report ? 1 : 0));
    return output;
}

/** @brief 解码一个方向的跨严格组图片签名关系。 */
std::optional<SimilarImageRelationSummary> DeserializeImageRelation(
    const std::uint64_t ordinal,
    const std::string& value,
    std::string& error) {
    try {
        Reader reader(value);
        if (reader.Read<std::uint8_t>() != kImageRelationCodecVersion) {
            throw std::runtime_error("Unsupported image similarity relation");
        }
        SimilarImageRelationSummary relation;
        relation.ordinal = ordinal;
        relation.current_group_id = reader.Read<std::uint64_t>();
        relation.neighbor_group_id = reader.Read<std::uint64_t>();
        relation.current_representative_sha512 = reader.ReadString();
        relation.neighbor_representative_sha512 = reader.ReadString();
        relation.current_image_dhash = reader.Read<std::uint64_t>();
        relation.neighbor_image_dhash = reader.Read<std::uint64_t>();
        relation.hamming_distance = reader.Read<std::uint8_t>();
        relation.neighbor_active_member_count = reader.Read<std::uint64_t>();
        const std::uint8_t neighborVisible = reader.Read<std::uint8_t>();
        if (neighborVisible > 1) throw std::runtime_error("Invalid neighbor visibility flag");
        relation.neighbor_group_in_main_report = neighborVisible != 0;
        reader.RequireEnd();
        error.clear();
        return relation;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

/** @brief 编码一个独立的报告成员值。 */
std::string SerializeStandaloneMember(const DuplicateMember& member) {
    std::string output;
    Append(output, kGroupCodecVersion);
    AppendMember(output, member);
    return output;
}

/** @brief 解码一个独立的报告成员值。 */
std::optional<DuplicateMember> DeserializeStandaloneMember(const std::string& value,
                                                            std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t codecVersion = reader.Read<std::uint8_t>();
        if (codecVersion != 4 && codecVersion != kGroupCodecVersion) {
            throw std::runtime_error("Unsupported similarity signature member");
        }
        DuplicateMember member = ReadMember(reader, codecVersion);
        reader.RequireEnd();
        error.clear();
        return member;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::string Hex64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        const unsigned shift = static_cast<unsigned>((result.size() - 1 - index) * 4);
        result[index] = digits[(value >> shift) & 0x0F];
    }
    return result;
}

/** @brief 严格解析固定宽度十六进制 uint64。 */
std::optional<std::uint64_t> ParseHex64(const std::string& value) {
    if (value.size() != 16) return std::nullopt;
    std::uint64_t result = 0;
    for (const char character : value) {
        result <<= 4;
        if (character >= '0' && character <= '9') result |= static_cast<std::uint64_t>(character - '0');
        else if (character >= 'a' && character <= 'f') result |= static_cast<std::uint64_t>(character - 'a' + 10);
        else return std::nullopt;
    }
    return result;
}

/** @brief 报告类型稳定键名。 */
const char* KindName(const DuplicateReportKind kind) {
    return kind == DuplicateReportKind::Exact ? "exact" : "similar";
}

/** @brief 报告代前缀。 */
std::string GenerationPrefix(const DuplicateReportKind kind, const std::uint64_t generationId) {
    return std::string("report/") + KindName(kind) + "/" + Hex64(generationId) + "/";
}

/** @brief 当前发布代键。 */
std::string ActiveKey(const DuplicateReportKind kind) {
    return std::string("report/") + KindName(kind) + "/active";
}

/** @brief 读取十进制计数值。 */
std::optional<std::uint64_t> ParseDecimal(const std::string& value) {
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

/** @brief 活跃路径与 MySQL active=1 规则保持一致。 */
bool IsActivePath(const FilePathRecord& record) {
    return record.sha512.has_value() &&
           (record.state == FilePathState::Available || record.state == FilePathState::Unchanged);
}

/** @brief 从当前 MySQL 报告代的临时内容缓存加载记录。 */
std::optional<ShaFileData> LoadWorkContent(RocksStore& store,
                                          const std::string& work_prefix,
                                          const Sha512Digest& digest,
                                          std::string& error) {
    std::string value;
    const RocksStatus status = store.Get(RocksColumnFamily::ExactIndex,
                                         work_prefix + "content/" + Sha512ToHex(digest),
                                         value);
    if (!status.succeeded) {
        error = status.message;
        return std::nullopt;
    }
    return CoreModelCodec::DeserializeShaFileData(value, error);
}

/** @brief 相似报告工作键前缀。 */
std::string WorkPrefix(const std::uint64_t generationId) {
    return "report/similar/work/" + Hex64(generationId) + "/";
}

/** @brief 返回规范化无分隔符的两个 SHA-512 十六进制组合键。 */
std::string NormalizedPairKey(const std::string& left, const std::string& right) {
    return left < right ? left + right : right + left;
}

/** @brief 当前 UTC Unix 毫秒时间戳。 */
std::int64_t CurrentUtcMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** @brief 为已通过前两筛的图片对创建可继续补充结构分的证据。 */
SimilarityEvidence ImageEvidence(const ShaFileData& left,
                                 const ShaFileData& right,
                                 const ZonedPHashEvidence& zoned,
                                 const bool force_scalar,
                                 const ImageQualityClass quality_class,
                                 const bool used_dhash_fallback) {
    SimilarityEvidence evidence;
    evidence.compared_frame_count = 1;
    evidence.image_quality_class = quality_class;
    const std::uint32_t pdqDistance = ImageSimilarityRules::PdqHammingDistance(
        *left.image_pdq_hash, *right.image_pdq_hash, force_scalar);
    evidence.image_pdq_hamming_distance = static_cast<std::uint16_t>(pdqDistance);
    if (left.image_dhash.has_value() && right.image_dhash.has_value()) {
        evidence.image_dhash_hamming_distance = DHashRules::HammingDistance(
            *left.image_dhash, *right.image_dhash);
    }
    evidence.image_used_dhash_fallback = used_dhash_fallback;
    evidence.frame_distances[0] = static_cast<std::uint8_t>((std::min)(pdqDistance, 255U));
    evidence.average_hamming_distance = static_cast<double>(pdqDistance);
    evidence.image_zoned_passing_tiles = static_cast<std::uint8_t>(zoned.passing_tiles);
    evidence.image_zoned_ignored_tiles = static_cast<std::uint8_t>(zoned.ignored_tiles);
    evidence.image_zoned_trimmed_distance_sum =
        static_cast<std::uint16_t>(zoned.trimmed_distance_sum);
    evidence.image_zoned_retained_tiles = static_cast<std::uint8_t>(zoned.retained_tiles);
    return evidence;
}

/** @brief 内容是否具有可参与相似报告的有效视觉签名。 */
bool IsUsableVisual(const ShaFileData& data) {
    if (data.media_kind == MediaKind::Image) {
        return ImageSimilarityRules::HasCompleteFeatures(data);
    }
    return ClassifyVisualDHash(data) == VisualDHashStatus::Valid && !data.static_visual;
}

/** @brief 图片使用独立摘要、视频压缩相同签名，保证图片都经过结构直验。 */
std::string VisualSignatureKey(const ShaFileData& data) {
    if (IsUsableVisual(data) && data.media_kind == MediaKind::Image) {
        return "I/" + Sha512ToHex(data.sha512);
    }
    if (IsUsableVisual(data) && data.media_kind == MediaKind::Video) {
        std::string key = "V/" + Hex64(static_cast<std::uint64_t>((std::max<std::int64_t>)(0, data.video_duration_ms)));
        for (const std::uint64_t hash : data.video_dhashes) key += Hex64(hash);
        return key;
    }
    return {};
}

/** @brief 对两个内容执行最终真实距离判断并返回组类型。 */
std::optional<SimilarityEvidence> CompareVisuals(const ShaFileData& left,
                                                 const ShaFileData& right,
                                                 DuplicateGroupKind& kind,
                                                 const DHashSimilarityConfig& config) {
    if (left.media_kind == MediaKind::Video && right.media_kind == MediaKind::Video) {
        kind = DuplicateGroupKind::SimilarVideo;
        return DHashRules::CompareVideos(
            left, right, 0, 0, config.video_max_average_hamming_distance);
    }
    return std::nullopt;
}

/** @brief 编码临时相似边，首字节记录图片或视频类别。 */
std::string SerializeEdge(const DuplicateGroupKind kind, const SimilarityEvidence& evidence) {
    std::string result;
    Append(result, kEvidenceCodecVersion);
    Append(result, static_cast<std::uint8_t>(kind));
    AppendEvidence(result, evidence);
    return result;
}

/** @brief 解码临时相似边。 */
bool DeserializeEdge(const std::string& value,
                     DuplicateGroupKind& kind,
                     SimilarityEvidence& evidence,
                     std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t codecVersion = reader.Read<std::uint8_t>();
        if (codecVersion != 1 && codecVersion != 2 && codecVersion != 3 &&
            codecVersion != kEvidenceCodecVersion) {
            throw std::runtime_error("Unsupported evidence");
        }
        const std::uint8_t kindValue = reader.Read<std::uint8_t>();
        if (kindValue != static_cast<std::uint8_t>(DuplicateGroupKind::SimilarImage) &&
            kindValue != static_cast<std::uint8_t>(DuplicateGroupKind::SimilarVideo)) {
            throw std::runtime_error("Invalid similarity evidence kind");
        }
        kind = static_cast<DuplicateGroupKind>(kindValue);
        evidence = ReadEvidence(
            reader, codecVersion >= 2, codecVersion >= 3, codecVersion >= 4);
        reader.RequireEnd();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

/** @brief 使用 SHA-512 根摘要生成稳定报告组 ID。 */
std::uint64_t StableGroupId(const std::string& rootHex, const DuplicateGroupKind kind) {
    std::uint64_t value = 1469598103934665603ULL ^ static_cast<std::uint64_t>(kind);
    for (const unsigned char byte : rootHex) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

/** @brief 进度回调隔离，调用方异常不会破坏报告任务。 */
void PublishProgress(const DuplicateReportGenerator::ProgressCallback& callback,
                     const DuplicateReportProgress& progress) noexcept {
    if (!callback) return;
    try {
        callback(progress);
    } catch (...) {
    }
}

/**
 * @brief 隔离报告诊断回调，日志窗口或调用方异常不得破坏报告任务。
 * @param callback 可选诊断接收者。
 * @param diagnostic 已脱敏的结构化诊断。
 */
void PublishDiagnostic(const DuplicateReportGenerator::DiagnosticCallback& callback,
                       const DuplicateReportDiagnostic& diagnostic) noexcept {
    if (!callback) return;
    try {
        callback(diagnostic);
    } catch (...) {
        // 诊断镜像失败不改变候选跳过或报告发布结果。
    }
}

/**
 * @brief 以固定时间间隔发布轻量进度，并在阶段边界强制刷新。
 *
 * 该对象只在报告工作线程中使用，不持有存储锁；回调异常由 PublishProgress 隔离。
 */
class ThrottledProgressPublisher final {
public:
    /**
     * @brief 绑定报告回调和当前线程独占的进度快照。
     * @param callback GUI 或调用方提供的轻量回调。
     * @param progress 当前报告生成期间持续更新的快照。
     */
    ThrottledProgressPublisher(const DuplicateReportGenerator::ProgressCallback& callback,
                               DuplicateReportProgress& progress)
        : callback_(callback), progress_(progress) {}

    /**
     * @brief 切换当前阶段并立即发布初始快照。
     * @param stage 稳定阶段键。
     * @param stage_index 当前阶段序号。
     * @param stage_count 阶段总数。
     * @param total_known 是否具有可靠总量。
     * @param total 当前阶段总量。
     */
    void BeginStage(const char* stage,
                    const std::uint32_t stage_index,
                    const std::uint32_t stage_count,
                    const bool total_known,
                    const std::uint64_t total) noexcept {
        progress_.stage = stage;
        progress_.stage_index = stage_index;
        progress_.stage_count = stage_count;
        progress_.stage_processed = 0;
        progress_.stage_total = total;
        progress_.stage_total_known = total_known;
        Publish(true);
    }

    /**
     * @brief 按约 100 ms 时间间隔发布进度。
     * @param force 为 true 时忽略时间间隔，供阶段边界、取消和结束使用。
     */
    void Publish(const bool force = false) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_published_ < std::chrono::milliseconds(100)) return;
        PublishProgress(callback_, progress_);
        last_published_ = now;
    }

private:
    /** @brief 调用方进度回调，生命周期覆盖当前报告生成函数。 */
    const DuplicateReportGenerator::ProgressCallback& callback_;
    /** @brief 当前报告线程独占更新的进度快照。 */
    DuplicateReportProgress& progress_;
    /** @brief 最近一次实际发布时间，用于限制跨线程快照复制频率。 */
    std::chrono::steady_clock::time_point last_published_{};
};

/** @brief 失败/取消时清理半成品，成功时只清理相似报告工作键。 */
class ReportGenerationCleanup final {
public:
    ReportGenerationCleanup(RocksStore& store,
                            const DuplicateReportKind kind,
                            const std::uint64_t generation_id,
                            std::string work_prefix = {},
                            std::string candidate_prefix = {})
        : store_(store),
          generation_prefix_(GenerationPrefix(kind, generation_id)),
          work_prefix_(std::move(work_prefix)),
          candidate_prefix_(std::move(candidate_prefix)) {}

    ~ReportGenerationCleanup() {
        if (!published_) store_.DeletePrefix(RocksColumnFamily::ExactIndex, generation_prefix_, 4096, false);
        if (!work_prefix_.empty()) store_.DeletePrefix(RocksColumnFamily::ExactIndex, work_prefix_, 4096, false);
        if (!candidate_prefix_.empty()) {
            store_.DeletePrefix(RocksColumnFamily::ImageDhashIndex, candidate_prefix_, 4096, false);
            store_.DeletePrefix(RocksColumnFamily::VideoDhashIndex, candidate_prefix_, 4096, false);
        }
    }

    /** @brief 报告已经原子发布，析构时不得删除正式分组。 */
    void MarkPublished() noexcept { published_ = true; }

private:
    RocksStore& store_;
    std::string generation_prefix_;
    std::string work_prefix_;
    std::string candidate_prefix_;
    bool published_ = false;
};

/** @brief 一个已经规范化、等待真实视觉距离校验的签名代表对。 */
struct CandidatePair {
    std::string left;
    std::string right;
};

/** @brief 跳过记录编解码版本。 */
constexpr std::uint8_t kSkippedContentCodecVersion = 1;

/** @brief 编码一条跳过视觉内容记录。 */
std::string SerializeSkippedContent(const SkippedVisualContentRecord& record) {
    std::string output;
    Append(output, kSkippedContentCodecVersion);
    Append(output, static_cast<std::uint8_t>(record.reason));
    Append(output, static_cast<std::uint8_t>(record.media_kind));
    Append(output, record.active_path_count);
    AppendString(output, record.primary_sha512);
    AppendString(output, record.secondary_sha512);
    Append(output, static_cast<std::uint32_t>(record.sample_paths.size()));
    for (const std::wstring& path : record.sample_paths) AppendString(output, WideToUtf8(path));
    return output;
}

/** @brief 解码跳过视觉内容记录并拒绝未知版本。 */
std::optional<SkippedVisualContentRecord> DeserializeSkippedContent(const std::string& value,
                                                                    std::string& error) {
    try {
        Reader reader(value);
        if (reader.Read<std::uint8_t>() != kSkippedContentCodecVersion) {
            throw std::runtime_error("Unsupported skipped content record");
        }
        const std::uint8_t reason = reader.Read<std::uint8_t>();
        if (reason >= static_cast<std::uint8_t>(SkippedVisualContentReason::Count)) {
            throw std::runtime_error("Invalid skipped content reason");
        }
        const std::uint8_t mediaKind = reader.Read<std::uint8_t>();
        if (mediaKind > static_cast<std::uint8_t>(MediaKind::Audio)) {
            throw std::runtime_error("Invalid skipped content media kind");
        }
        SkippedVisualContentRecord record;
        record.reason = static_cast<SkippedVisualContentReason>(reason);
        record.media_kind = static_cast<MediaKind>(mediaKind);
        record.active_path_count = reader.Read<std::uint64_t>();
        record.primary_sha512 = reader.ReadString();
        record.secondary_sha512 = reader.ReadString();
        const std::uint32_t pathCount = reader.Read<std::uint32_t>();
        if (pathCount > 4) throw std::runtime_error("Skipped content record has too many sample paths");
        record.sample_paths.reserve(pathCount);
        for (std::uint32_t index = 0; index < pathCount; ++index) {
            record.sample_paths.push_back(Utf8ToWide(reader.ReadString()));
        }
        reader.RequireEnd();
        return record;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace

VisualDHashStatus ClassifyVisualDHash(const ShaFileData& data) noexcept {
    if (data.media_kind == MediaKind::Image) {
        return data.image_dhash.has_value() && *data.image_dhash != 0
                   ? VisualDHashStatus::Valid
                   : VisualDHashStatus::InvalidImage;
    }
    if (data.media_kind == MediaKind::Video) {
        if (!data.has_video_dhashes) return VisualDHashStatus::MissingVideo;
        const bool hasZeroFrame =
            std::any_of(data.video_dhashes.begin(), data.video_dhashes.end(), [](const std::uint64_t hash) {
                return hash == 0;
            });
        return hasZeroFrame ? VisualDHashStatus::ZeroVideoFrame : VisualDHashStatus::Valid;
    }
    return VisualDHashStatus::UnsupportedMedia;
}

bool IsSimilarReportEligibleForPermanentDeletion(
    const SimilarReportMetadata& metadata) noexcept {
    return metadata.report_schema_version == 4 && metadata.image_uses_three_stage_verification;
}

DuplicateReportStore::DuplicateReportStore(RocksStore& store) : store_(store) {}

RocksStatus DuplicateReportStore::SaveGroup(const DuplicateReportKind kind,
                                             const std::uint64_t generation_id,
                                             const std::uint64_t ordinal,
                                             const DuplicateGroup& group) {
    try {
        const std::string prefix = GenerationPrefix(kind, generation_id);
        return store_.WriteBatch(
            {{RocksColumnFamily::ExactIndex, prefix + "group/" + Hex64(ordinal), SerializeGroup(group)},
             {RocksColumnFamily::ExactIndex, prefix + "summary/" + Hex64(ordinal), SerializeSummary(group)}},
            false);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

RocksStatus DuplicateReportStore::Publish(const DuplicateReportKind kind,
                                           const std::uint64_t generation_id,
                                           const std::uint64_t group_count) {
    const std::optional<std::uint64_t> previous = ActiveGeneration(kind);
    const RocksStatus published = store_.WriteBatch(
        {{RocksColumnFamily::ExactIndex,
          GenerationPrefix(kind, generation_id) + "count",
          std::to_string(group_count)},
         {RocksColumnFamily::ExactIndex, ActiveKey(kind), Hex64(generation_id)}},
        true);
    if (published.succeeded && previous.has_value() && *previous != generation_id) {
        store_.DeletePrefix(RocksColumnFamily::ExactIndex,
                            GenerationPrefix(kind, *previous),
                            4096,
                            false);
    }
    return published;
}

RocksStatus DuplicateReportStore::DeleteAll(const DuplicateReportKind kind) {
    // 先同步撤销活动报告，避免分批清理期间继续向界面暴露正在删除的数据。
    const RocksStatus deactivated =
        store_.Delete(RocksColumnFamily::ExactIndex, ActiveKey(kind), true);
    if (!deactivated.succeeded) return deactivated;

    const std::string reportPrefix = std::string("report/") + KindName(kind) + "/";
    const RocksStatus reportsDeleted =
        store_.DeletePrefix(RocksColumnFamily::ExactIndex, reportPrefix, 4096, false);
    if (!reportsDeleted.succeeded) return reportsDeleted;
    const std::string selectionPrefix =
        std::string("report-selection/") + KindName(kind) + "/";
    return store_.DeletePrefix(RocksColumnFamily::Default, selectionPrefix, 4096, false);
}

RocksStatus DuplicateReportStore::CleanupInterruptedWork() {
    for (const DuplicateReportKind kind : {DuplicateReportKind::Exact, DuplicateReportKind::Similar}) {
        const std::optional<std::uint64_t> active = ActiveGeneration(kind);
        const std::string prefix = std::string("report/") + KindName(kind) + "/";
        std::unordered_set<std::uint64_t> generations;
        const RocksStatus listed = store_.ForEachPrefix(
            RocksColumnFamily::ExactIndex,
            prefix,
            0,
            [&](const std::string_view key, const std::string_view) {
                const std::size_t begin = prefix.size();
                const std::size_t slash = key.find('/', begin);
                if (slash == std::string_view::npos || slash - begin != 16) return true;
                const auto generation = ParseHex64(std::string(key.substr(begin, 16)));
                if (generation.has_value() && (!active.has_value() || *generation != *active)) {
                    generations.insert(*generation);
                }
                return true;
            });
        if (!listed.succeeded) return listed;
        for (const std::uint64_t generation : generations) {
            const RocksStatus deleted = store_.DeletePrefix(
                RocksColumnFamily::ExactIndex, GenerationPrefix(kind, generation), 4096, false);
            if (!deleted.succeeded) return deleted;
        }
    }
    return store_.DeletePrefix(
        RocksColumnFamily::ExactIndex, "report/similar/work/", 4096, false);
}

std::optional<std::uint64_t> DuplicateReportStore::ActiveGeneration(const DuplicateReportKind kind) const {
    std::string value;
    const RocksStatus status = store_.Get(RocksColumnFamily::ExactIndex, ActiveKey(kind), value);
    if (!status.succeeded) return std::nullopt;
    return ParseHex64(value);
}

std::optional<std::uint64_t> DuplicateReportStore::GroupCount(const DuplicateReportKind kind,
                                                               const std::uint64_t generation_id) const {
    std::string value;
    const RocksStatus status = store_.Get(RocksColumnFamily::ExactIndex,
                                          GenerationPrefix(kind, generation_id) + "count",
                                          value);
    if (!status.succeeded) return std::nullopt;
    return ParseDecimal(value);
}

RocksStatus DuplicateReportStore::LoadPage(const DuplicateReportKind kind,
                                            const std::uint64_t generation_id,
                                            const std::uint64_t offset,
                                            const std::size_t page_size,
                                            std::vector<DuplicateGroup>& groups) const {
    groups.clear();
    if (page_size == 0) return {true, {}};
    const std::string prefix = GenerationPrefix(kind, generation_id) + "group/";
    std::uint64_t visited = 0;
    std::string decodeError;
    const std::uint64_t maximum = offset > std::numeric_limits<std::uint64_t>::max() - page_size
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : offset + page_size;
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        prefix,
        static_cast<std::size_t>((std::min<std::uint64_t>)(maximum, std::numeric_limits<std::size_t>::max())),
        [&](const std::string_view, const std::string_view value) {
            if (visited++ < offset) return true;
            std::optional<DuplicateGroup> group = DeserializeGroup(std::string(value), decodeError);
            if (!group.has_value()) return false;
            groups.push_back(std::move(*group));
            return groups.size() < page_size;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}

RocksStatus DuplicateReportStore::LoadSummaries(const DuplicateReportKind kind,
                                                 const std::uint64_t generation_id,
                                                 std::vector<ReportGroupSummary>& summaries) {
    summaries.clear();
    const std::uint64_t expected = GroupCount(kind, generation_id).value_or(0);
    const std::string generationPrefix = GenerationPrefix(kind, generation_id);
    const std::string summaryPrefix = generationPrefix + "summary/";
    std::string decodeError;
    RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        summaryPrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            const std::optional<std::uint64_t> ordinal = ParseHex64(std::string(key.substr(summaryPrefix.size())));
            if (!ordinal.has_value()) {
                decodeError = "Invalid report summary ordinal";
                return false;
            }
            std::optional<ReportGroupSummary> summary =
                DeserializeSummary(*ordinal, std::string(value), decodeError);
            if (!summary.has_value()) return false;
            summaries.push_back(*summary);
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    if (summaries.size() == expected) return {true, {}};

    // 兼容旧版报告：首次完整解码后补建轻量摘要，后续不再扫描成员详情。
    summaries.clear();
    const std::string groupPrefix = generationPrefix + "group/";
    status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        groupPrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            const std::optional<std::uint64_t> ordinal = ParseHex64(std::string(key.substr(groupPrefix.size())));
            if (!ordinal.has_value()) {
                decodeError = "Invalid report group ordinal";
                return false;
            }
            std::optional<DuplicateGroup> group = DeserializeGroup(std::string(value), decodeError);
            if (!group.has_value()) return false;
            summaries.push_back({*ordinal,
                                 group->group_id,
                                 static_cast<std::uint64_t>(group->members.size()),
                                 group->reclaimable_bytes});
            const RocksStatus saved = store_.Put(RocksColumnFamily::ExactIndex,
                                                 summaryPrefix + Hex64(*ordinal),
                                                 SerializeSummary(*group),
                                                 false);
            if (!saved.succeeded) {
                decodeError = saved.message;
                return false;
            }
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}

RocksStatus DuplicateReportStore::LoadGroup(const DuplicateReportKind kind,
                                             const std::uint64_t generation_id,
                                             const std::uint64_t ordinal,
                                             DuplicateGroup& group) const {
    std::string value;
    const RocksStatus status = store_.Get(
        RocksColumnFamily::ExactIndex,
        GenerationPrefix(kind, generation_id) + "group/" + Hex64(ordinal),
        value);
    if (!status.succeeded) return status;
    std::string decodeError;
    std::optional<DuplicateGroup> decoded = DeserializeGroup(value, decodeError);
    if (!decoded.has_value()) return {false, decodeError};
    group = std::move(*decoded);
    return {true, {}};
}

RocksStatus DuplicateReportStore::SaveSimilarMetadata(
    const std::uint64_t generation_id,
    const SimilarReportMetadata& metadata) {
    try {
        return store_.Put(RocksColumnFamily::ExactIndex,
                          GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "metadata",
                          SerializeSimilarMetadata(metadata),
                          false);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

RocksStatus DuplicateReportStore::LoadSimilarMetadata(
    const std::uint64_t generation_id,
    SimilarReportMetadata& metadata) const {
    std::string value;
    const RocksStatus status = store_.Get(
        RocksColumnFamily::ExactIndex,
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "metadata",
        value);
    if (!status.succeeded) return status;
    std::string error;
    std::optional<SimilarReportMetadata> decoded = DeserializeSimilarMetadata(value, error);
    if (!decoded.has_value()) return {false, error};
    metadata = std::move(*decoded);
    return {true, {}};
}

RocksStatus DuplicateReportStore::SaveSkippedContents(
    const std::uint64_t generation_id,
    const std::vector<SkippedVisualContentRecord>& records) {
    try {
        const std::string prefix =
            GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
        std::vector<RocksMutation> batch;
        batch.reserve(4096);
        for (std::size_t index = 0; index < records.size(); ++index) {
            batch.push_back({RocksColumnFamily::ExactIndex,
                             prefix + Hex64(index),
                             SerializeSkippedContent(records[index])});
            if (batch.size() == 4096) {
                const RocksStatus written = store_.WriteBatch(batch, false);
                if (!written.succeeded) return written;
                batch.clear();
            }
        }
        if (!batch.empty()) return store_.WriteBatch(batch, false);
        return {true, {}};
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

RocksStatus DuplicateReportStore::LoadSkippedContents(
    const std::uint64_t generation_id,
    const std::uint64_t offset,
    const std::size_t maximum_items,
    std::vector<SkippedVisualContentRecord>& records) const {
    records.clear();
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
    std::uint64_t index = 0;
    std::string decodeError;
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        prefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            if (index++ < offset) return true;
            if (records.size() >= maximum_items) return false;
            std::optional<SkippedVisualContentRecord> decoded =
                DeserializeSkippedContent(std::string(value), decodeError);
            if (!decoded.has_value()) return false;
            records.push_back(std::move(*decoded));
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}

RocksStatus DuplicateReportStore::CountSkippedVisualContents(
    const std::uint64_t generation_id,
    SkippedVisualContentStats& stats) const {
    stats = {};
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) + "skipped/";
    std::string decodeError;
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        prefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            std::optional<SkippedVisualContentRecord> decoded =
                DeserializeSkippedContent(std::string(value), decodeError);
            if (!decoded.has_value()) return false;
            ++stats.total;
            ++stats.by_reason[static_cast<std::size_t>(decoded->reason)];
            return true;
        });
    if (!status.succeeded) return status;
    if (!decodeError.empty()) return {false, decodeError};
    return {true, {}};
}

RocksStatus DuplicateReportStore::SaveImageRelation(
    const std::uint64_t generation_id,
    const std::uint64_t group_id,
    const SimilarImageRelationSummary& relation) {
    try {
        const std::string prefix =
            GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
            "relation/" + Hex64(group_id) + "/";
        const std::string countKey =
            GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
            "relation-count/" + Hex64(group_id);
        std::uint64_t ordinal = 0;
        std::string encodedCount;
        const RocksStatus current = store_.Get(RocksColumnFamily::ExactIndex, countKey, encodedCount);
        if (current.succeeded) {
            const std::optional<std::uint64_t> parsed = ParseDecimal(encodedCount);
            if (!parsed.has_value()) return {false, "invalid image relation count"};
            ordinal = *parsed;
        } else if (current.message != "not_found") {
            return current;
        }
        SimilarImageRelationSummary stored = relation;
        stored.ordinal = ordinal;
        return store_.WriteBatch(
            {{RocksColumnFamily::ExactIndex,
              prefix + Hex64(ordinal),
              SerializeImageRelation(stored)},
             {RocksColumnFamily::ExactIndex, countKey, std::to_string(ordinal + 1)}},
            false);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

std::uint64_t DuplicateReportStore::ImageRelationCount(
    const std::uint64_t generation_id,
    const std::uint64_t group_id) const {
    std::string value;
    const RocksStatus status = store_.Get(
        RocksColumnFamily::ExactIndex,
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
            "relation-count/" + Hex64(group_id),
        value);
    if (!status.succeeded) return 0;
    return ParseDecimal(value).value_or(0);
}

RocksStatus DuplicateReportStore::LoadImageRelations(
    const std::uint64_t generation_id,
    const std::uint64_t group_id,
    const std::uint64_t offset,
    const std::size_t maximum_items,
    std::vector<SimilarImageRelationSummary>& relations) const {
    relations.clear();
    if (maximum_items == 0) return {true, {}};
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
        "relation/" + Hex64(group_id) + "/";
    const std::uint64_t count = ImageRelationCount(generation_id, group_id);
    const std::uint64_t maximum =
        offset > std::numeric_limits<std::uint64_t>::max() - maximum_items
            ? std::numeric_limits<std::uint64_t>::max()
            : offset + static_cast<std::uint64_t>(maximum_items);
    for (std::uint64_t ordinal = offset; ordinal < count && ordinal < maximum; ++ordinal) {
        std::string value;
        const RocksStatus status = store_.Get(
            RocksColumnFamily::ExactIndex,
            prefix + Hex64(ordinal),
            value);
        if (!status.succeeded) return status;
        std::string error;
        std::optional<SimilarImageRelationSummary> relation =
            DeserializeImageRelation(ordinal, value, error);
        if (!relation.has_value()) return {false, error};
        relations.push_back(std::move(*relation));
    }
    return {true, {}};
}

RocksStatus DuplicateReportStore::SaveImageSignatureMember(
    const std::uint64_t generation_id,
    const std::string& representative_sha512,
    const DuplicateMember& member) {
    if (!Sha512FromHex(representative_sha512).has_value()) {
        return {false, "invalid image signature representative"};
    }
    try {
        const std::string generationPrefix =
            GenerationPrefix(DuplicateReportKind::Similar, generation_id);
        const std::string countKey =
            generationPrefix + "signature-member-count/" + representative_sha512;
        std::uint64_t ordinal = 0;
        std::string encodedCount;
        const RocksStatus current = store_.Get(RocksColumnFamily::ExactIndex, countKey, encodedCount);
        if (current.succeeded) {
            const std::optional<std::uint64_t> parsed = ParseDecimal(encodedCount);
            if (!parsed.has_value()) return {false, "invalid image signature member count"};
            ordinal = *parsed;
        } else if (current.message != "not_found") {
            return current;
        }
        return store_.WriteBatch(
            {{RocksColumnFamily::ExactIndex,
              generationPrefix + "signature-member/" + representative_sha512 + "/" + Hex64(ordinal),
              SerializeStandaloneMember(member)},
             {RocksColumnFamily::ExactIndex, countKey, std::to_string(ordinal + 1)}},
            false);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

std::uint64_t DuplicateReportStore::ImageSignatureMemberCount(
    const std::uint64_t generation_id,
    const std::string& representative_sha512) const {
    std::string value;
    const RocksStatus status = store_.Get(
        RocksColumnFamily::ExactIndex,
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
            "signature-member-count/" + representative_sha512,
        value);
    if (!status.succeeded) return 0;
    return ParseDecimal(value).value_or(0);
}

RocksStatus DuplicateReportStore::LoadImageSignatureMembers(
    const std::uint64_t generation_id,
    const std::string& representative_sha512,
    const std::uint64_t offset,
    const std::size_t maximum_items,
    std::vector<DuplicateMember>& members) const {
    members.clear();
    if (!Sha512FromHex(representative_sha512).has_value()) {
        return {false, "invalid image signature representative"};
    }
    if (maximum_items == 0) return {true, {}};
    const std::string prefix =
        GenerationPrefix(DuplicateReportKind::Similar, generation_id) +
        "signature-member/" + representative_sha512 + "/";
    const std::uint64_t count =
        ImageSignatureMemberCount(generation_id, representative_sha512);
    const std::uint64_t maximum =
        offset > std::numeric_limits<std::uint64_t>::max() - maximum_items
            ? std::numeric_limits<std::uint64_t>::max()
            : offset + static_cast<std::uint64_t>(maximum_items);
    for (std::uint64_t ordinal = offset; ordinal < count && ordinal < maximum; ++ordinal) {
        std::string value;
        const RocksStatus status = store_.Get(
            RocksColumnFamily::ExactIndex,
            prefix + Hex64(ordinal),
            value);
        if (!status.succeeded) return status;
        std::string error;
        std::optional<DuplicateMember> member = DeserializeStandaloneMember(value, error);
        if (!member.has_value()) return {false, error};
        members.push_back(std::move(*member));
    }
    return {true, {}};
}

DuplicateReportGenerator::DuplicateReportGenerator(
    RocksStore& store,
    std::string algorithm_version,
    DHashSimilarityConfig dhash_similarity,
    ImageSimilarityConfig image_similarity,
    ComputeConfig compute,
    IoConfig io,
    StorageConfig storage)
    : store_(store),
      algorithm_version_(std::move(algorithm_version)),
      dhash_similarity_(dhash_similarity),
      image_similarity_(image_similarity),
      compute_(compute),
      io_(io),
      storage_(storage) {}

DuplicateReportResult DuplicateReportGenerator::GenerateExact(
    MySqlClient& client,
    const std::atomic_bool& cancel_requested,
    const ProgressCallback& progress_callback) {
    DuplicateReportResult result;
    result.generation_id = NextGenerationId();
    DuplicateReportProgress progress;
    progress.generation_id = result.generation_id;
    ThrottledProgressPublisher publisher(progress_callback, progress);
    publisher.BeginStage("streaming_mysql_exact_groups", 1, 2, false, 0);
    DuplicateReportStore reportStore(store_);
    ReportGenerationCleanup cleanup(store_, DuplicateReportKind::Exact, result.generation_id);
    ExactDuplicateReader reader(client);
    std::string saveError;
    const MySqlStatus status = reader.Stream([&](DuplicateGroup&& group) {
        if (cancel_requested.load(std::memory_order_relaxed)) return false;
        const RocksStatus saved = reportStore.SaveGroup(DuplicateReportKind::Exact,
                                                        result.generation_id,
                                                        progress.emitted_groups,
                                                        group);
        if (!saved.succeeded) {
            saveError = saved.message;
            return false;
        }
        ++progress.emitted_groups;
        progress.processed_paths += group.members.size();
        progress.stage_processed = progress.emitted_groups;
        publisher.Publish();
        return true;
    });
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!saveError.empty()) {
        result.message = saveError;
        return result;
    }
    if (!status.succeeded) {
        result.message = status.message;
        return result;
    }
    publisher.BeginStage("publishing_exact_report", 2, 2, true, 1);
    const RocksStatus published = reportStore.Publish(DuplicateReportKind::Exact,
                                                      result.generation_id,
                                                      progress.emitted_groups);
    if (!published.succeeded) {
        result.message = published.message;
        return result;
    }
    cleanup.MarkPublished();
    result.succeeded = true;
    result.group_count = progress.emitted_groups;
    result.message = "ok";
    progress.stage = "completed";
    progress.stage_processed = 1;
    publisher.Publish(true);
    return result;
}

DuplicateReportResult DuplicateReportGenerator::GenerateSimilar(
    MySqlClient& client,
    const std::atomic_bool& cancel_requested,
    const ProgressCallback& progress_callback,
    const DiagnosticCallback& diagnostic_callback) {
    DuplicateReportResult result;
    result.generation_id = NextGenerationId();
    DuplicateReportProgress progress;
    progress.generation_id = result.generation_id;
    ThrottledProgressPublisher publisher(progress_callback, progress);
    const std::string workPrefix = WorkPrefix(result.generation_id);
    const std::string candidateNamespace = "sim_" + Hex64(result.generation_id);
    const std::string candidatePrefix = candidateNamespace + std::string(1, '\0');
    DHashCandidateIndex candidateIndex(
        store_,
        algorithm_version_,
        candidateNamespace,
        dhash_similarity_.image_max_hamming_distance,
        dhash_similarity_.video_max_average_hamming_distance);
    DuplicateReportStore reportStore(store_);
    ReportGenerationCleanup cleanup(store_,
                                    DuplicateReportKind::Similar,
                                    result.generation_id,
                                    workPrefix,
                                    candidatePrefix);
    std::string failure;
    std::uint64_t representativeCount = 0;
    std::uint64_t groupRootCount = 0;
    std::uint64_t identicalSignatureEdgeCount = 0;
    std::uint64_t crossRelationCount = 0;
    std::uint64_t deferredHotSignatures = 0;
    std::uint64_t candidatePeakBytes = 0;
    /**
     * @brief 读取报告工作区十进制计数。
     * @param key 完整 RocksDB 键。
     * @param value 输出计数。
     * @param missing_as_zero 键不存在时是否按零处理。
     * @return 成功时返回 true；失败信息写入 failure。
     */
    const auto readCounter = [&](const std::string& key,
                                 std::uint64_t& value,
                                 const bool missing_as_zero) {
        std::string encoded;
        const RocksStatus status = store_.Get(RocksColumnFamily::ExactIndex, key, encoded);
        if (!status.succeeded) {
            if (missing_as_zero && status.message == "not_found") {
                value = 0;
                return true;
            }
            failure = status.message;
            return false;
        }
        const std::optional<std::uint64_t> parsed = ParseDecimal(encoded);
        if (!parsed.has_value()) {
            failure = "invalid similarity work counter";
            return false;
        }
        value = *parsed;
        return true;
    };

    /**
     * @brief 加载报告工作区中的 SHA-512 内容。
     * @param digest_hex 128 字符 SHA-512 十六进制。
     * @param data 输出内容。
     * @return 成功返回 true，失败信息写入 failure。
     */
    const auto loadContentByHex = [&](const std::string& digest_hex, ShaFileData& data) {
        const std::optional<Sha512Digest> digest = Sha512FromHex(digest_hex);
        std::string loadError;
        std::optional<ShaFileData> loaded =
            digest.has_value() ? LoadWorkContent(store_, workPrefix, *digest, loadError) : std::nullopt;
        if (!loaded.has_value()) {
            failure = loadError.empty() ? "invalid visual content digest" : loadError;
            return false;
        }
        data = std::move(*loaded);
        return true;
    };

    /**
     * @brief 加载一个 SHA 的全部结构面路径候选。
     * @param digest_hex 内容 SHA-512 十六进制。
     * @return 按扫描优先级和路径 ID 稳定排序的候选。
     * @throws std::runtime_error RocksDB 读取或记录格式错误。
     */
    const auto loadStructurePaths = [&](const std::string& digest_hex) {
        std::vector<StructuralPathCandidate> candidates;
        const std::string prefix = workPrefix + "structure-path/" + digest_hex + "/";
        const RocksStatus loaded = store_.ForEachPrefix(
            RocksColumnFamily::ExactIndex,
            prefix,
            0,
            [&](const std::string_view, const std::string_view value) {
                const std::size_t separator = value.find('\0');
                if (separator == std::string_view::npos || separator + 1 >= value.size()) {
                    throw std::runtime_error("invalid structural path candidate");
                }
                candidates.push_back({std::string(value.substr(separator + 1)),
                                      std::string(value.substr(0, separator))});
                return true;
            });
        if (!loaded.succeeded) throw std::runtime_error(loaded.message);
        return candidates;
    };

    /**
     * @brief 把内容摘要解析到完整视觉签名代表。
     * @param digest_hex 内容 SHA-512 十六进制。
     * @param representative 输出视觉签名代表。
     * @return 成功返回 true；未参与报告或存储失败返回 false。
     */
    const auto resolveVisualRepresentative = [&](const std::string& digest_hex,
                                                 std::string& representative) {
        const RocksStatus status = store_.Get(
            RocksColumnFamily::ExactIndex,
            workPrefix + "visual-representative/" + digest_hex,
            representative);
        if (!status.succeeded) {
            if (status.message != "not_found") failure = status.message;
            return false;
        }
        return true;
    };

    /**
     * @brief 把任意内容摘要解析到严格分组根。
     * @param digest_hex 内容 SHA-512 十六进制。
     * @param root 输出严格分组根摘要。
     * @return 成功解析时返回 true；未参加有效视觉报告或存储失败时返回 false。
     */
    const auto resolveGroupRoot = [&](const std::string& digest_hex, std::string& root) {
        std::string representative;
        if (!resolveVisualRepresentative(digest_hex, representative)) return false;
        const RocksStatus groupStatus = store_.Get(
            RocksColumnFamily::ExactIndex,
            workPrefix + "group-of/" + representative,
            root);
        if (!groupStatus.succeeded) {
            if (groupStatus.message != "not_found") failure = groupStatus.message;
            return false;
        }
        return true;
    };

    // 生成期收集的跳过视觉内容记录；随元数据在发布前批量写入。
    // 诊断回调已实时流式记录每条跳过，收集上限只影响持久化明细，不丢失统计；超出后跳过明细截断。
    constexpr std::size_t kMaxSkippedContentRecords = 100000;
    std::mutex skippedContentsMutex;
    std::vector<SkippedVisualContentRecord> skippedContents;

    publisher.BeginStage("streaming_mysql_visual_contents", 1, 9, false, 0);
    MySqlReadRepository repository(client);
    ImageFeatureCompletenessSnapshot imageCompleteness;
    const MySqlStatus completenessStatus = repository.CountImageFeatureCompleteness(
        algorithm_version_, imageCompleteness);
    if (!completenessStatus.succeeded) {
        result.message = completenessStatus.message;
        return result;
    }
    const MySqlStatus visualContents = repository.StreamVisualContents(
        algorithm_version_,
        [&](ShaFileData&& data) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            const VisualDHashStatus visualStatus = data.media_kind == MediaKind::Image
                                                       ? (data.media_algorithm_version == algorithm_version_ &&
                                                                  ImageSimilarityRules::HasCompleteFeatures(data)
                                                              ? VisualDHashStatus::Valid
                                                              : VisualDHashStatus::InvalidImage)
                                                       : ClassifyVisualDHash(data);
            if (visualStatus != VisualDHashStatus::Valid) {
                ++result.skipped_invalid_visuals;
                if (visualStatus == VisualDHashStatus::InvalidImage) {
                    ++result.skipped_invalid_images;
                } else if (visualStatus == VisualDHashStatus::MissingVideo ||
                           visualStatus == VisualDHashStatus::ZeroVideoFrame) {
                    ++result.skipped_invalid_videos;
                }
                {
                    std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                    SkippedVisualContentRecord skipped;
                    skipped.primary_sha512 = Sha512ToHex(data.sha512);
                    skipped.media_kind = data.media_kind;
                    skipped.reason =
                        visualStatus == VisualDHashStatus::InvalidImage
                            ? SkippedVisualContentReason::InvalidImage
                            : visualStatus == VisualDHashStatus::MissingVideo
                                  ? SkippedVisualContentReason::MissingVideoDHash
                                  : visualStatus == VisualDHashStatus::ZeroVideoFrame
                                        ? SkippedVisualContentReason::ZeroVideoFrame
                                        : SkippedVisualContentReason::UnsupportedMedia;
                    if (skippedContents.size() < kMaxSkippedContentRecords) {
                        skippedContents.push_back(std::move(skipped));
                    }
                }
                DuplicateReportDiagnostic diagnostic;
                diagnostic.severity = DuplicateReportDiagnosticSeverity::Warning;
                diagnostic.stage = "streaming_mysql_visual_contents";
                diagnostic.subject = Sha512ToHex(data.sha512);
                if (visualStatus == VisualDHashStatus::InvalidImage) {
                    diagnostic.operation = "skip_invalid_image";
                    diagnostic.message = "图片三级特征未完成，已跳过该视觉内容";
                } else if (visualStatus == VisualDHashStatus::MissingVideo) {
                    diagnostic.operation = "skip_missing_video_dhash";
                    diagnostic.message = "视频六帧 dHash 缺失，已跳过该视觉内容";
                } else if (visualStatus == VisualDHashStatus::ZeroVideoFrame) {
                    diagnostic.operation = "skip_zero_video_dhash";
                    diagnostic.message = "视频六帧 dHash 包含零值，已跳过该视觉内容";
                } else {
                    diagnostic.operation = "skip_unsupported_media";
                    diagnostic.message = "媒体类型不参与视觉相似报告，已跳过";
                }
                PublishDiagnostic(diagnostic_callback, diagnostic);
                ++progress.stage_processed;
                publisher.Publish();
                return true;
            }
            const RocksStatus saved = store_.Put(
                RocksColumnFamily::ExactIndex,
                workPrefix + "content/" + Sha512ToHex(data.sha512),
                CoreModelCodec::SerializeShaFileData(data),
                false);
            if (!saved.succeeded) {
                failure = saved.message;
                return false;
            }
            const RocksStatus indexed = data.media_kind == MediaKind::Image
                                            ? RocksStatus{true, {}}
                                            : candidateIndex.AddVideo(data);
            if (!indexed.succeeded) {
                failure = indexed.message;
                return false;
            }
            ++progress.processed_contents;
            ++progress.stage_processed;
            publisher.Publish();
            return true;
        });
    if (!visualContents.succeeded && failure.empty()) failure = visualContents.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    publisher.BeginStage("preparing_visual_representatives", 2, 9, true, progress.processed_contents);
    const RocksStatus signatures = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        workPrefix + "content/",
        0,
        [&](const std::string_view, const std::string_view value) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            const auto finishCurrentContent = [&]() {
                ++progress.stage_processed;
                publisher.Publish();
                return true;
            };
            std::string decodeError;
            std::optional<ShaFileData> current =
                CoreModelCodec::DeserializeShaFileData(std::string(value), decodeError);
            if (!current.has_value()) {
                failure = decodeError;
                return false;
            }
            if (!IsUsableVisual(*current)) return finishCurrentContent();
            const std::string currentHex = Sha512ToHex(current->sha512);
            const std::string signatureKey = workPrefix + "signature/" + VisualSignatureKey(*current);
            std::string representative;
            const RocksStatus existing = store_.Get(RocksColumnFamily::ExactIndex, signatureKey, representative);
            if (!existing.succeeded && existing.message != "not_found") {
                failure = existing.message;
                return false;
            }
            if (!existing.succeeded) {
                const RocksStatus saved = store_.WriteBatch(
                    {{RocksColumnFamily::ExactIndex, signatureKey, currentHex},
                     {RocksColumnFamily::ExactIndex,
                      workPrefix + "visual-representative/" + currentHex,
                      currentHex},
                     {RocksColumnFamily::ExactIndex,
                      workPrefix + "representative/" + currentHex,
                      std::string{}},
                     {RocksColumnFamily::ExactIndex,
                      workPrefix + "signature-count/" + currentHex,
                      "1"}},
                    false);
                if (!saved.succeeded) {
                    failure = saved.message;
                    return false;
                }
                ++representativeCount;
            } else if (representative != currentHex) {
                std::uint64_t signatureCount = 0;
                if (!readCounter(workPrefix + "signature-count/" + representative,
                                 signatureCount,
                                 false)) {
                    return false;
                }
                std::string loadError;
                const std::optional<Sha512Digest> representativeDigest = Sha512FromHex(representative);
                std::optional<ShaFileData> representativeData = representativeDigest.has_value()
                                                                       ? LoadWorkContent(store_, workPrefix, *representativeDigest, loadError)
                                                                       : std::nullopt;
                if (!representativeData.has_value()) {
                    failure = loadError.empty() ? "invalid visual signature representative" : loadError;
                    return false;
                }
                DuplicateGroupKind groupKind{};
                std::optional<SimilarityEvidence> evidence = CompareVisuals(
                    *representativeData,
                    *current,
                    groupKind,
                    dhash_similarity_);
                if (!evidence.has_value()) {
                    failure = "identical visual signature did not pass final comparison";
                    return false;
                }
                const std::string leftHex = (std::min)(representative, currentHex);
                const std::string rightHex = (std::max)(representative, currentHex);
                const RocksStatus savedEdge = store_.WriteBatch(
                    {{RocksColumnFamily::ExactIndex,
                      workPrefix + "visual-representative/" + currentHex,
                      representative},
                      {RocksColumnFamily::ExactIndex,
                       workPrefix + "signature-count/" + representative,
                       std::to_string(signatureCount + 1)},
                      {RocksColumnFamily::ExactIndex,
                       workPrefix + "identical-edge/" + leftHex + rightHex,
                       SerializeEdge(groupKind, *evidence)}},
                    false);
                if (!savedEdge.succeeded) {
                    failure = savedEdge.message;
                    return false;
                }
                ++identicalSignatureEdgeCount;
            }
            return finishCurrentContent();
        });
    if (!signatures.succeeded && failure.empty()) failure = signatures.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    publisher.BeginStage("enumerating_primary_candidates", 3, 9, true, representativeCount);
    const std::string representativePrefix = workPrefix + "representative/";
    const std::string candidatePairPrefix = workPrefix + "candidate-pair/";
    std::vector<PdqCandidateRecord> imageRepresentatives;
    imageRepresentatives.reserve(static_cast<std::size_t>((std::min<std::uint64_t>)(
        representativeCount, std::numeric_limits<std::uint32_t>::max())));
    const RocksStatus enumerated = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        representativePrefix,
        0,
        [&](const std::string_view key, const std::string_view) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            if (key.size() != representativePrefix.size() + 128) {
                failure = "invalid visual representative key";
                return false;
            }
            const std::string currentHex(key.substr(representativePrefix.size()));
            const std::optional<Sha512Digest> currentDigest = Sha512FromHex(currentHex);
            std::string loadError;
            std::optional<ShaFileData> current = currentDigest.has_value()
                                                         ? LoadWorkContent(store_, workPrefix, *currentDigest, loadError)
                                                         : std::nullopt;
            if (!current.has_value()) {
                failure = loadError.empty() ? "invalid visual representative digest" : loadError;
                return false;
            }
            if (current->media_kind == MediaKind::Image) {
                if (imageRepresentatives.size() >= std::numeric_limits<std::uint32_t>::max()) {
                    failure = "too many image representatives for PDQ index";
                    return false;
                }
                imageRepresentatives.push_back(
                    {current->sha512,
                     *current->image_pdq_hash,
                     current->image_dhash.value_or(0),
                     current->image_dhash.has_value(),
                     current->width,
                     current->height,
                     current->image_pdq_quality.value_or(0)});
                ++progress.stage_processed;
                publisher.Publish();
                return true;
            }
            const auto candidateVisitor = [&](const Sha512Digest& candidateDigest) {
                if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
                std::optional<ShaFileData> candidate =
                    LoadWorkContent(store_, workPrefix, candidateDigest, loadError);
                if (!candidate.has_value()) {
                    failure = loadError;
                    return false;
                }
                if (!IsUsableVisual(*candidate)) return true;
                std::string candidateRepresentative;
                const RocksStatus representativeStatus = store_.Get(
                    RocksColumnFamily::ExactIndex,
                    workPrefix + "signature/" + VisualSignatureKey(*candidate),
                    candidateRepresentative);
                if (!representativeStatus.succeeded) {
                    failure = representativeStatus.message;
                    return false;
                }
                // 每个无序签名对只由字典序较大的代表写入一次候选任务。
                if (candidateRepresentative >= currentHex) return true;
                const RocksStatus saved = store_.Put(
                    RocksColumnFamily::ExactIndex,
                    candidatePairPrefix + candidateRepresentative + currentHex,
                    std::string{},
                    false);
                if (!saved.succeeded) {
                    failure = saved.message;
                    return false;
                }
                ++progress.candidate_pairs_total;
                publisher.Publish();
                return true;
            };
            const RocksStatus candidates = candidateIndex.FindVideoCandidates(*current, candidateVisitor);
            if (!candidates.succeeded) {
                failure = candidates.message;
                return false;
            }
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            ++progress.stage_processed;
            publisher.Publish();
            return true;
        });
    if (!enumerated.succeeded && failure.empty()) failure = enumerated.message;
    if (failure.empty() && !cancel_requested.load(std::memory_order_relaxed)) {
        std::vector<RocksMutation> candidateBatch;
        candidateBatch.reserve(image_similarity_.candidate_write_batch_size);
        std::uint64_t candidateTempBytes = 0;
        const auto flushCandidates = [&]() {
            if (candidateBatch.empty()) return true;
            const RocksStatus saved = store_.WriteBatch(candidateBatch, false);
            if (!saved.succeeded) failure = saved.message;
            candidateBatch.clear();
            return saved.succeeded;
        };
        PdqCandidateBuilder builder(image_similarity_);
        const PdqCandidateBuildResult built = builder.Build(
            imageRepresentatives,
            cancel_requested,
            [&](const Sha512Digest& left, const Sha512Digest& right) {
                const std::string pairKey = NormalizedPairKey(Sha512ToHex(left), Sha512ToHex(right));
                candidateTempBytes += candidatePairPrefix.size() + pairKey.size();
                if (candidateTempBytes >
                    static_cast<std::uint64_t>(image_similarity_.candidate_temp_mib) * 1024ULL * 1024ULL) {
                    failure = "PDQ candidate temporary-space budget exceeded";
                    return false;
                }
                candidateBatch.push_back(
                    {RocksColumnFamily::ExactIndex, candidatePairPrefix + pairKey, std::string{}});
                return candidateBatch.size() < image_similarity_.candidate_write_batch_size ||
                       flushCandidates();
            });
        deferredHotSignatures = built.deferred_hot_signatures;
        candidatePeakBytes = built.peak_memory_bytes;
        if (built.cancelled) {
            result.cancelled = true;
        } else if (!built.succeeded && failure.empty()) {
            failure = built.message;
        }
        if (failure.empty() && !result.cancelled) flushCandidates();
    }
    progress.candidate_pairs_total = 0;
    if (failure.empty() && !cancel_requested.load(std::memory_order_relaxed)) {
        const RocksStatus countedCandidates = store_.ForEachPrefix(
            RocksColumnFamily::ExactIndex,
            candidatePairPrefix,
            0,
            [&](const std::string_view, const std::string_view) {
                ++progress.candidate_pairs_total;
                return true;
            });
        if (!countedCandidates.succeeded) failure = countedCandidates.message;
    }
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    const std::string structuralPairPrefix = workPrefix + "structural-pair/";
    std::uint64_t secondaryPairCount = 0;
    std::uint64_t videoAcceptedPairCount = 0;
    std::uint64_t lowQualityPairsEvaluatedCount = 0;
    std::uint64_t lowQualityPairsSecondaryPassedCount = 0;
    std::uint64_t lowQualityPairsStructureAcceptedCount = 0;
    std::uint64_t structuralIoFailureCount = 0;
    std::uint64_t structuralTimeoutCount = 0;
    std::uint64_t structuralComputeFailureCount = 0;
    publisher.BeginStage("validating_secondary_phash",
                         4,
                         9,
                         true,
                         progress.candidate_pairs_total);
    progress.configured_validation_threads = image_similarity_.report_validation_worker_threads;
    if (progress.candidate_pairs_total != 0) {
        const std::size_t queueCapacity = (std::max<std::size_t>)(
            64,
            static_cast<std::size_t>(image_similarity_.report_validation_worker_threads) * 8);
        std::atomic<std::uint64_t> validatedPairs{0};
        std::atomic<std::uint64_t> secondaryPairs{0};
        std::atomic<std::uint64_t> acceptedVideos{0};
        std::atomic<std::uint64_t> rejectedPairs{0};
        std::atomic<std::uint64_t> lowQualityPairsEvaluated{0};
        std::atomic<std::uint64_t> lowQualityPairsSecondaryPassed{0};
        TaskThreadPool validationPool("visual-secondary-report",
                                      image_similarity_.report_validation_worker_threads,
                                      queueCapacity);

        RocksStatus produced{true, {}};
        produced = store_.ForEachPrefix(
            RocksColumnFamily::ExactIndex,
            candidatePairPrefix,
            0,
            [&](const std::string_view key, const std::string_view) {
                if (cancel_requested.load(std::memory_order_relaxed) ||
                    !validationPool.failure_message().empty()) {
                    return false;
                }
                if (key.size() != candidatePairPrefix.size() + 256) {
                    failure = "invalid dHash candidate pair key";
                    return false;
                }
                CandidatePair pair{
                    std::string(key.substr(candidatePairPrefix.size(), 128)),
                    std::string(key.substr(candidatePairPrefix.size() + 128, 128))};
                const bool submitted = validationPool.Submit([&, pair = std::move(pair)] {
                    if (cancel_requested.load(std::memory_order_relaxed)) return;
                    std::string loadError;
                    const std::optional<Sha512Digest> leftDigest = Sha512FromHex(pair.left);
                    const std::optional<Sha512Digest> rightDigest = Sha512FromHex(pair.right);
                    std::optional<ShaFileData> left =
                        leftDigest.has_value()
                            ? LoadWorkContent(store_, workPrefix, *leftDigest, loadError)
                            : std::nullopt;
                    if (!left.has_value()) {
                        throw std::runtime_error(loadError.empty()
                                                     ? "invalid left dHash candidate digest"
                                                     : loadError);
                    }
                    std::optional<ShaFileData> right =
                        rightDigest.has_value()
                            ? LoadWorkContent(store_, workPrefix, *rightDigest, loadError)
                            : std::nullopt;
                    if (!right.has_value()) {
                        throw std::runtime_error(loadError.empty()
                                                     ? "invalid right dHash candidate digest"
                                                     : loadError);
                    }
                    if (left->media_kind == MediaKind::Image &&
                        right->media_kind == MediaKind::Image &&
                        ImageSimilarityRules::HasCompleteFeatures(*left) &&
                        ImageSimilarityRules::HasCompleteFeatures(*right)) {
                        ImageQualityClass qualityClass = ImageQualityClass::Standard;
                        const ImageSimilarityThresholdProfile& profile =
                            ImageSimilarityRules::SelectThresholdProfile(
                                *left, *right, image_similarity_, qualityClass);
                        if (qualityClass == ImageQualityClass::LowQuality) {
                            lowQualityPairsEvaluated.fetch_add(1, std::memory_order_relaxed);
                        }
                        const std::uint32_t pdqDistance =
                            ImageSimilarityRules::PdqHammingDistance(
                                *left->image_pdq_hash,
                                *right->image_pdq_hash,
                                image_similarity_.force_scalar_kernels);
                        const bool usedDHashFallback =
                            pdqDistance > profile.pdq_max_hamming_distance &&
                            left->image_dhash.has_value() && right->image_dhash.has_value() &&
                            DHashRules::HammingDistance(*left->image_dhash, *right->image_dhash) <=
                                profile.fallback_dhash_max_hamming_distance;
                        const bool primaryAccepted =
                            ImageSimilarityRules::AspectRatiosCompatible(
                                *left, *right, image_similarity_.aspect_ratio_tolerance_percent) &&
                            (pdqDistance <= profile.pdq_max_hamming_distance ||
                             usedDHashFallback);
                        if (!primaryAccepted) {
                            rejectedPairs.fetch_add(1, std::memory_order_relaxed);
                            validatedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        const ZonedPHashEvidence zoned =
                            ImageSimilarityRules::CompareZonedPHashes(
                                *left,
                                *right,
                                profile,
                                image_similarity_.force_scalar_kernels);
                        if (!zoned.accepted) {
                            rejectedPairs.fetch_add(1, std::memory_order_relaxed);
                            validatedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        const SimilarityEvidence evidence = ImageEvidence(
                            *left,
                            *right,
                            zoned,
                            image_similarity_.force_scalar_kernels,
                            qualityClass,
                            usedDHashFallback);
                        const RocksStatus saved = store_.Put(
                            RocksColumnFamily::ExactIndex,
                            structuralPairPrefix + pair.left + pair.right,
                            SerializeEdge(DuplicateGroupKind::SimilarImage, evidence),
                            false);
                        if (!saved.succeeded) throw std::runtime_error(saved.message);
                        secondaryPairs.fetch_add(1, std::memory_order_relaxed);
                        if (qualityClass == ImageQualityClass::LowQuality) {
                            lowQualityPairsSecondaryPassed.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        DuplicateGroupKind kind{};
                        std::optional<SimilarityEvidence> evidence =
                            CompareVisuals(*left, *right, kind, dhash_similarity_);
                        if (!evidence.has_value()) {
                            rejectedPairs.fetch_add(1, std::memory_order_relaxed);
                            validatedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        const std::string encoded = SerializeEdge(kind, *evidence);
                        const RocksStatus saved = store_.WriteBatch(
                            {{RocksColumnFamily::ExactIndex,
                              workPrefix + "edge/" + pair.left + pair.right,
                              encoded},
                             {RocksColumnFamily::ExactIndex,
                              workPrefix + "adjacency/" + pair.left + "/" + pair.right,
                              encoded},
                             {RocksColumnFamily::ExactIndex,
                              workPrefix + "adjacency/" + pair.right + "/" + pair.left,
                              encoded}},
                            false);
                        if (!saved.succeeded) throw std::runtime_error(saved.message);
                        acceptedVideos.fetch_add(1, std::memory_order_relaxed);
                    }
                    validatedPairs.fetch_add(1, std::memory_order_relaxed);
                });
                const TaskThreadPoolSnapshot pool = validationPool.snapshot();
                progress.validated_candidate_pairs = validatedPairs.load(std::memory_order_relaxed);
                progress.accepted_similarity_pairs = acceptedVideos.load(std::memory_order_relaxed);
                progress.rejected_bucket_collisions = rejectedPairs.load(std::memory_order_relaxed);
                progress.active_validation_threads = pool.active_threads;
                progress.stage_processed = progress.validated_candidate_pairs;
                publisher.Publish();
                return submitted;
            });
        validationPool.CloseSubmissions();

        while (true) {
            const TaskThreadPoolSnapshot pool = validationPool.snapshot();
            progress.validated_candidate_pairs =
                validatedPairs.load(std::memory_order_relaxed);
            progress.accepted_similarity_pairs = acceptedVideos.load(std::memory_order_relaxed);
            progress.rejected_bucket_collisions =
                rejectedPairs.load(std::memory_order_relaxed);
            progress.active_validation_threads = pool.active_threads;
            progress.stage_processed = progress.validated_candidate_pairs;
            publisher.Publish();
            if (pool.queued_tasks == 0 && pool.active_threads == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        validationPool.Join();

        progress.validated_candidate_pairs =
            validatedPairs.load(std::memory_order_relaxed);
        secondaryPairCount = secondaryPairs.load(std::memory_order_relaxed);
        videoAcceptedPairCount = acceptedVideos.load(std::memory_order_relaxed);
        lowQualityPairsEvaluatedCount =
            lowQualityPairsEvaluated.load(std::memory_order_relaxed);
        lowQualityPairsSecondaryPassedCount =
            lowQualityPairsSecondaryPassed.load(std::memory_order_relaxed);
        progress.accepted_similarity_pairs = videoAcceptedPairCount;
        progress.rejected_bucket_collisions =
            rejectedPairs.load(std::memory_order_relaxed);
        progress.active_validation_threads = 0;
        progress.stage_processed = progress.validated_candidate_pairs;
        progress.matched_pairs =
            identicalSignatureEdgeCount + progress.accepted_similarity_pairs;
        publisher.Publish(true);

        if (!produced.succeeded && failure.empty()) {
            failure = produced.message;
        }
        if (!validationPool.failure_message().empty()) {
            failure = validationPool.failure_message();
        }
        if (!cancel_requested.load(std::memory_order_relaxed) &&
            failure.empty() &&
            progress.validated_candidate_pairs != progress.candidate_pairs_total) {
            failure = "secondary validation candidate count mismatch";
        }
    } else {
        progress.matched_pairs = identicalSignatureEdgeCount;
        progress.stage_processed = 0;
        publisher.Publish(true);
    }
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        publisher.Publish(true);
        return result;
    }
    if (!failure.empty()) {
        result.message = failure;
        publisher.Publish(true);
        return result;
    }

    publisher.BeginStage("loading_structure_paths", 5, 9, false, 0);
    const MySqlStatus structurePaths = repository.StreamActivePaths(
        [&](FilePathRecord&& path) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            ++progress.stage_processed;
            publisher.Publish();
            if (!IsActivePath(path)) return true;
            const std::string digestHex = Sha512ToHex(*path.sha512);
            std::string ignored;
            const RocksStatus content = store_.Get(RocksColumnFamily::ExactIndex,
                                                   workPrefix + "content/" + digestHex,
                                                   ignored);
            if (!content.succeeded) {
                if (content.message == "not_found") return true;
                failure = content.message;
                return false;
            }
            const std::string pathKey = workPrefix + "structure-path/" + digestHex + "/" +
                                        Hex64(path.scan_root_priority) + Hex64(path.path_id);
            const std::string encodedPath = WideToUtf8(path.storage_target_key) + '\0' +
                                            WideToUtf8(path.path.wstring());
            const RocksStatus saved = store_.Put(RocksColumnFamily::ExactIndex,
                                                  pathKey,
                                                  encodedPath,
                                                  false);
            if (!saved.succeeded) failure = saved.message;
            return saved.succeeded;
        });
    if (!structurePaths.succeeded && failure.empty()) failure = structurePaths.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        publisher.Publish(true);
        return result;
    }
    if (!failure.empty()) {
        result.message = failure;
        publisher.Publish(true);
        return result;
    }

    StructuralVerificationCache structureCache(
        static_cast<std::size_t>(image_similarity_.structural_cache_mib) * 1024U * 1024U,
        compute_.ffmpeg_threads_per_task,
        io_.no_progress_timeout_seconds * 1000U,
        cancel_requested);
    StructuralVerificationScheduler structuralScheduler(
        image_similarity_.structural_worker_threads,
        compute_,
        storage_,
        cancel_requested);
    publisher.BeginStage("validating_image_structure", 6, 9, true, secondaryPairCount);
    progress.configured_validation_threads = structuralScheduler.effective_worker_count();
    if (secondaryPairCount != 0) {
        const std::uint32_t structuralWorkerCount = structuralScheduler.effective_worker_count();
        const std::size_t queueCapacity = (std::max<std::size_t>)(
            16,
            static_cast<std::size_t>(structuralWorkerCount) * 4);
        std::atomic<std::uint64_t> verifiedPairs{0};
        std::atomic<std::uint64_t> acceptedImages{0};
        std::atomic<std::uint64_t> rejectedImages{0};
        std::atomic<std::uint64_t> structuralIoFailures{0};
        std::atomic<std::uint64_t> structuralTimeouts{0};
        std::atomic<std::uint64_t> structuralMissingPaths{0};
        std::atomic<std::uint64_t> structuralComputeFailures{0};
        std::atomic<std::uint64_t> lowQualityStructureAccepted{0};
        TaskThreadPool structuralPool("image-structural-report",
                                      structuralWorkerCount,
                                      queueCapacity);
        const RocksStatus produced = store_.ForEachPrefix(
            RocksColumnFamily::ExactIndex,
            structuralPairPrefix,
            0,
            [&](const std::string_view key, const std::string_view value) {
                if (cancel_requested.load(std::memory_order_relaxed) ||
                    !structuralPool.failure_message().empty()) return false;
                if (key.size() != structuralPairPrefix.size() + 256) {
                    failure = "invalid structural candidate pair key";
                    return false;
                }
                CandidatePair pair{
                    std::string(key.substr(structuralPairPrefix.size(), 128)),
                    std::string(key.substr(structuralPairPrefix.size() + 128, 128))};
                const std::string encodedEvidence(value);
                const bool submitted = structuralPool.Submit(
                    [&, pair = std::move(pair), encodedEvidence] {
                        if (cancel_requested.load(std::memory_order_relaxed)) return;
                        DuplicateGroupKind kind{};
                        SimilarityEvidence evidence;
                        std::string decodeError;
                        if (!DeserializeEdge(encodedEvidence, kind, evidence, decodeError)) {
                            throw std::runtime_error(decodeError);
                        }
                        const std::vector<StructuralPathCandidate> leftPaths = loadStructurePaths(pair.left);
                        const std::vector<StructuralPathCandidate> rightPaths = loadStructurePaths(pair.right);
                        if (leftPaths.empty() || rightPaths.empty()) {
                            structuralIoFailures.fetch_add(1, std::memory_order_relaxed);
                            structuralMissingPaths.fetch_add(1, std::memory_order_relaxed);
                            {
                                std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                                SkippedVisualContentRecord skipped;
                                skipped.primary_sha512 = pair.left;
                                skipped.secondary_sha512 = pair.right;
                                skipped.media_kind = MediaKind::Image;
                                skipped.reason = SkippedVisualContentReason::StructuralIoFailure;
                                if (skippedContents.size() < kMaxSkippedContentRecords) {
                                    skippedContents.push_back(std::move(skipped));
                                }
                            }
                            DuplicateReportDiagnostic diagnostic;
                            diagnostic.severity = DuplicateReportDiagnosticSeverity::Warning;
                            diagnostic.stage = "validating_image_structure";
                            diagnostic.operation = "structure_missing_path";
                            diagnostic.subject = pair.left + " <-> " + pair.right;
                            diagnostic.message = "候选对至少一侧没有可读活动路径，已跳过结构三筛";
                            PublishDiagnostic(diagnostic_callback, diagnostic);
                            verifiedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        StructuralCacheLookup leftStructure;
                        StructuralCacheLookup rightStructure;
                        if (!structuralScheduler.RunWithReadBudget(
                                leftPaths.front().storage_target_key_utf8,
                                [&] { leftStructure = structureCache.Get(pair.left, leftPaths); }) ||
                            !structuralScheduler.RunWithReadBudget(
                                rightPaths.front().storage_target_key_utf8,
                                [&] { rightStructure = structureCache.Get(pair.right, rightPaths); })) {
                            return;
                        }
                        if (!leftStructure.structure || !rightStructure.structure) {
                            structuralIoFailures.fetch_add(1, std::memory_order_relaxed);
                            const bool timedOut = leftStructure.status == StructuralLoadStatus::TimedOut ||
                                                  rightStructure.status == StructuralLoadStatus::TimedOut;
                            if (timedOut) {
                                structuralTimeouts.fetch_add(1, std::memory_order_relaxed);
                            }
                            {
                                std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                                SkippedVisualContentRecord skipped;
                                skipped.primary_sha512 = pair.left;
                                skipped.secondary_sha512 = pair.right;
                                skipped.media_kind = MediaKind::Image;
                                skipped.reason = timedOut
                                                     ? SkippedVisualContentReason::StructuralTimeout
                                                     : SkippedVisualContentReason::StructuralIoFailure;
                                if (skippedContents.size() < kMaxSkippedContentRecords) {
                                    skippedContents.push_back(std::move(skipped));
                                }
                            }
                            DuplicateReportDiagnostic diagnostic;
                            diagnostic.severity = DuplicateReportDiagnosticSeverity::Warning;
                            diagnostic.stage = "validating_image_structure";
                            diagnostic.operation = timedOut ? "structure_timeout" : "structure_load_failed";
                            diagnostic.status_code = static_cast<std::uint32_t>(
                                leftStructure.structure ? rightStructure.status : leftStructure.status);
                            diagnostic.subject = pair.left + " <-> " + pair.right;
                            diagnostic.message = timedOut
                                                     ? "候选对结构读取超时，已跳过当前候选"
                                                     : "候选对结构读取或解码失败，已跳过当前候选";
                            if (!leftStructure.error.empty()) {
                                diagnostic.message += "；左侧=" + leftStructure.error;
                            }
                            if (!rightStructure.error.empty()) {
                                diagnostic.message += "；右侧=" + rightStructure.error;
                            }
                            PublishDiagnostic(diagnostic_callback, diagnostic);
                            verifiedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        VideoScImageStructureCompareOptionsV1 options{};
                        options.structSize = sizeof(options);
                        const ImageSimilarityThresholdProfile& profile =
                            evidence.image_quality_class == ImageQualityClass::LowQuality
                                ? image_similarity_.low_quality_profile
                                : image_similarity_.standard_profile;
                        options.blockPassScore =
                            static_cast<double>(profile.structural_block_pass_score_millionths) /
                            1'000'000.0;
                        VideoScImageStructureCompareResultV1 compared{};
                        compared.structSize = sizeof(compared);
                        if (CompareImageStructuresV1(leftStructure.structure->native_handle(),
                                                     rightStructure.structure->native_handle(),
                                                     &options,
                                                     &compared) == 0) {
                            const std::string error = compared.errorMessage == nullptr
                                                          ? "Cannot compare image structures"
                                                          : compared.errorMessage;
                            FreeVideoScImageStructureCompareResultV1(&compared);
                            structuralComputeFailures.fetch_add(1, std::memory_order_relaxed);
                            {
                                std::lock_guard<std::mutex> skippedLock(skippedContentsMutex);
                                SkippedVisualContentRecord skipped;
                                skipped.primary_sha512 = pair.left;
                                skipped.secondary_sha512 = pair.right;
                                skipped.media_kind = MediaKind::Image;
                                skipped.reason = SkippedVisualContentReason::StructuralComputeFailure;
                                if (skippedContents.size() < kMaxSkippedContentRecords) {
                                    skippedContents.push_back(std::move(skipped));
                                }
                            }
                            DuplicateReportDiagnostic diagnostic;
                            diagnostic.severity = DuplicateReportDiagnosticSeverity::Warning;
                            diagnostic.stage = "validating_image_structure";
                            diagnostic.operation = "structure_compare_failed";
                            diagnostic.subject = pair.left + " <-> " + pair.right;
                            diagnostic.message = error + "；已跳过当前候选";
                            PublishDiagnostic(diagnostic_callback, diagnostic);
                            verifiedPairs.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        evidence.image_global_edge_zncc_millionths = compared.globalEdgeZnccMillionths;
                        evidence.image_trimmed_block_score_millionths = compared.trimmedBlockScoreMillionths;
                        evidence.image_passing_block_percent_millionths = compared.passingBlockPercentMillionths;
                        const bool accepted =
                            compared.globalEdgeZnccMillionths >=
                                profile.structural_global_edge_min_millionths &&
                            compared.trimmedBlockScoreMillionths >=
                                profile.structural_trimmed_block_min_millionths &&
                            compared.passingBlockPercentMillionths >=
                                profile.structural_min_passing_percent_millionths;
                        FreeVideoScImageStructureCompareResultV1(&compared);
                        if (accepted) {
                            const std::string encoded = SerializeEdge(kind, evidence);
                            const RocksStatus saved = store_.WriteBatch(
                                {{RocksColumnFamily::ExactIndex,
                                  workPrefix + "edge/" + pair.left + pair.right,
                                  encoded},
                                 {RocksColumnFamily::ExactIndex,
                                  workPrefix + "adjacency/" + pair.left + "/" + pair.right,
                                  encoded},
                                 {RocksColumnFamily::ExactIndex,
                                  workPrefix + "adjacency/" + pair.right + "/" + pair.left,
                                  encoded}},
                                false);
                            if (!saved.succeeded) throw std::runtime_error(saved.message);
                            acceptedImages.fetch_add(1, std::memory_order_relaxed);
                            if (evidence.image_quality_class == ImageQualityClass::LowQuality) {
                                lowQualityStructureAccepted.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            rejectedImages.fetch_add(1, std::memory_order_relaxed);
                        }
                        verifiedPairs.fetch_add(1, std::memory_order_relaxed);
                    });
                const TaskThreadPoolSnapshot pool = structuralPool.snapshot();
                progress.stage_processed = verifiedPairs.load(std::memory_order_relaxed);
                progress.active_validation_threads = pool.active_threads;
                publisher.Publish();
                return submitted;
            });
        structuralPool.CloseSubmissions();
        while (true) {
            const TaskThreadPoolSnapshot pool = structuralPool.snapshot();
            progress.stage_processed = verifiedPairs.load(std::memory_order_relaxed);
            progress.active_validation_threads = pool.active_threads;
            publisher.Publish();
            if (pool.queued_tasks == 0 && pool.active_threads == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        structuralPool.Join();
        if (!produced.succeeded && failure.empty()) failure = produced.message;
        if (!structuralPool.failure_message().empty()) failure = structuralPool.failure_message();
        if (!cancel_requested.load(std::memory_order_relaxed) && failure.empty() &&
            verifiedPairs.load(std::memory_order_relaxed) != secondaryPairCount) {
            failure = "structural validation candidate count mismatch";
        }
        progress.accepted_similarity_pairs =
            videoAcceptedPairCount + acceptedImages.load(std::memory_order_relaxed);
        progress.rejected_bucket_collisions += rejectedImages.load(std::memory_order_relaxed);
        lowQualityPairsStructureAcceptedCount =
            lowQualityStructureAccepted.load(std::memory_order_relaxed);
        structuralIoFailureCount = structuralIoFailures.load(std::memory_order_relaxed);
        structuralTimeoutCount = structuralTimeouts.load(std::memory_order_relaxed);
        structuralComputeFailureCount =
            structuralComputeFailures.load(std::memory_order_relaxed);
        progress.stage_processed = verifiedPairs.load(std::memory_order_relaxed);
        progress.active_validation_threads = 0;
    }
    progress.matched_pairs = identicalSignatureEdgeCount + progress.accepted_similarity_pairs;
    publisher.Publish(true);
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        result.message = failure;
        return result;
    }

    publisher.BeginStage("grouping_strict_similarity", 7, 9, true, representativeCount);
    const RocksStatus groupedRepresentatives = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        representativePrefix,
        0,
        [&](const std::string_view key, const std::string_view) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            if (key.size() != representativePrefix.size() + 128) {
                failure = "invalid visual representative key";
                return false;
            }
            const std::string currentHex(key.substr(representativePrefix.size()));
            std::unordered_set<std::string> candidateGroupSet;
            const std::string adjacencyPrefix = workPrefix + "adjacency/" + currentHex + "/";
            const RocksStatus adjacent = store_.ForEachPrefix(
                RocksColumnFamily::ExactIndex,
                adjacencyPrefix,
                0,
                [&](const std::string_view adjacentKey, const std::string_view) {
                    if (adjacentKey.size() != adjacencyPrefix.size() + 128) {
                        failure = "invalid dHash adjacency key";
                        return false;
                    }
                    const std::string candidateHex(adjacentKey.substr(adjacencyPrefix.size()));
                    if (candidateHex >= currentHex) return true;
                    std::string candidateGroup;
                    const RocksStatus groupStatus = store_.Get(
                        RocksColumnFamily::ExactIndex,
                        workPrefix + "group-of/" + candidateHex,
                        candidateGroup);
                    if (!groupStatus.succeeded) {
                        failure = groupStatus.message;
                        return false;
                    }
                    candidateGroupSet.insert(std::move(candidateGroup));
                    return true;
                });
            if (!adjacent.succeeded && failure.empty()) failure = adjacent.message;
            if (!failure.empty()) return false;

            std::vector<std::string> candidateGroups(candidateGroupSet.begin(), candidateGroupSet.end());
            std::sort(candidateGroups.begin(), candidateGroups.end());
            std::optional<std::string> bestGroup;
            std::optional<SimilarityEvidence> bestEvidence;
            DuplicateGroupKind bestKind = DuplicateGroupKind::SimilarImage;
            double bestMaximumDistance = std::numeric_limits<double>::infinity();
            double bestAverageDistance = std::numeric_limits<double>::infinity();

            for (const std::string& candidateGroup : candidateGroups) {
                bool completeLinkAccepted = true;
                std::uint64_t comparedMembers = 0;
                double totalDistance = 0.0;
                double maximumDistance = 0.0;
                std::optional<SimilarityEvidence> rootEvidence;
                DuplicateGroupKind groupKind = DuplicateGroupKind::SimilarImage;
                const std::string memberPrefix =
                    workPrefix + "group-content/" + candidateGroup + "/";
                const RocksStatus validated = store_.ForEachPrefix(
                    RocksColumnFamily::ExactIndex,
                    memberPrefix,
                    0,
                    [&](const std::string_view memberKey, const std::string_view) {
                        if (cancel_requested.load(std::memory_order_relaxed)) return false;
                        if (memberKey.size() != memberPrefix.size() + 128) {
                            failure = "invalid strict similarity group member key";
                            return false;
                        }
                        const std::string memberHex(memberKey.substr(memberPrefix.size()));
                        std::string encodedEdge;
                        const RocksStatus edgeStatus = store_.Get(
                            RocksColumnFamily::ExactIndex,
                            workPrefix + "edge/" + NormalizedPairKey(currentHex, memberHex),
                            encodedEdge);
                        if (!edgeStatus.succeeded) {
                            if (edgeStatus.message == "not_found") {
                                completeLinkAccepted = false;
                                return false;
                            }
                            failure = edgeStatus.message;
                            return false;
                        }
                        SimilarityEvidence evidence;
                        std::string decodeError;
                        if (!DeserializeEdge(encodedEdge, groupKind, evidence, decodeError)) {
                            failure = decodeError;
                            return false;
                        }
                        ++comparedMembers;
                        totalDistance += evidence.average_hamming_distance;
                        maximumDistance =
                            (std::max)(maximumDistance, evidence.average_hamming_distance);
                        if (memberHex == candidateGroup || !rootEvidence.has_value()) {
                            rootEvidence = evidence;
                        }
                        return true;
                    });
                if (!validated.succeeded && failure.empty() && completeLinkAccepted) {
                    failure = validated.message;
                }
                if (!failure.empty()) return false;
                if (!completeLinkAccepted || comparedMembers == 0 || !rootEvidence.has_value()) continue;

                const double averageDistance = totalDistance / static_cast<double>(comparedMembers);
                const bool isBetter =
                    !bestGroup.has_value() ||
                    maximumDistance < bestMaximumDistance ||
                    (maximumDistance == bestMaximumDistance &&
                     (averageDistance < bestAverageDistance ||
                      (averageDistance == bestAverageDistance && candidateGroup < *bestGroup)));
                if (isBetter) {
                    bestGroup = candidateGroup;
                    bestEvidence = rootEvidence;
                    bestKind = groupKind;
                    bestMaximumDistance = maximumDistance;
                    bestAverageDistance = averageDistance;
                }
            }

            const std::string assignedGroup = bestGroup.value_or(currentHex);
            std::uint64_t signatureCount = 0;
            std::uint64_t groupContentCount = 0;
            if (!readCounter(workPrefix + "signature-count/" + currentHex, signatureCount, false) ||
                !readCounter(workPrefix + "group-content-count/" + assignedGroup,
                             groupContentCount,
                             true)) {
                return false;
            }
            std::vector<RocksMutation> changes = {
                {RocksColumnFamily::ExactIndex,
                 workPrefix + "group-of/" + currentHex,
                 assignedGroup},
                {RocksColumnFamily::ExactIndex,
                 workPrefix + "group-content/" + assignedGroup + "/" + currentHex,
                 std::string{}},
                {RocksColumnFamily::ExactIndex,
                 workPrefix + "group-content-count/" + assignedGroup,
                 std::to_string(groupContentCount + signatureCount)},
            };
            if (bestGroup.has_value() && bestEvidence.has_value()) {
                changes.push_back(
                    {RocksColumnFamily::ExactIndex,
                     workPrefix + "evidence/" + assignedGroup + "/" +
                         NormalizedPairKey(assignedGroup, currentHex),
                     SerializeEdge(bestKind, *bestEvidence)});
            }
            const RocksStatus assigned = store_.WriteBatch(changes, false);
            if (!assigned.succeeded) {
                failure = assigned.message;
                return false;
            }
            ++progress.stage_processed;
            publisher.Publish();
            return true;
        });
    if (!groupedRepresentatives.succeeded && failure.empty()) {
        failure = groupedRepresentatives.message;
    }
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        publisher.Publish(true);
        return result;
    }
    if (!failure.empty()) {
        result.message = failure;
        publisher.Publish(true);
        return result;
    }

    // 相同签名的不同 SHA-512 内容使用线性证据边，避免组详情缺少距离为零的解释。
    const std::string identicalEdgePrefix = workPrefix + "identical-edge/";
    const RocksStatus organizedIdentical = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        identicalEdgePrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            if (key.size() != identicalEdgePrefix.size() + 256) {
                failure = "invalid identical visual edge key";
                return false;
            }
            const std::string leftHex(key.substr(identicalEdgePrefix.size(), 128));
            const std::string pairKey(key.substr(identicalEdgePrefix.size(), 256));
            std::string root;
            if (!resolveGroupRoot(leftHex, root)) {
                failure = failure.empty() ? "identical visual edge lost its strict group" : failure;
                return false;
            }
            const RocksStatus saved = store_.Put(
                RocksColumnFamily::ExactIndex,
                workPrefix + "evidence/" + root + "/" + pairKey,
                value,
                false);
            if (!saved.succeeded) failure = saved.message;
            return saved.succeeded;
        });
    if (!organizedIdentical.succeeded && failure.empty()) failure = organizedIdentical.message;
    if (!failure.empty()) {
        result.message = failure;
        publisher.Publish(true);
        return result;
    }

    // 只显式保存跨严格组图片边；组内完整关系由严格组不变量隐式保证。
    const std::string edgePrefix = workPrefix + "edge/";
    const RocksStatus organizedRelations = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        edgePrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            if (key.size() != edgePrefix.size() + 256) {
                failure = "invalid similarity edge key";
                return false;
            }
            const std::string leftHex(key.substr(edgePrefix.size(), 128));
            const std::string rightHex(key.substr(edgePrefix.size() + 128, 128));
            std::string leftRoot;
            std::string rightRoot;
            if (!resolveGroupRoot(leftHex, leftRoot) || !resolveGroupRoot(rightHex, rightRoot)) {
                failure = failure.empty() ? "similarity edge lost its strict group" : failure;
                return false;
            }
            if (leftRoot == rightRoot) return true;

            DuplicateGroupKind kind{};
            SimilarityEvidence evidence;
            std::string decodeError;
            if (!DeserializeEdge(std::string(value), kind, evidence, decodeError)) {
                failure = decodeError;
                return false;
            }
            if (kind != DuplicateGroupKind::SimilarImage) return true;
            ShaFileData left;
            ShaFileData right;
            if (!loadContentByHex(leftHex, left) || !loadContentByHex(rightHex, right)) {
                if (failure.empty()) failure = "cross-group image relation lost its content";
                return false;
            }

            SimilarImageRelationSummary leftRelation;
            leftRelation.current_group_id = StableGroupId(leftRoot, kind);
            leftRelation.neighbor_group_id = StableGroupId(rightRoot, kind);
            leftRelation.current_representative_sha512 = leftHex;
            leftRelation.neighbor_representative_sha512 = rightHex;
            leftRelation.current_image_dhash = left.image_dhash.value_or(0);
            leftRelation.neighbor_image_dhash = right.image_dhash.value_or(0);
            leftRelation.hamming_distance = evidence.frame_distances[0];

            SimilarImageRelationSummary rightRelation;
            rightRelation.current_group_id = leftRelation.neighbor_group_id;
            rightRelation.neighbor_group_id = leftRelation.current_group_id;
            rightRelation.current_representative_sha512 = rightHex;
            rightRelation.neighbor_representative_sha512 = leftHex;
            rightRelation.current_image_dhash = right.image_dhash.value_or(0);
            rightRelation.neighbor_image_dhash = left.image_dhash.value_or(0);
            rightRelation.hamming_distance = evidence.frame_distances[0];

            const std::string pairKey = leftHex + rightHex;
            const RocksStatus saved = store_.WriteBatch(
                {{RocksColumnFamily::ExactIndex,
                  workPrefix + "cross-relation/" + leftRoot + "/" + pairKey,
                  SerializeImageRelation(leftRelation)},
                 {RocksColumnFamily::ExactIndex,
                  workPrefix + "cross-relation/" + rightRoot + "/" + pairKey,
                  SerializeImageRelation(rightRelation)},
                 {RocksColumnFamily::ExactIndex,
                  workPrefix + "relation-signature/" + leftHex,
                  std::string{}},
                 {RocksColumnFamily::ExactIndex,
                  workPrefix + "relation-signature/" + rightHex,
                  std::string{}}},
                false);
            if (!saved.succeeded) {
                failure = saved.message;
                return false;
            }
            ++crossRelationCount;
            return true;
        });
    if (!organizedRelations.succeeded && failure.empty()) failure = organizedRelations.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.message = "cancelled";
        publisher.Publish(true);
        return result;
    }
    if (!failure.empty()) {
        result.message = failure;
        publisher.Publish(true);
        return result;
    }

    const std::string groupContentCountPrefix = workPrefix + "group-content-count/";
    const RocksStatus countedRoots = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        groupContentCountPrefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            const std::optional<std::uint64_t> count = ParseDecimal(std::string(value));
            if (!count.has_value()) {
                failure = "invalid strict similarity group content count";
                return false;
            }
            if (*count >= 2) ++groupRootCount;
            return true;
        });
    if (!countedRoots.succeeded && failure.empty()) failure = countedRoots.message;
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    // 跳过记录按 SHA 建索引，供活动路径流匹配样例路径。
    std::unordered_map<std::string, std::vector<std::size_t>> skippedContentIndex;
    for (std::size_t index = 0; index < skippedContents.size(); ++index) {
        skippedContentIndex[skippedContents[index].primary_sha512].push_back(index);
        if (!skippedContents[index].secondary_sha512.empty()) {
            skippedContentIndex[skippedContents[index].secondary_sha512].push_back(index);
        }
    }

    publisher.BeginStage("joining_active_paths", 8, 9, false, 0);
    const MySqlStatus joined = repository.StreamActivePaths(
        [&](FilePathRecord&& path) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            ++progress.processed_paths;
            const auto finishCurrentPath = [&]() {
                ++progress.stage_processed;
                publisher.Publish();
                return true;
            };
            if (!IsActivePath(path)) return finishCurrentPath();
            const std::string digestHex = Sha512ToHex(*path.sha512);
            const auto skippedMatch = skippedContentIndex.find(digestHex);
            if (skippedMatch != skippedContentIndex.end()) {
                for (const std::size_t skippedIndex : skippedMatch->second) {
                    SkippedVisualContentRecord& skipped = skippedContents[skippedIndex];
                    ++skipped.active_path_count;
                    if (skipped.sample_paths.size() < 4) {
                        skipped.sample_paths.push_back(path.path.wstring());
                    }
                }
            }
            std::string representative;
            if (!resolveVisualRepresentative(digestHex, representative)) {
                return failure.empty() ? finishCurrentPath() : false;
            }
            std::string root;
            if (!resolveGroupRoot(digestHex, root)) {
                return failure.empty() ? finishCurrentPath() : false;
            }
            std::uint64_t distinctContentCount = 0;
            if (!readCounter(workPrefix + "group-content-count/" + root,
                             distinctContentCount,
                             false)) {
                return false;
            }
            const bool includeMainGroup = distinctContentCount >= 2;
            std::string ignored;
            const RocksStatus relationMarker = store_.Get(
                RocksColumnFamily::ExactIndex,
                workPrefix + "relation-signature/" + representative,
                ignored);
            const bool includeRelationSignature = relationMarker.succeeded;
            if (!includeRelationSignature && relationMarker.message != "not_found") {
                failure = relationMarker.message;
                return false;
            }
            if (!includeMainGroup && !includeRelationSignature) return finishCurrentPath();
            std::string contentError;
            std::optional<ShaFileData> content =
                LoadWorkContent(store_, workPrefix, *path.sha512, contentError);
            if (!content.has_value()) {
                failure = contentError;
                return false;
            }
            DuplicateMember member;
            member.path_id = path.path_id;
            member.content_sha512 = *path.sha512;
            member.path = path.path;
            member.storage_target_key = path.storage_target_key;
            member.size_bytes = path.size_bytes;
            member.width = content->width;
            member.height = content->height;
            member.bitrate = content->video_bitrate;
            member.last_write_time_utc_ms = path.last_write_time_utc_ms;
            member.scan_root_priority = path.scan_root_priority;
            member.thumbnail_path = content->contact_sheet_path.empty() ? path.path : content->contact_sheet_path;
            member.media_kind = content->media_kind;
            member.image_dhash = content->image_dhash;
            member.video_dhashes = content->video_dhashes;
            member.has_video_dhashes = content->has_video_dhashes;

            if (includeMainGroup) {
                const std::string encodedMember = SerializeStandaloneMember(member);
                const std::string countKey = workPrefix + "count/" + root;
                std::string countValue;
                const RocksStatus currentCount =
                    store_.Get(RocksColumnFamily::ExactIndex, countKey, countValue);
                std::uint64_t count = 0;
                if (currentCount.succeeded) {
                    const auto parsed = ParseDecimal(countValue);
                    if (!parsed.has_value()) {
                        failure = "invalid similarity member count";
                        return false;
                    }
                    count = *parsed;
                } else if (currentCount.message != "not_found") {
                    failure = currentCount.message;
                    return false;
                }
                const RocksStatus saved = store_.WriteBatch(
                    {{RocksColumnFamily::ExactIndex,
                      workPrefix + "member/" + root + "/" + Hex64(path.path_id),
                      encodedMember},
                     {RocksColumnFamily::ExactIndex, countKey, std::to_string(count + 1)},
                     {RocksColumnFamily::ExactIndex,
                      workPrefix + "first/" + digestHex,
                      Hex64(path.path_id)}},
                    false);
                if (!saved.succeeded) {
                    failure = saved.message;
                    return false;
                }
            }
            if (includeRelationSignature && content->media_kind == MediaKind::Image) {
                const RocksStatus savedSignature = reportStore.SaveImageSignatureMember(
                    result.generation_id,
                    representative,
                    member);
                if (!savedSignature.succeeded) {
                    failure = savedSignature.message;
                    return false;
                }
            }
            return finishCurrentPath();
        });
    if (!joined.succeeded && failure.empty()) failure = joined.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    groupRootCount = 0;
    const std::string countPrefix = workPrefix + "count/";
    const RocksStatus countedActiveRoots = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        countPrefix,
        0,
        [&](const std::string_view, const std::string_view value) {
            const std::optional<std::uint64_t> count = ParseDecimal(std::string(value));
            if (!count.has_value()) {
                failure = "invalid active similarity group member count";
                return false;
            }
            if (*count >= 2) ++groupRootCount;
            return true;
        });
    if (!countedActiveRoots.succeeded && failure.empty()) {
        failure = countedActiveRoots.message;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    publisher.BeginStage("writing_similarity_groups", 9, 9, true, groupRootCount);
    SimilarReportMetadata metadata;
    metadata.image_max_hamming_distance =
        image_similarity_.standard_profile.pdq_max_hamming_distance;
    metadata.video_max_average_hamming_distance =
        dhash_similarity_.video_max_average_hamming_distance;
    metadata.image_aspect_ratio_tolerance_percent =
        image_similarity_.aspect_ratio_tolerance_percent;
    metadata.validation_worker_threads =
        image_similarity_.report_validation_worker_threads;
    metadata.image_similarity = image_similarity_;
    metadata.structural_worker_threads = structuralScheduler.effective_worker_count();
    metadata.structural_cache_mib = image_similarity_.structural_cache_mib;
    metadata.image_uses_three_stage_verification = true;
    metadata.image_scope_total = imageCompleteness.total_images;
    metadata.image_features_complete = imageCompleteness.complete_images;
    metadata.image_features_incomplete = imageCompleteness.incomplete_images;
    metadata.partial_scope_published = result.skipped_invalid_visuals != 0 ||
                                       structuralIoFailureCount != 0 ||
                                       structuralComputeFailureCount != 0;
    metadata.deferred_hot_signatures = deferredHotSignatures;
    metadata.candidate_peak_bytes = candidatePeakBytes;
    metadata.candidate_pairs = progress.candidate_pairs_total;
    metadata.popcount_path = image_similarity_.force_scalar_kernels
                                 ? "forced-portable-swar"
                                 : (ImageSimilarityRules::UsesHardwarePopcnt()
                                        ? "runtime-popcnt"
                                        : "portable-swar");
    metadata.generated_utc_ms = CurrentUtcMilliseconds();
    metadata.media_algorithm_version = algorithm_version_;
    const RocksStatus savedMetadata =
        reportStore.SaveSimilarMetadata(result.generation_id, metadata);
    if (!savedMetadata.succeeded) {
        result.message = savedMetadata.message;
        publisher.Publish(true);
        return result;
    }

    const RocksStatus savedSkippedContents =
        reportStore.SaveSkippedContents(result.generation_id, skippedContents);
    if (!savedSkippedContents.succeeded) {
        result.message = savedSkippedContents.message;
        publisher.Publish(true);
        return result;
    }

    const RocksStatus writtenGroups = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        countPrefix,
        0,
        [&](const std::string_view key, const std::string_view value) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            const std::optional<std::uint64_t> memberCount = ParseDecimal(std::string(value));
            if (!memberCount.has_value()) {
                failure = "invalid similarity group count";
                return false;
            }
            if (*memberCount < 2) {
                ++progress.stage_processed;
                publisher.Publish();
                return true;
            }
            const std::string root(key.substr(countPrefix.size()));
            DuplicateGroup group;
            group.algorithm_version = algorithm_version_;
            group.members.reserve(static_cast<std::size_t>(
                (std::min<std::uint64_t>)(*memberCount, std::numeric_limits<std::size_t>::max())));
            const std::string memberPrefix = workPrefix + "member/" + root + "/";
            const RocksStatus members = store_.ForEachPrefix(
                RocksColumnFamily::ExactIndex,
                memberPrefix,
                0,
                [&](const std::string_view, const std::string_view memberValue) {
                    try {
                        const std::string encodedMember(memberValue);
                        Reader reader(encodedMember);
                        const std::uint8_t codecVersion = reader.Read<std::uint8_t>();
                        if (codecVersion != kGroupCodecVersion) {
                            throw std::runtime_error("Unsupported similarity member");
                        }
                        group.members.push_back(ReadMember(reader, codecVersion));
                        reader.RequireEnd();
                        publisher.Publish();
                        return true;
                    } catch (const std::exception& exception) {
                        failure = exception.what();
                        return false;
                    }
                });
            if (!members.succeeded) failure = members.message;
            if (!failure.empty()) return false;

            DuplicateGroupKind groupKind = DuplicateGroupKind::SimilarImage;
            const std::string evidencePrefix = workPrefix + "evidence/" + root + "/";
            const RocksStatus evidenceStatus = store_.ForEachPrefix(
                RocksColumnFamily::ExactIndex,
                evidencePrefix,
                0,
                [&](const std::string_view evidenceKey, const std::string_view evidenceValue) {
                    if (evidenceKey.size() != evidencePrefix.size() + 256) {
                        failure = "invalid strict group evidence key";
                        return false;
                    }
                    SimilarityEvidence evidence;
                    std::string decodeError;
                    if (!DeserializeEdge(std::string(evidenceValue), groupKind, evidence, decodeError)) {
                        failure = decodeError;
                        return false;
                    }
                    const std::string leftHex(evidenceKey.substr(evidencePrefix.size(), 128));
                    const std::string rightHex(evidenceKey.substr(evidencePrefix.size() + 128, 128));
                    std::string leftPath;
                    std::string rightPath;
                    const RocksStatus leftStatus = store_.Get(
                        RocksColumnFamily::ExactIndex,
                        workPrefix + "first/" + leftHex,
                        leftPath);
                    const RocksStatus rightStatus = store_.Get(
                        RocksColumnFamily::ExactIndex,
                        workPrefix + "first/" + rightHex,
                        rightPath);
                    if (!leftStatus.succeeded || !rightStatus.succeeded) return true;
                    const std::optional<std::uint64_t> leftId = ParseHex64(leftPath);
                    const std::optional<std::uint64_t> rightId = ParseHex64(rightPath);
                    if (!leftId.has_value() || !rightId.has_value()) return true;
                    evidence.left_path_id = *leftId;
                    evidence.right_path_id = *rightId;
                    group.evidence.push_back(evidence);
                    publisher.Publish();
                    return true;
                });
            if (!evidenceStatus.succeeded) failure = evidenceStatus.message;
            if (!failure.empty()) return false;
            if (group.evidence.empty() && !group.members.empty()) {
                groupKind = group.members.front().media_kind == MediaKind::Video
                                ? DuplicateGroupKind::SimilarVideo
                                : DuplicateGroupKind::SimilarImage;
            }
            group.kind = groupKind;
            group.group_id = StableGroupId(root, group.kind);
            std::uint64_t totalSize = 0;
            std::uint64_t minimumSize = std::numeric_limits<std::uint64_t>::max();
            for (const DuplicateMember& member : group.members) {
                totalSize += member.size_bytes;
                minimumSize = (std::min)(minimumSize, member.size_bytes);
            }
            group.reclaimable_bytes = totalSize >= minimumSize ? totalSize - minimumSize : 0;
            const RocksStatus saved = reportStore.SaveGroup(DuplicateReportKind::Similar,
                                                            result.generation_id,
                                                            progress.emitted_groups,
                                                            group);
            if (!saved.succeeded) {
                failure = saved.message;
                return false;
            }
            const RocksStatus markedMain = store_.Put(
                RocksColumnFamily::ExactIndex,
                workPrefix + "main-group-id/" + Hex64(group.group_id),
                std::string{},
                false);
            if (!markedMain.succeeded) {
                failure = markedMain.message;
                return false;
            }
            ++progress.emitted_groups;
            ++progress.stage_processed;
            publisher.Publish();
            return true;
        });
    if (!writtenGroups.succeeded && failure.empty()) failure = writtenGroups.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    const std::string crossRelationPrefix = workPrefix + "cross-relation/";
    const RocksStatus writtenRelations = store_.ForEachPrefix(
        RocksColumnFamily::ExactIndex,
        crossRelationPrefix,
        0,
        [&](const std::string_view, const std::string_view relationValue) {
            if (cancel_requested.load(std::memory_order_relaxed) || !failure.empty()) return false;
            std::string decodeError;
            std::optional<SimilarImageRelationSummary> relation =
                DeserializeImageRelation(0, std::string(relationValue), decodeError);
            if (!relation.has_value()) {
                failure = decodeError;
                return false;
            }
            const std::uint64_t currentMemberCount =
                reportStore.ImageSignatureMemberCount(
                    result.generation_id,
                    relation->current_representative_sha512);
            relation->neighbor_active_member_count =
                reportStore.ImageSignatureMemberCount(
                    result.generation_id,
                    relation->neighbor_representative_sha512);
            if (currentMemberCount == 0 || relation->neighbor_active_member_count == 0) {
                return true;
            }
            std::string ignored;
            relation->neighbor_group_in_main_report =
                store_.Get(
                    RocksColumnFamily::ExactIndex,
                    workPrefix + "main-group-id/" + Hex64(relation->neighbor_group_id),
                    ignored)
                    .succeeded;
            const RocksStatus saved = reportStore.SaveImageRelation(
                result.generation_id,
                relation->current_group_id,
                *relation);
            if (!saved.succeeded) {
                failure = saved.message;
                return false;
            }
            return true;
        });
    if (!writtenRelations.succeeded && failure.empty()) failure = writtenRelations.message;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        publisher.Publish(true);
        result.cancelled = true;
        result.message = "cancelled";
        return result;
    }
    if (!failure.empty()) {
        publisher.Publish(true);
        result.message = failure;
        return result;
    }

    const RocksStatus published = reportStore.Publish(DuplicateReportKind::Similar,
                                                      result.generation_id,
                                                      progress.emitted_groups);
    if (!published.succeeded) {
        result.message = published.message;
        return result;
    }
    cleanup.MarkPublished();
    result.succeeded = true;
    result.group_count = progress.emitted_groups;
    result.low_quality_pairs_evaluated = lowQualityPairsEvaluatedCount;
    result.low_quality_pairs_secondary_passed = lowQualityPairsSecondaryPassedCount;
    result.low_quality_pairs_structure_accepted = lowQualityPairsStructureAcceptedCount;
    result.structural_io_failures = structuralIoFailureCount;
    result.structural_timeouts = structuralTimeoutCount;
    result.structural_compute_failures = structuralComputeFailureCount;
    result.message = "ok; candidate_pairs=" +
                     std::to_string(progress.candidate_pairs_total) +
                     "; validated_pairs=" +
                     std::to_string(progress.validated_candidate_pairs) +
                     "; accepted_pairs=" +
                     std::to_string(progress.accepted_similarity_pairs) +
                     "; rejected_bucket_collisions=" +
                     std::to_string(progress.rejected_bucket_collisions) +
                     "; secondary_pairs=" +
                     std::to_string(secondaryPairCount) +
                     "; structure_cache_hits=" +
                     std::to_string(structureCache.stats().hits) +
                     "; structure_decodes=" +
                     std::to_string(structureCache.stats().decodes) +
                     "; structure_path_retries=" +
                     std::to_string(structureCache.stats().path_retries) +
                     "; structure_timeouts=" +
                     std::to_string(structureCache.stats().timeouts) +
                     "; structure_decode_failures=" +
                     std::to_string(structureCache.stats().decode_failures) +
                     "; structural_io_failures=" +
                     std::to_string(structuralIoFailureCount) +
                     "; structural_compute_failures=" +
                     std::to_string(structuralComputeFailureCount) +
                     "; low_quality_evaluated=" +
                     std::to_string(lowQualityPairsEvaluatedCount) +
                     "; low_quality_secondary_passed=" +
                     std::to_string(lowQualityPairsSecondaryPassedCount) +
                     "; low_quality_structure_accepted=" +
                     std::to_string(lowQualityPairsStructureAcceptedCount) +
                     "; candidate_peak_bytes=" +
                     std::to_string(candidatePeakBytes) +
                     "; cross_relations=" +
                     std::to_string(crossRelationCount);
    if (result.skipped_invalid_visuals != 0) {
        result.message += "; skipped_invalid_visuals=" + std::to_string(result.skipped_invalid_visuals) +
                          "; skipped_invalid_images=" + std::to_string(result.skipped_invalid_images) +
                          "; skipped_invalid_videos=" + std::to_string(result.skipped_invalid_videos);
    }
    progress.stage = "completed";
    progress.stage_processed = progress.stage_total;
    progress.stage_total_known = true;
    publisher.Publish(true);
    return result;
}

}  // namespace videosc::dedup
