#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief SHA-512 固定长度二进制摘要，数据库和 RocksDB 均以 64 字节保存。 */
using Sha512Digest = std::array<std::uint8_t, 64>;
/** @brief PDQ-256 的规范化 32 字节表示。 */
using PdqHash256 = std::array<std::uint8_t, 32>;

/** @brief 文件内容类型；音频只参与 SHA-512 精确去重。 */
enum class MediaKind {
    Other,
    Image,
    Video,
    Audio,
};

/** @brief 图片对在三级筛选中实际使用的阈值类别。 */
enum class ImageQualityClass : std::uint8_t {
    Standard = 0,
    LowQuality = 1,
};

/** @brief 路径相对最近一次扫描配置和文件系统的状态。 */
enum class FilePathState {
    Pending,
    Available,
    Unchanged,
    Changed,
    Missing,
    Offline,
    OutOfScope,
    ProgramDeleted,
    Unreadable,
    ReadTimeout,
};

/** @brief RocksDB 结果相对 MySQL 的同步状态。 */
enum class SyncState {
    LocalOnly,
    Pending,
    Synchronized,
    FailedRetryable,
};

/** @brief 重启和删除前复核使用的稳定 Windows 文件身份。 */
struct FileIdentity {
    std::uint64_t volume_serial = 0;
    std::uint64_t file_id_high = 0;
    std::uint64_t file_id_low = 0;
};

/**
 * @brief 路径到 SHA-512 映射及扫描所需的路径级元数据。
 *
 * 该记录允许路径删除而不删除 ShaFileData；normalized_path_key 用于大小写无关的路径唯一约束。
 */
struct FilePathRecord {
    std::uint64_t path_id = 0;
    std::uint64_t scan_id = 0;
    std::filesystem::path path;
    std::wstring normalized_path_key;
    std::optional<Sha512Digest> sha512;
    FileIdentity identity;
    std::wstring volume_guid;
    std::wstring storage_target_key;
    std::uint64_t size_bytes = 0;
    std::wstring extension;
    std::int64_t creation_time_utc_ms = 0;
    std::int64_t last_write_time_utc_ms = 0;
    std::uint32_t scan_root_priority = 0;
    FilePathState state = FilePathState::Pending;
    SyncState sync_state = SyncState::LocalOnly;
};

/**
 * @brief SHA-512 唯一内容对应的媒体数据。
 *
 * 视频只保存画面相关字段、六个固定采样 dHash 和一张 2x3 拼图路径，不保存音轨信息。
 */
struct ShaFileData {
    Sha512Digest sha512{};
    std::uint64_t content_size_bytes = 0;
    MediaKind media_kind = MediaKind::Other;
    std::string mime_type;
    std::string container_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::uint64_t> image_dhash;
    /** 图片 PDQ-256；字节顺序与 VideoSc V1 图片感知接口一致。 */
    std::optional<PdqHash256> image_pdq_hash;
    /** PDQ 官方质量分，只有 image_pdq_hash 有值时有效。 */
    std::optional<std::uint8_t> image_pdq_quality;
    /** 4×4 固定坐标分区 pHash，行优先保存。 */
    std::array<std::uint64_t, 16> image_zoned_phashes{};
    bool has_image_zoned_phashes = false;
    /** 图片感知特征算法版本；零表示历史记录尚未回填。 */
    std::uint32_t image_perceptual_algorithm_version = 0;
    /** 结构直验算法版本；结构面本身不持久化。 */
    std::uint32_t image_structural_algorithm_version = 0;
    std::int64_t video_duration_ms = 0;
    double video_frame_rate = 0.0;
    std::uint64_t video_bitrate = 0;
    std::string video_codec;
    std::string pixel_format;
    std::array<std::uint64_t, 6> video_dhashes{};
    bool has_video_dhashes = false;
    bool static_visual = false;
    std::filesystem::path contact_sheet_path;
    std::string media_algorithm_version;
};

/** @brief 重复组类型，精确组自动生成，内容相似组由用户手动触发。 */
enum class DuplicateGroupKind {
    ExactSha512,
    SimilarImage,
    SimilarVideo,
};

/** @brief 组内一个文件路径成员及用于保留策略的质量字段。 */
struct DuplicateMember {
    std::uint64_t path_id = 0;
    Sha512Digest content_sha512{};
    std::filesystem::path path;
    std::wstring storage_target_key;
    std::uint64_t size_bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t bitrate = 0;
    std::int64_t last_write_time_utc_ms = 0;
    std::uint32_t scan_root_priority = 0;
    std::filesystem::path thumbnail_path;
    MediaKind media_kind = MediaKind::Other;
    std::optional<std::uint64_t> image_dhash;
    std::array<std::uint64_t, 6> video_dhashes{};
    bool has_video_dhashes = false;
};

/** @brief 视觉相似组的量化证据，不参与精确 SHA-512 组。 */
struct SimilarityEvidence {
    std::uint64_t left_path_id = 0;
    std::uint64_t right_path_id = 0;
    std::array<std::uint8_t, 6> frame_distances{};
    std::uint32_t compared_frame_count = 0;
    double average_hamming_distance = 0.0;
    std::int64_t duration_difference_ms = 0;
    /** 图片对实际应用的阈值类别；视频证据固定为 Standard。 */
    ImageQualityClass image_quality_class = ImageQualityClass::Standard;
    /** 图片一筛 PDQ-256 完整汉明距离；视频与旧报告为零。 */
    std::uint16_t image_pdq_hamming_distance = 0;
    /** 图片一筛兼容 dHash 完整汉明距离；视频与旧报告为零。 */
    std::uint8_t image_dhash_hamming_distance = 0;
    /** PDQ 超限后是否由严格 dHash 回退路径召回进入二筛。 */
    bool image_used_dhash_fallback = false;
    /** 图片二筛中距离不超过单区阈值的分区数。 */
    std::uint8_t image_zoned_passing_tiles = 0;
    /** 图片二筛裁剪统计忽略的最差分区数。 */
    std::uint8_t image_zoned_ignored_tiles = 0;
    /** 图片二筛保留分区的距离和及数量。 */
    std::uint16_t image_zoned_trimmed_distance_sum = 0;
    std::uint8_t image_zoned_retained_tiles = 0;
    /** 图片三筛结构证据，均为 0～1,000,000 的整数分。 */
    std::uint32_t image_global_edge_zncc_millionths = 0;
    std::uint32_t image_trimmed_block_score_millionths = 0;
    std::uint32_t image_passing_block_percent_millionths = 0;
};

/**
 * @brief 可分页审阅的重复组。
 *
 * selected_for_deletion 只属于当前删除计划，不写入 ShaFileData；执行前必须重新保证至少一个成员未选中。
 */
struct DuplicateGroup {
    std::uint64_t group_id = 0;
    DuplicateGroupKind kind = DuplicateGroupKind::ExactSha512;
    std::string algorithm_version;
    std::vector<DuplicateMember> members;
    std::vector<SimilarityEvidence> evidence;
    std::optional<std::uint64_t> retained_path_id;
    std::vector<std::uint64_t> selected_for_deletion;
    std::uint64_t reclaimable_bytes = 0;
};

/**
 * @brief 把 64 字节 SHA-512 摘要转换为 128 字符小写十六进制。
 * @param digest 二进制摘要。
 * @return 稳定的小写十六进制文本。
 */
std::string Sha512ToHex(const Sha512Digest& digest);

/**
 * @brief 解析严格的 128 字符 SHA-512 十六进制文本。
 * @param hex 大小写均可的十六进制文本。
 * @return 成功时返回二进制摘要，长度或字符非法时返回空。
 */
std::optional<Sha512Digest> Sha512FromHex(const std::string& hex);

}  // namespace videosc::dedup
