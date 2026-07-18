#include "ExactDuplicateReader.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace videosc::dedup {
namespace {

/** @brief 由摘要生成稳定的 64 位报告组 ID；完整摘要仍保留在每个成员中用于碰撞复核。 */
std::uint64_t StableGroupId(const Sha512Digest& digest) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const std::uint8_t byte : digest) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

/** @brief 严格解析 MySQL 无符号十进制列。 */
template <typename Value>
Value ParseUnsigned(const std::optional<std::string>& column, const char* field) {
    if (!column.has_value()) throw std::runtime_error(std::string("NULL exact group field: ") + field);
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(column->data(), column->data() + column->size(), parsed);
    if (result.ec != std::errc{} || result.ptr != column->data() + column->size() ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
        throw std::runtime_error(std::string("Invalid exact group field: ") + field);
    }
    return static_cast<Value>(parsed);
}

/** @brief 严格解析 MySQL 有符号十进制列。 */
std::int64_t ParseSigned(const std::optional<std::string>& column, const char* field) {
    if (!column.has_value()) throw std::runtime_error(std::string("NULL exact group field: ") + field);
    std::int64_t parsed = 0;
    const auto result = std::from_chars(column->data(), column->data() + column->size(), parsed);
    if (result.ec != std::errc{} || result.ptr != column->data() + column->size()) {
        throw std::runtime_error(std::string("Invalid exact group field: ") + field);
    }
    return parsed;
}

std::array<std::uint64_t, 6> ParseVideoDHashes(const std::string& hex) {
    if (hex.size() != 96) throw std::runtime_error("Invalid exact group video dHash bytes");
    const auto nibble = [](const char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') return static_cast<std::uint8_t>(character - '0');
        if (character >= 'A' && character <= 'F') return static_cast<std::uint8_t>(character - 'A' + 10);
        if (character >= 'a' && character <= 'f') return static_cast<std::uint8_t>(character - 'a' + 10);
        throw std::runtime_error("Invalid exact group video dHash hex");
    };
    std::array<std::uint64_t, 6> hashes{};
    for (std::size_t hashIndex = 0; hashIndex < hashes.size(); ++hashIndex) {
        std::uint64_t value = 0;
        for (std::size_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
            const std::size_t offset = (hashIndex * 8 + byteIndex) * 2;
            value = (value << 8) |
                    static_cast<std::uint64_t>((nibble(hex[offset]) << 4) | nibble(hex[offset + 1]));
        }
        hashes[hashIndex] = value;
    }
    return hashes;
}

/** @brief 严格 UTF-8 到 UTF-16 转换。 */
std::wstring Utf8ToWide(const std::optional<std::string>& value, const char* field) {
    if (!value.has_value()) throw std::runtime_error(std::string("NULL exact group field: ") + field);
    if (value->empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value->data(),
                                           static_cast<int>(value->size()),
                                           nullptr,
                                           0);
    if (length <= 0) throw std::runtime_error(std::string("Invalid UTF-8 exact group field: ") + field);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value->data(),
                            static_cast<int>(value->size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error(std::string("Invalid UTF-8 exact group field: ") + field);
    }
    return result;
}

}  // namespace

ExactGroupAccumulator::ExactGroupAccumulator(GroupVisitor visitor) : visitor_(std::move(visitor)) {
    if (!visitor_) throw std::invalid_argument("Exact group visitor is required");
}

bool ExactGroupAccumulator::Consume(const Sha512Digest& sha512, DuplicateMember member) {
    if (stopped_) return false;
    if (current_sha512_.has_value() && sha512 < *current_sha512_) {
        throw std::runtime_error("Exact group input is not ordered by SHA-512");
    }
    if (current_sha512_.has_value() && sha512 != *current_sha512_) {
        if (!FlushCurrent()) return false;
    }
    if (!current_sha512_.has_value()) current_sha512_ = sha512;
    member.content_sha512 = sha512;
    current_members_.push_back(std::move(member));
    ++consumed_members_;
    return true;
}

bool ExactGroupAccumulator::Finish() {
    if (stopped_) return false;
    return FlushCurrent();
}

std::uint64_t ExactGroupAccumulator::consumed_members() const noexcept {
    return consumed_members_;
}

std::uint64_t ExactGroupAccumulator::emitted_groups() const noexcept {
    return emitted_groups_;
}

bool ExactGroupAccumulator::FlushCurrent() {
    if (!current_sha512_.has_value()) return true;
    if (current_members_.size() >= 2) {
        DuplicateGroup group;
        group.group_id = StableGroupId(*current_sha512_);
        group.kind = DuplicateGroupKind::ExactSha512;
        group.algorithm_version = "sha512-exact-v1";
        group.members = std::move(current_members_);
        group.reclaimable_bytes = group.members.front().size_bytes * (group.members.size() - 1);
        ++emitted_groups_;
        if (!visitor_(std::move(group))) {
            stopped_ = true;
            current_sha512_.reset();
            current_members_.clear();
            return false;
        }
    }
    current_members_.clear();
    current_sha512_.reset();
    return true;
}

ExactDuplicateReader::ExactDuplicateReader(MySqlClient& client) : client_(client) {}

MySqlStatus ExactDuplicateReader::Stream(const ExactGroupAccumulator::GroupVisitor& visitor) {
    try {
        ExactGroupAccumulator accumulator(visitor);
        bool continueReading = true;
        const MySqlStatus query = client_.Query(
            "SELECT HEX(p.sha512),p.path_id,p.full_path,p.storage_target_key,p.size_bytes,"
            "d.width,d.height,d.video_bitrate,p.last_write_time_utc_ms,p.scan_root_priority,d.contact_sheet_path,"
            "d.media_kind,d.image_dhash,HEX(d.video_dhashes),d.has_video_dhashes "
            "FROM file_path_sha512 AS p FORCE INDEX (ix_exact_active_sha) "
            "LEFT JOIN sha512_file_data AS d ON d.sha512=p.sha512 "
            "WHERE p.active=1 ORDER BY p.sha512,p.path_id",
            [&](const MySqlRow& row) {
                if (row.size() != 15 || !row[0].has_value()) {
                    throw std::runtime_error("Unexpected exact duplicate query shape");
                }
                const std::optional<Sha512Digest> digest = Sha512FromHex(*row[0]);
                if (!digest.has_value()) throw std::runtime_error("Invalid SHA-512 returned by MySQL");
                DuplicateMember member;
                member.path_id = ParseUnsigned<std::uint64_t>(row[1], "path_id");
                member.path = Utf8ToWide(row[2], "full_path");
                member.storage_target_key = Utf8ToWide(row[3], "storage_target_key");
                member.size_bytes = ParseUnsigned<std::uint64_t>(row[4], "size_bytes");
                member.width = ParseUnsigned<std::uint32_t>(row[5], "width");
                member.height = ParseUnsigned<std::uint32_t>(row[6], "height");
                member.bitrate = ParseUnsigned<std::uint64_t>(row[7], "video_bitrate");
                member.last_write_time_utc_ms = ParseSigned(row[8], "last_write_time_utc_ms");
                member.scan_root_priority = ParseUnsigned<std::uint32_t>(row[9], "scan_root_priority");
                member.thumbnail_path = Utf8ToWide(row[10], "contact_sheet_path");
                const std::uint8_t mediaKind = ParseUnsigned<std::uint8_t>(row[11], "media_kind");
                if (mediaKind > static_cast<std::uint8_t>(MediaKind::Audio)) {
                    throw std::runtime_error("Invalid exact group media kind");
                }
                member.media_kind = static_cast<MediaKind>(mediaKind);
                if (row[12].has_value()) {
                    member.image_dhash = ParseUnsigned<std::uint64_t>(row[12], "image_dhash");
                }
                member.has_video_dhashes = ParseUnsigned<std::uint8_t>(row[14], "has_video_dhashes") != 0;
                if (member.has_video_dhashes) {
                    if (!row[13].has_value()) {
                        throw std::runtime_error("Exact group video dHash flag has no bytes");
                    }
                    member.video_dhashes = ParseVideoDHashes(*row[13]);
                }
                continueReading = accumulator.Consume(*digest, std::move(member));
                return continueReading;
            });
        if (!query.succeeded) return query;
        if (continueReading) accumulator.Finish();
        return {true, 0, {}};
    } catch (const std::exception& exception) {
        return {false, 0, exception.what()};
    }
}

}  // namespace videosc::dedup
