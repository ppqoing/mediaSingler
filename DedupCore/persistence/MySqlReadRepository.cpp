#include "MySqlReadRepository.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#pragma comment(lib, "bcrypt.lib")

namespace videosc::dedup {
namespace {

constexpr std::size_t kPathHashBytes = 32;

/** @brief 严格 UTF-16 到 UTF-8。 */
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
    if (length <= 0) throw std::runtime_error("Cannot encode MySQL read value");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode MySQL read value");
    }
    return result;
}

/** @brief 严格 UTF-8 到 UTF-16。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) throw std::runtime_error("Cannot decode MySQL read value");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode MySQL read value");
    }
    return result;
}

/** @brief 二进制字节转大写十六进制 SQL 字面量内容。 */
std::string HexBytes(const std::uint8_t* bytes, const std::size_t size) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string value(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        value[index * 2] = digits[bytes[index] >> 4];
        value[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    return value;
}

/** @brief 使用 Windows CNG 生成与同步写入端一致的路径 SHA-256。 */
std::array<std::uint8_t, kPathHashBytes> HashNormalizedPath(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD returned = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, kPathHashBytes> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm,
                                   BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectLength),
                                   sizeof(objectLength),
                                   &returned,
                                   0);
    }
    if (status >= 0) {
        object.resize(objectLength);
        status = BCryptCreateHash(algorithm,
                                  &hash,
                                  object.data(),
                                  static_cast<ULONG>(object.size()),
                                  nullptr,
                                  0,
                                  0);
    }
    if (status >= 0 && !value.empty()) {
        status = BCryptHashData(hash,
                                reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                                static_cast<ULONG>(value.size()),
                                0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot hash normalized path for MySQL read");
    return digest;
}

/** @brief 严格读取非 NULL 列。 */
const std::string& Required(const MySqlRow& row, const std::size_t index) {
    if (index >= row.size() || !row[index].has_value()) throw std::runtime_error("MySQL row is incomplete");
    return *row[index];
}

/** @brief 严格解析无符号十进制。 */
template <typename T>
T UnsignedValue(const std::string& value) {
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) {
        throw std::runtime_error("Invalid unsigned MySQL value");
    }
    return static_cast<T>(parsed);
}

/** @brief 严格解析有符号十进制。 */
std::int64_t SignedValue(const std::string& value) {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw std::runtime_error("Invalid signed MySQL value");
    }
    return parsed;
}

/** @brief 解析 MySQL DOUBLE 文本。 */
double DoubleValue(const std::string& value) {
    std::size_t used = 0;
    const double parsed = std::stod(value, &used);
    if (used != value.size()) throw std::runtime_error("Invalid floating MySQL value");
    return parsed;
}

/** @brief 从 HEX(video_dhashes) 恢复六帧网络字节序 dHash。 */
std::array<std::uint64_t, 6> ParseVideoDHashes(const std::string& hex) {
    if (hex.size() != 96) throw std::runtime_error("Invalid video dHash bytes");
    std::array<std::uint64_t, 6> hashes{};
    for (std::size_t hashIndex = 0; hashIndex < hashes.size(); ++hashIndex) {
        std::uint64_t value = 0;
        for (std::size_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
            const char high = hex[(hashIndex * 8 + byteIndex) * 2];
            const char low = hex[(hashIndex * 8 + byteIndex) * 2 + 1];
            const auto nibble = [](const char character) -> std::uint8_t {
                if (character >= '0' && character <= '9') return static_cast<std::uint8_t>(character - '0');
                if (character >= 'A' && character <= 'F') return static_cast<std::uint8_t>(character - 'A' + 10);
                if (character >= 'a' && character <= 'f') return static_cast<std::uint8_t>(character - 'a' + 10);
                throw std::runtime_error("Invalid video dHash hex");
            };
            value = (value << 8) | static_cast<std::uint64_t>((nibble(high) << 4) | nibble(low));
        }
        hashes[hashIndex] = value;
    }
    return hashes;
}

/** @brief 从固定长度 HEX 恢复原始字节。 */
template <std::size_t Size>
std::array<std::uint8_t, Size> ParseFixedBytes(const std::string& hex) {
    if (hex.size() != Size * 2) throw std::runtime_error("Invalid fixed binary hex length");
    const auto nibble = [](const char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') return static_cast<std::uint8_t>(character - '0');
        if (character >= 'A' && character <= 'F') return static_cast<std::uint8_t>(character - 'A' + 10);
        if (character >= 'a' && character <= 'f') return static_cast<std::uint8_t>(character - 'a' + 10);
        throw std::runtime_error("Invalid fixed binary hex");
    };
    std::array<std::uint8_t, Size> bytes{};
    for (std::size_t index = 0; index < Size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((nibble(hex[index * 2]) << 4) |
                                                 nibble(hex[index * 2 + 1]));
    }
    return bytes;
}

/** @brief 从 128 字节网络字节序恢复 16 个分区 pHash。 */
std::array<std::uint64_t, 16> ParseZonedPHashes(const std::string& hex) {
    const auto bytes = ParseFixedBytes<128>(hex);
    std::array<std::uint64_t, 16> hashes{};
    for (std::size_t hashIndex = 0; hashIndex < hashes.size(); ++hashIndex) {
        for (std::size_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
            hashes[hashIndex] = (hashes[hashIndex] << 8) | bytes[hashIndex * 8 + byteIndex];
        }
    }
    return hashes;
}

/** @brief 路径查询公共列。 */
constexpr const char* kPathColumns =
    "path_id,full_path,normalized_path,HEX(sha512),scan_id,volume_serial,file_id_high,file_id_low,"
    "volume_guid,storage_target_key,size_bytes,extension,creation_time_utc_ms,last_write_time_utc_ms,"
    "scan_root_priority,path_state";

/** @brief 内容查询公共列。 */
constexpr const char* kContentColumns =
    "HEX(sha512),content_size_bytes,media_kind,mime_type,container_name,width,height,image_dhash,"
    "HEX(image_pdq_hash),image_pdq_quality,HEX(image_zoned_phashes),image_perceptual_algorithm_version,"
    "image_structural_algorithm_version,video_duration_ms,video_frame_rate,video_bitrate,video_codec,pixel_format,HEX(video_dhashes),"
    "has_video_dhashes,static_visual,contact_sheet_path,media_algorithm_version";

/** @brief 带 d 表别名的内容列，用于和路径表连接时消除 sha512 等同名列歧义。 */
constexpr const char* kQualifiedContentColumns =
    "HEX(d.sha512),d.content_size_bytes,d.media_kind,d.mime_type,d.container_name,d.width,d.height,d.image_dhash,"
    "HEX(d.image_pdq_hash),d.image_pdq_quality,HEX(d.image_zoned_phashes),d.image_perceptual_algorithm_version,"
    "d.image_structural_algorithm_version,d.video_duration_ms,d.video_frame_rate,d.video_bitrate,d.video_codec,"
    "d.pixel_format,HEX(d.video_dhashes),d.has_video_dhashes,d.static_visual,d.contact_sheet_path,d.media_algorithm_version";

/** @brief 解析路径行并标记为远端已同步状态。 */
FilePathRecord DecodePath(const MySqlRow& row) {
    if (row.size() != 16) throw std::runtime_error("Unexpected MySQL path column count");
    FilePathRecord path;
    path.path_id = UnsignedValue<std::uint64_t>(Required(row, 0));
    path.path = Utf8ToWide(Required(row, 1));
    path.normalized_path_key = Utf8ToWide(Required(row, 2));
    path.sha512 = Sha512FromHex(Required(row, 3));
    if (!path.sha512.has_value()) throw std::runtime_error("Invalid MySQL path SHA-512");
    path.scan_id = UnsignedValue<std::uint64_t>(Required(row, 4));
    path.identity.volume_serial = UnsignedValue<std::uint64_t>(Required(row, 5));
    path.identity.file_id_high = UnsignedValue<std::uint64_t>(Required(row, 6));
    path.identity.file_id_low = UnsignedValue<std::uint64_t>(Required(row, 7));
    path.volume_guid = Utf8ToWide(Required(row, 8));
    path.storage_target_key = Utf8ToWide(Required(row, 9));
    path.size_bytes = UnsignedValue<std::uint64_t>(Required(row, 10));
    path.extension = Utf8ToWide(Required(row, 11));
    path.creation_time_utc_ms = SignedValue(Required(row, 12));
    path.last_write_time_utc_ms = SignedValue(Required(row, 13));
    path.scan_root_priority = UnsignedValue<std::uint32_t>(Required(row, 14));
    const int state = UnsignedValue<int>(Required(row, 15));
    if (state < static_cast<int>(FilePathState::Pending) || state > static_cast<int>(FilePathState::ReadTimeout)) {
        throw std::runtime_error("Invalid MySQL path state");
    }
    path.state = static_cast<FilePathState>(state);
    path.sync_state = SyncState::Synchronized;
    return path;
}

/** @brief 解析媒体内容行。 */
ShaFileData DecodeContent(const MySqlRow& row) {
    if (row.size() != 23) throw std::runtime_error("Unexpected MySQL content column count");
    ShaFileData data;
    const auto sha = Sha512FromHex(Required(row, 0));
    if (!sha.has_value()) throw std::runtime_error("Invalid MySQL content SHA-512");
    data.sha512 = *sha;
    data.content_size_bytes = UnsignedValue<std::uint64_t>(Required(row, 1));
    const int mediaKind = UnsignedValue<int>(Required(row, 2));
    if (mediaKind < static_cast<int>(MediaKind::Other) || mediaKind > static_cast<int>(MediaKind::Audio)) {
        throw std::runtime_error("Invalid MySQL media kind");
    }
    data.media_kind = static_cast<MediaKind>(mediaKind);
    data.mime_type = Required(row, 3);
    data.container_name = Required(row, 4);
    data.width = UnsignedValue<std::uint32_t>(Required(row, 5));
    data.height = UnsignedValue<std::uint32_t>(Required(row, 6));
    if (row[7].has_value()) data.image_dhash = UnsignedValue<std::uint64_t>(*row[7]);
    if (row[8].has_value()) data.image_pdq_hash = ParseFixedBytes<32>(*row[8]);
    if (row[9].has_value()) data.image_pdq_quality = UnsignedValue<std::uint8_t>(*row[9]);
    if (row[10].has_value()) {
        data.image_zoned_phashes = ParseZonedPHashes(*row[10]);
        data.has_image_zoned_phashes = true;
    }
    data.image_perceptual_algorithm_version = UnsignedValue<std::uint32_t>(Required(row, 11));
    data.image_structural_algorithm_version = UnsignedValue<std::uint32_t>(Required(row, 12));
    data.video_duration_ms = SignedValue(Required(row, 13));
    data.video_frame_rate = DoubleValue(Required(row, 14));
    data.video_bitrate = UnsignedValue<std::uint64_t>(Required(row, 15));
    data.video_codec = Required(row, 16);
    data.pixel_format = Required(row, 17);
    data.has_video_dhashes = UnsignedValue<unsigned int>(Required(row, 19)) != 0;
    if (data.has_video_dhashes) {
        if (!row[18].has_value()) throw std::runtime_error("MySQL video dHash flag has no bytes");
        data.video_dhashes = ParseVideoDHashes(*row[18]);
    }
    data.static_visual = UnsignedValue<unsigned int>(Required(row, 20)) != 0;
    data.contact_sheet_path = Utf8ToWide(Required(row, 21));
    data.media_algorithm_version = Required(row, 22);
    return data;
}

/** @brief 把摘要集合构造成有界 IN 列表。 */
template <typename Iterator, typename Encoder>
std::string HexInList(Iterator begin, Iterator end, Encoder encoder) {
    std::string result;
    for (auto iterator = begin; iterator != end; ++iterator) {
        if (!result.empty()) result += ',';
        result += "X'" + encoder(*iterator) + "'";
    }
    return result;
}

}  // namespace

MySqlReadRepository::MySqlReadRepository(MySqlClient& client) : client_(client) {}

MySqlStatus MySqlReadRepository::LoadPaths(
    const std::vector<std::wstring>& normalized_paths,
    std::unordered_map<std::wstring, FilePathRecord>& records) {
    records.clear();
    if (normalized_paths.empty()) return {true, 0, {}};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    try {
        std::vector<std::array<std::uint8_t, kPathHashBytes>> hashes;
        hashes.reserve(normalized_paths.size());
        std::unordered_set<std::wstring> requested;
        requested.reserve(normalized_paths.size());
        for (const std::wstring& path : normalized_paths) {
            hashes.push_back(HashNormalizedPath(WideToUtf8(path)));
            requested.insert(path);
        }
        const std::string inList = HexInList(
            hashes.begin(), hashes.end(), [](const auto& digest) { return HexBytes(digest.data(), digest.size()); });
        std::string parseError;
        const MySqlStatus status = client_.Query(
            std::string("SELECT ") + kPathColumns +
                " FROM file_path_sha512 WHERE path_hash IN (" + inList + ")",
            [&](const MySqlRow& row) {
                try {
                    FilePathRecord path = DecodePath(row);
                    if (requested.find(path.normalized_path_key) != requested.end()) {
                        records[path.normalized_path_key] = std::move(path);
                    }
                    return true;
                } catch (const std::exception& exception) {
                    parseError = exception.what();
                    return false;
                }
            });
        if (!status.succeeded) return status;
        return parseError.empty() ? MySqlStatus{true, 0, {}}
                                  : MySqlStatus{false, 0, parseError};
    } catch (const std::exception& exception) {
        return {false, 0, exception.what()};
    }
}

MySqlStatus MySqlReadRepository::LoadContents(
    const std::vector<Sha512Digest>& digests,
    std::unordered_map<std::string, ShaFileData>& records) {
    records.clear();
    if (digests.empty()) return {true, 0, {}};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    try {
        const std::string inList = HexInList(
            digests.begin(), digests.end(), [](const Sha512Digest& digest) { return Sha512ToHex(digest); });
        std::string parseError;
        const MySqlStatus status = client_.Query(
            std::string("SELECT ") + kContentColumns +
                " FROM sha512_file_data WHERE sha512 IN (" + inList + ")",
            [&](const MySqlRow& row) {
                try {
                    ShaFileData data = DecodeContent(row);
                    records[Sha512ToHex(data.sha512)] = std::move(data);
                    return true;
                } catch (const std::exception& exception) {
                    parseError = exception.what();
                    return false;
                }
            });
        if (!status.succeeded) return status;
        return parseError.empty() ? MySqlStatus{true, 0, {}}
                                  : MySqlStatus{false, 0, parseError};
    } catch (const std::exception& exception) {
        return {false, 0, exception.what()};
    }
}

MySqlStatus MySqlReadRepository::StreamVisualContents(
    const std::string& algorithm_version,
    const ContentVisitor& visitor) {
    if (!visitor) return {false, 0, "content_visitor_required"};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    (void)algorithm_version;
    std::string parseError;
    const MySqlStatus status = client_.Query(
        std::string("SELECT ") + kQualifiedContentColumns +
            " FROM sha512_file_data d JOIN (SELECT DISTINCT sha512 FROM file_path_sha512 WHERE active=1) p "
            "ON p.sha512=d.sha512 WHERE d.media_kind IN (1,2) ORDER BY d.sha512",
        [&](const MySqlRow& row) {
            try {
                return visitor(DecodeContent(row));
            } catch (const std::exception& exception) {
                parseError = exception.what();
                return false;
            }
        });
    if (!status.succeeded) return status;
    return parseError.empty() ? MySqlStatus{true, 0, {}}
                              : MySqlStatus{false, 0, parseError};
}

MySqlStatus MySqlReadRepository::CountImageFeatureCompleteness(
    const std::string& algorithm_version,
    ImageFeatureCompletenessSnapshot& snapshot) {
    snapshot = {};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    std::string versionLiteral;
    const MySqlStatus escaped = client_.EscapeLiteral(algorithm_version, versionLiteral);
    if (!escaped.succeeded) return escaped;
    std::string parseError;
    const std::string completeCondition =
        "d.media_algorithm_version=" + versionLiteral +
        " AND d.image_pdq_hash IS NOT NULL AND d.image_pdq_quality IS NOT NULL "
        "AND d.image_zoned_phashes IS NOT NULL AND d.image_perceptual_algorithm_version=1 "
        "AND d.image_structural_algorithm_version=1";
    const MySqlStatus status = client_.Query(
        "SELECT COUNT(*),SUM(CASE WHEN " + completeCondition +
            " THEN 1 ELSE 0 END) FROM sha512_file_data d "
            "JOIN (SELECT DISTINCT sha512 FROM file_path_sha512 WHERE active=1) p ON p.sha512=d.sha512 "
            "WHERE d.media_kind=1",
        [&](const MySqlRow& row) {
            try {
                if (row.size() != 2) throw std::runtime_error("Unexpected completeness column count");
                snapshot.total_images = UnsignedValue<std::uint64_t>(Required(row, 0));
                snapshot.complete_images = row[1].has_value()
                                               ? UnsignedValue<std::uint64_t>(*row[1])
                                               : 0;
                snapshot.incomplete_images = snapshot.total_images - snapshot.complete_images;
                return true;
            } catch (const std::exception& exception) {
                parseError = exception.what();
                return false;
            }
        });
    if (!status.succeeded) return status;
    return parseError.empty() ? MySqlStatus{true, 0, {}}
                              : MySqlStatus{false, 0, parseError};
}

MySqlStatus MySqlReadRepository::StreamIncompleteImageFeatureTargets(
    const std::string& algorithm_version,
    const ImageBackfillVisitor& visitor) {
    if (!visitor) return {false, 0, "image_backfill_visitor_required"};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    std::string versionLiteral;
    const MySqlStatus escaped = client_.EscapeLiteral(algorithm_version, versionLiteral);
    if (!escaped.succeeded) return escaped;
    std::string parseError;
    std::optional<ImageFeatureBackfillTarget> current;
    bool visitorStopped = false;
    const auto flushCurrent = [&]() {
        if (!current.has_value()) return true;
        ImageFeatureBackfillTarget target = std::move(*current);
        current.reset();
        if (!visitor(std::move(target))) {
            visitorStopped = true;
            return false;
        }
        return true;
    };
    const std::string incompleteCondition =
        "(d.media_algorithm_version<>" + versionLiteral +
        " OR d.image_pdq_hash IS NULL OR d.image_pdq_quality IS NULL "
        "OR d.image_zoned_phashes IS NULL OR d.image_perceptual_algorithm_version<>1 "
        "OR d.image_structural_algorithm_version<>1)";
    const MySqlStatus status = client_.Query(
        std::string("SELECT ") + kQualifiedContentColumns +
            ",p.path_id,p.full_path,p.normalized_path,p.storage_target_key,p.size_bytes,"
            "p.scan_root_priority,p.path_state FROM sha512_file_data d "
            "JOIN file_path_sha512 p ON p.sha512=d.sha512 AND p.active=1 "
            "WHERE d.media_kind=1 AND " + incompleteCondition +
            " ORDER BY d.sha512,p.scan_root_priority,p.path_id",
        [&](const MySqlRow& row) {
            try {
                if (row.size() != 30) throw std::runtime_error("Unexpected image backfill column count");
                MySqlRow contentRow(row.begin(), row.begin() + 23);
                ShaFileData content = DecodeContent(contentRow);
                if (!current.has_value() || current->content.sha512 != content.sha512) {
                    if (!flushCurrent()) return false;
                    current = ImageFeatureBackfillTarget{};
                    current->content = std::move(content);
                }
                FilePathRecord path;
                path.path_id = UnsignedValue<std::uint64_t>(Required(row, 23));
                path.path = Utf8ToWide(Required(row, 24));
                path.normalized_path_key = Utf8ToWide(Required(row, 25));
                path.sha512 = current->content.sha512;
                path.storage_target_key = Utf8ToWide(Required(row, 26));
                path.size_bytes = UnsignedValue<std::uint64_t>(Required(row, 27));
                path.scan_root_priority = UnsignedValue<std::uint32_t>(Required(row, 28));
                const int state = UnsignedValue<int>(Required(row, 29));
                if (state < static_cast<int>(FilePathState::Pending) ||
                    state > static_cast<int>(FilePathState::ReadTimeout)) {
                    throw std::runtime_error("Invalid image backfill path state");
                }
                path.state = static_cast<FilePathState>(state);
                path.sync_state = SyncState::Synchronized;
                current->active_paths.push_back(std::move(path));
                return true;
            } catch (const std::exception& exception) {
                parseError = exception.what();
                return false;
            }
        });
    if (!status.succeeded) return status;
    if (parseError.empty() && !visitorStopped) flushCurrent();
    return parseError.empty() ? MySqlStatus{true, 0, {}}
                              : MySqlStatus{false, 0, parseError};
}

MySqlStatus MySqlReadRepository::StreamActivePaths(const PathVisitor& visitor) {
    if (!visitor) return {false, 0, "path_visitor_required"};
    if (!client_.is_connected()) {
        const MySqlStatus connected = client_.Connect();
        if (!connected.succeeded) return connected;
    }
    std::string parseError;
    const MySqlStatus status = client_.Query(
        std::string("SELECT ") + kPathColumns +
            " FROM file_path_sha512 WHERE active=1 ORDER BY path_id",
        [&](const MySqlRow& row) {
            try {
                return visitor(DecodePath(row));
            } catch (const std::exception& exception) {
                parseError = exception.what();
                return false;
            }
        });
    if (!status.succeeded) return status;
    return parseError.empty() ? MySqlStatus{true, 0, {}}
                              : MySqlStatus{false, 0, parseError};
}

}  // namespace videosc::dedup
