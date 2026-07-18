#include "SyncOperation.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace videosc::dedup {
namespace {

using Json = nlohmann::json;
constexpr std::string_view kQueuePrefix = "sync/";
constexpr std::string_view kSequenceKey = "metadata/sync_next_sequence";
constexpr std::string_view kStagePrefix = "sync_stage/";
constexpr std::string_view kStagedCountPrefix = "metadata/sync_staged_count/";
constexpr std::string_view kPendingCountPrefix = "metadata/sync_pending_count/";

bool ParseSequence(const std::string& value, std::uint64_t& sequence);

/** @brief 把扫描 ID 编码为可排序的固定宽度十进制。 */
std::string PaddedId(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::setw(20) << std::setfill('0') << value;
    return stream.str();
}

/** @brief 构造扫描级计数器键。 */
std::string CountKey(const std::string_view prefix, const std::uint64_t scan_id) {
    return std::string(prefix) + PaddedId(scan_id);
}

/** @brief 读取不存在时为零的扫描级计数器。 */
RocksStatus ReadCount(const RocksStore& store,
                      const std::string& key,
                      std::uint64_t& count) {
    std::string value;
    const RocksStatus status = store.Get(RocksColumnFamily::Default, key, value);
    if (!status.succeeded) {
        if (status.message == "not_found") {
            count = 0;
            return {true, {}};
        }
        return status;
    }
    if (!ParseSequence(value, count)) return {false, "invalid_sync_counter"};
    return {true, {}};
}

/** @brief 严格 UTF-16 到 UTF-8 转换。 */
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
    if (length <= 0) throw std::runtime_error("Cannot encode sync path as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode sync path as UTF-8");
    }
    return result;
}

/** @brief 严格 UTF-8 到 UTF-16 转换。 */
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) throw std::runtime_error("Cannot decode sync path from UTF-8");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode sync path from UTF-8");
    }
    return result;
}

/** @brief 把固定二进制特征编码为大写十六进制。 */
std::string HexBytes(const std::uint8_t* bytes, const std::size_t size) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

/** @brief 严格解析固定长度十六进制特征。 */
template <std::size_t Size>
std::array<std::uint8_t, Size> ParseHexBytes(const std::string& text) {
    if (text.size() != Size * 2) throw std::runtime_error("Invalid image perceptual hex length");
    const auto nibble = [](const char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::runtime_error("Invalid image perceptual hex digit");
    };
    std::array<std::uint8_t, Size> bytes{};
    for (std::size_t index = 0; index < Size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((nibble(text[index * 2]) << 4) |
                                                 nibble(text[index * 2 + 1]));
    }
    return bytes;
}

/** @brief 将路径记录编码为稳定 JSON 对象。 */
Json EncodeFilePath(const FilePathRecord& record) {
    Json value = {
        {"path_id", record.path_id},
        {"scan_id", record.scan_id},
        {"path", WideToUtf8(record.path.wstring())},
        {"normalized_path", WideToUtf8(record.normalized_path_key)},
        {"volume_serial", record.identity.volume_serial},
        {"file_id_high", record.identity.file_id_high},
        {"file_id_low", record.identity.file_id_low},
        {"volume_guid", WideToUtf8(record.volume_guid)},
        {"storage_target_key", WideToUtf8(record.storage_target_key)},
        {"size_bytes", record.size_bytes},
        {"extension", WideToUtf8(record.extension)},
        {"creation_time_utc_ms", record.creation_time_utc_ms},
        {"last_write_time_utc_ms", record.last_write_time_utc_ms},
        {"scan_root_priority", record.scan_root_priority},
        {"state", static_cast<int>(record.state)},
    };
    value["sha512"] = record.sha512.has_value() ? Json(Sha512ToHex(*record.sha512)) : Json(nullptr);
    return value;
}

/** @brief 从稳定 JSON 对象恢复路径记录。 */
FilePathRecord DecodeFilePath(const Json& value) {
    FilePathRecord record;
    record.path_id = value.at("path_id").get<std::uint64_t>();
    record.scan_id = value.at("scan_id").get<std::uint64_t>();
    record.path = Utf8ToWide(value.at("path").get<std::string>());
    record.normalized_path_key = Utf8ToWide(value.at("normalized_path").get<std::string>());
    record.identity.volume_serial = value.at("volume_serial").get<std::uint64_t>();
    record.identity.file_id_high = value.at("file_id_high").get<std::uint64_t>();
    record.identity.file_id_low = value.at("file_id_low").get<std::uint64_t>();
    record.volume_guid = Utf8ToWide(value.at("volume_guid").get<std::string>());
    record.storage_target_key = Utf8ToWide(value.at("storage_target_key").get<std::string>());
    record.size_bytes = value.at("size_bytes").get<std::uint64_t>();
    record.extension = Utf8ToWide(value.at("extension").get<std::string>());
    record.creation_time_utc_ms = value.at("creation_time_utc_ms").get<std::int64_t>();
    record.last_write_time_utc_ms = value.at("last_write_time_utc_ms").get<std::int64_t>();
    record.scan_root_priority = value.at("scan_root_priority").get<std::uint32_t>();
    const int state = value.at("state").get<int>();
    if (state < static_cast<int>(FilePathState::Pending) || state > static_cast<int>(FilePathState::ReadTimeout)) {
        throw std::runtime_error("Invalid file path state in sync message");
    }
    record.state = static_cast<FilePathState>(state);
    record.sync_state = SyncState::Pending;
    if (!value.at("sha512").is_null()) {
        record.sha512 = Sha512FromHex(value.at("sha512").get<std::string>());
        if (!record.sha512.has_value()) throw std::runtime_error("Invalid path SHA-512 in sync message");
    }
    return record;
}

/** @brief 将内容记录编码为稳定 JSON 对象。 */
Json EncodeShaData(const ShaFileData& data) {
    Json value = {
        {"sha512", Sha512ToHex(data.sha512)},
        {"content_size_bytes", data.content_size_bytes},
        {"media_kind", static_cast<int>(data.media_kind)},
        {"mime_type", data.mime_type},
        {"container_name", data.container_name},
        {"width", data.width},
        {"height", data.height},
        {"video_duration_ms", data.video_duration_ms},
        {"video_frame_rate", data.video_frame_rate},
        {"video_bitrate", data.video_bitrate},
        {"video_codec", data.video_codec},
        {"pixel_format", data.pixel_format},
        {"video_dhashes", data.video_dhashes},
        {"has_video_dhashes", data.has_video_dhashes},
        {"static_visual", data.static_visual},
        {"contact_sheet_path", WideToUtf8(data.contact_sheet_path.wstring())},
        {"media_algorithm_version", data.media_algorithm_version},
    };
    value["image_dhash"] = data.image_dhash.has_value() ? Json(*data.image_dhash) : Json(nullptr);
    value["image_pdq_hash"] = data.image_pdq_hash.has_value()
                                  ? Json(HexBytes(data.image_pdq_hash->data(), data.image_pdq_hash->size()))
                                  : Json(nullptr);
    value["image_pdq_quality"] = data.image_pdq_quality.has_value()
                                     ? Json(*data.image_pdq_quality)
                                     : Json(nullptr);
    value["image_zoned_phashes"] = data.image_zoned_phashes;
    value["has_image_zoned_phashes"] = data.has_image_zoned_phashes;
    value["image_perceptual_algorithm_version"] = data.image_perceptual_algorithm_version;
    value["image_structural_algorithm_version"] = data.image_structural_algorithm_version;
    return value;
}

/** @brief 从稳定 JSON 对象恢复内容记录。 */
ShaFileData DecodeShaData(const Json& value) {
    ShaFileData data;
    const auto sha = Sha512FromHex(value.at("sha512").get<std::string>());
    if (!sha.has_value()) throw std::runtime_error("Invalid content SHA-512 in sync message");
    data.sha512 = *sha;
    data.content_size_bytes = value.at("content_size_bytes").get<std::uint64_t>();
    const int mediaKind = value.at("media_kind").get<int>();
    if (mediaKind < static_cast<int>(MediaKind::Other) || mediaKind > static_cast<int>(MediaKind::Audio)) {
        throw std::runtime_error("Invalid media kind in sync message");
    }
    data.media_kind = static_cast<MediaKind>(mediaKind);
    data.mime_type = value.at("mime_type").get<std::string>();
    data.container_name = value.at("container_name").get<std::string>();
    data.width = value.at("width").get<std::uint32_t>();
    data.height = value.at("height").get<std::uint32_t>();
    if (!value.at("image_dhash").is_null()) data.image_dhash = value.at("image_dhash").get<std::uint64_t>();
    data.video_duration_ms = value.at("video_duration_ms").get<std::int64_t>();
    data.video_frame_rate = value.at("video_frame_rate").get<double>();
    data.video_bitrate = value.at("video_bitrate").get<std::uint64_t>();
    data.video_codec = value.at("video_codec").get<std::string>();
    data.pixel_format = value.at("pixel_format").get<std::string>();
    data.video_dhashes = value.at("video_dhashes").get<std::array<std::uint64_t, 6>>();
    data.has_video_dhashes = value.at("has_video_dhashes").get<bool>();
    data.static_visual = value.at("static_visual").get<bool>();
    data.contact_sheet_path = Utf8ToWide(value.at("contact_sheet_path").get<std::string>());
    data.media_algorithm_version = value.at("media_algorithm_version").get<std::string>();
    if (value.contains("image_pdq_hash") && !value.at("image_pdq_hash").is_null()) {
        data.image_pdq_hash = ParseHexBytes<32>(value.at("image_pdq_hash").get<std::string>());
    }
    if (value.contains("image_pdq_quality") && !value.at("image_pdq_quality").is_null()) {
        data.image_pdq_quality = value.at("image_pdq_quality").get<std::uint8_t>();
    }
    if (value.contains("image_zoned_phashes")) {
        data.image_zoned_phashes = value.at("image_zoned_phashes").get<std::array<std::uint64_t, 16>>();
    }
    data.has_image_zoned_phashes = value.value("has_image_zoned_phashes", false);
    data.image_perceptual_algorithm_version =
        value.value("image_perceptual_algorithm_version", 0U);
    data.image_structural_algorithm_version =
        value.value("image_structural_algorithm_version", 0U);
    return data;
}

/** @brief 严格解析无符号十进制 sequence。 */
bool ParseSequence(const std::string& value, std::uint64_t& sequence) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), sequence);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

std::string SyncOperationCodec::Serialize(const SyncOperation& operation) {
    Json root = {
        {"version", 3},
        {"sequence", operation.sequence},
        {"batch_scan_id", operation.batch_scan_id},
        {"kind", static_cast<int>(operation.kind)},
        {"delete_path_id", operation.delete_path_id},
        {"attempt_count", operation.attempt_count},
        {"next_attempt_utc_ms", operation.next_attempt_utc_ms},
        {"last_native_error", operation.last_native_error},
    };
    root["file_path"] = operation.file_path.has_value() ? EncodeFilePath(*operation.file_path) : Json(nullptr);
    root["sha_file_data"] = operation.sha_file_data.has_value() ? EncodeShaData(*operation.sha_file_data) : Json(nullptr);
    root["expected_sha512"] = operation.expected_sha512.has_value()
                                   ? Json(Sha512ToHex(*operation.expected_sha512))
                                   : Json(nullptr);
    return root.dump();
}

std::optional<SyncOperation> SyncOperationCodec::Deserialize(const std::string& value, std::string& error) {
    try {
        const Json root = Json::parse(value);
        const int version = root.at("version").get<int>();
        if (version != 1 && version != 2 && version != 3) {
            throw std::runtime_error("Unsupported sync message version");
        }
        SyncOperation operation;
        operation.sequence = root.at("sequence").get<std::uint64_t>();
        operation.batch_scan_id = version >= 2 ? root.value("batch_scan_id", 0ULL) : 0ULL;
        const int kind = root.at("kind").get<int>();
        if (kind < static_cast<int>(SyncOperationKind::UpsertShaFileData) ||
            kind > static_cast<int>(SyncOperationKind::DeleteFilePath)) {
            throw std::runtime_error("Invalid sync operation kind");
        }
        operation.kind = static_cast<SyncOperationKind>(kind);
        operation.delete_path_id = root.at("delete_path_id").get<std::uint64_t>();
        operation.attempt_count = root.at("attempt_count").get<std::uint32_t>();
        operation.next_attempt_utc_ms = root.at("next_attempt_utc_ms").get<std::int64_t>();
        operation.last_native_error = root.at("last_native_error").get<unsigned int>();
        if (!root.at("file_path").is_null()) operation.file_path = DecodeFilePath(root.at("file_path"));
        if (!root.at("sha_file_data").is_null()) operation.sha_file_data = DecodeShaData(root.at("sha_file_data"));
        if (!root.at("expected_sha512").is_null()) {
            operation.expected_sha512 = Sha512FromHex(root.at("expected_sha512").get<std::string>());
            if (!operation.expected_sha512.has_value()) throw std::runtime_error("Invalid expected SHA-512");
        }
        error.clear();
        return operation;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

MySqlSyncQueue::MySqlSyncQueue(RocksStore& store) : store_(store) {}

std::string MySqlSyncQueue::QueueKey(const std::uint64_t sequence) {
    std::ostringstream stream;
    stream << kQueuePrefix << std::setw(20) << std::setfill('0') << sequence;
    return stream.str();
}

std::string MySqlSyncQueue::StagePrefix(const std::uint64_t scan_id) {
    return std::string(kStagePrefix) + PaddedId(scan_id) + "/";
}

std::string MySqlSyncQueue::StageKey(const std::uint64_t scan_id,
                                     const SyncOperation& operation) {
    std::string key = StagePrefix(scan_id) + std::to_string(static_cast<int>(operation.kind)) + "/";
    if (operation.kind == SyncOperationKind::UpsertShaFileData && operation.sha_file_data.has_value()) {
        return key + Sha512ToHex(operation.sha_file_data->sha512);
    }
    if (operation.kind == SyncOperationKind::UpsertFilePath && operation.file_path.has_value()) {
        return key + PaddedId(operation.file_path->path_id);
    }
    if (operation.kind == SyncOperationKind::DeleteFilePath && operation.delete_path_id != 0) {
        return key + PaddedId(operation.delete_path_id);
    }
    throw std::invalid_argument("incomplete_staged_sync_operation");
}

RocksStatus MySqlSyncQueue::NextSequence(std::uint64_t& sequence, std::vector<RocksMutation>& mutations) {
    std::string current;
    const RocksStatus read = store_.Get(RocksColumnFamily::Default, kSequenceKey, current);
    if (read.succeeded) {
        if (!ParseSequence(current, sequence) || sequence == std::numeric_limits<std::uint64_t>::max()) {
            return {false, "invalid_sync_sequence"};
        }
    } else if (read.message == "not_found") {
        sequence = 1;
    } else {
        return read;
    }
    mutations.push_back({RocksColumnFamily::Default, std::string(kSequenceKey), std::to_string(sequence + 1)});
    return {true, {}};
}

RocksStatus MySqlSyncQueue::Enqueue(const std::vector<RocksMutation>& local_mutations,
                                    SyncOperation& operation) {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    std::vector<RocksMutation> mutations = local_mutations;
    if (operation.sequence == 0) {
        const RocksStatus next = NextSequence(operation.sequence, mutations);
        if (!next.succeeded) return next;
    }
    mutations.push_back({RocksColumnFamily::SyncQueue, QueueKey(operation.sequence),
                         SyncOperationCodec::Serialize(operation)});
    return store_.WriteBatch(mutations, true);
}

RocksStatus MySqlSyncQueue::Stage(const std::uint64_t scan_id,
                                  const std::vector<RocksMutation>& local_mutations,
                                  SyncOperation operation,
                                  bool& inserted_new_key) {
    if (scan_id == 0) return {false, "scan_id_required"};
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    try {
        operation.sequence = 0;
        operation.batch_scan_id = scan_id;
        const std::string stageKey = StageKey(scan_id, operation);
        std::string existing;
        const RocksStatus read = store_.Get(RocksColumnFamily::Checkpoints, stageKey, existing);
        if (!read.succeeded && read.message != "not_found") return read;
        inserted_new_key = !read.succeeded;

        std::uint64_t stagedCount = 0;
        const std::string countKey = CountKey(kStagedCountPrefix, scan_id);
        const RocksStatus countStatus = ReadCount(store_, countKey, stagedCount);
        if (!countStatus.succeeded) return countStatus;
        if (inserted_new_key) {
            if (stagedCount == std::numeric_limits<std::uint64_t>::max()) {
                return {false, "staged_sync_count_overflow"};
            }
            ++stagedCount;
        }

        std::vector<RocksMutation> mutations = local_mutations;
        mutations.push_back({RocksColumnFamily::Checkpoints,
                             stageKey,
                             SyncOperationCodec::Serialize(operation)});
        mutations.push_back({RocksColumnFamily::Default, countKey, std::to_string(stagedCount)});
        return store_.WriteBatch(mutations, true);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

RocksStatus MySqlSyncQueue::PublishStaged(const std::uint64_t scan_id,
                                          const std::size_t maximum_items,
                                          std::size_t& published_items) {
    published_items = 0;
    if (scan_id == 0 || maximum_items == 0) return {false, "publish_staged_arguments_invalid"};
    std::lock_guard<std::mutex> lock(sequence_mutex_);

    std::vector<std::pair<std::string, SyncOperation>> staged;
    std::string decodeError;
    const RocksStatus iterated = store_.ForEachPrefix(
        RocksColumnFamily::Checkpoints,
        StagePrefix(scan_id),
        maximum_items,
        [&](const std::string_view key, const std::string_view value) {
            std::optional<SyncOperation> operation =
                SyncOperationCodec::Deserialize(std::string(value), decodeError);
            if (!operation.has_value()) return false;
            staged.emplace_back(std::string(key), std::move(*operation));
            return true;
        });
    if (!iterated.succeeded) return iterated;
    if (!decodeError.empty()) return {false, "invalid_staged_sync_message: " + decodeError};
    if (staged.empty()) return {true, {}};

    std::vector<RocksMutation> mutations;
    std::uint64_t firstSequence = 0;
    const RocksStatus sequenceStatus = NextSequence(firstSequence, mutations);
    if (!sequenceStatus.succeeded) return sequenceStatus;
    if (staged.size() - 1 > std::numeric_limits<std::uint64_t>::max() - firstSequence) {
        return {false, "sync_sequence_overflow"};
    }
    mutations.back().value = std::to_string(firstSequence + staged.size());

    std::uint64_t stagedCount = 0;
    std::uint64_t pendingCount = 0;
    const std::string stagedCountKey = CountKey(kStagedCountPrefix, scan_id);
    const std::string pendingCountKey = CountKey(kPendingCountPrefix, scan_id);
    RocksStatus countStatus = ReadCount(store_, stagedCountKey, stagedCount);
    if (!countStatus.succeeded) return countStatus;
    countStatus = ReadCount(store_, pendingCountKey, pendingCount);
    if (!countStatus.succeeded) return countStatus;
    if (stagedCount < staged.size() ||
        pendingCount > std::numeric_limits<std::uint64_t>::max() - staged.size()) {
        return {false, "invalid_staged_sync_counter"};
    }

    for (std::size_t index = 0; index < staged.size(); ++index) {
        SyncOperation& operation = staged[index].second;
        operation.sequence = firstSequence + index;
        operation.batch_scan_id = scan_id;
        mutations.push_back({RocksColumnFamily::SyncQueue,
                             QueueKey(operation.sequence),
                             SyncOperationCodec::Serialize(operation)});
        mutations.push_back({RocksColumnFamily::Checkpoints, staged[index].first, std::nullopt});
    }
    mutations.push_back({RocksColumnFamily::Default,
                         stagedCountKey,
                         std::to_string(stagedCount - staged.size())});
    mutations.push_back({RocksColumnFamily::Default,
                         pendingCountKey,
                         std::to_string(pendingCount + staged.size())});
    const RocksStatus written = store_.WriteBatch(mutations, true);
    if (written.succeeded) published_items = staged.size();
    return written;
}

RocksStatus MySqlSyncQueue::PublishAllStaged(const std::size_t maximum_items_per_batch,
                                             std::size_t& published_items) {
    published_items = 0;
    if (maximum_items_per_batch == 0) return {false, "publish_staged_batch_size_invalid"};

    std::set<std::uint64_t> scanIds;
    std::string parseError;
    const RocksStatus listed = store_.ForEachPrefix(
        RocksColumnFamily::Checkpoints,
        kStagePrefix,
        0,
        [&](const std::string_view key, const std::string_view) {
            const std::size_t idOffset = kStagePrefix.size();
            const std::size_t separator = key.find('/', idOffset);
            if (separator == std::string_view::npos || separator == idOffset) {
                parseError = "invalid_staged_sync_key";
                return false;
            }
            std::uint64_t scanId = 0;
            const std::string_view id = key.substr(idOffset, separator - idOffset);
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), scanId);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || scanId == 0) {
                parseError = "invalid_staged_sync_scan_id";
                return false;
            }
            scanIds.insert(scanId);
            return true;
        });
    if (!listed.succeeded) return listed;
    if (!parseError.empty()) return {false, parseError};

    for (const std::uint64_t scanId : scanIds) {
        while (true) {
            std::size_t publishedBatch = 0;
            const RocksStatus published = PublishStaged(scanId, maximum_items_per_batch, publishedBatch);
            if (!published.succeeded) return published;
            if (publishedBatch > (std::numeric_limits<std::size_t>::max)() - published_items) {
                return {false, "published_staged_count_overflow"};
            }
            published_items += publishedBatch;
            if (publishedBatch < maximum_items_per_batch) break;
        }
    }
    return {true, {}};
}

RocksStatus MySqlSyncQueue::StagedCount(const std::uint64_t scan_id,
                                        std::uint64_t& count) const {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    return ReadCount(store_, CountKey(kStagedCountPrefix, scan_id), count);
}

RocksStatus MySqlSyncQueue::PendingCount(const std::uint64_t scan_id,
                                         std::uint64_t& count) const {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    return ReadCount(store_, CountKey(kPendingCountPrefix, scan_id), count);
}

RocksStatus MySqlSyncQueue::ReadBatch(const std::size_t maximum_items,
                                      std::vector<SyncOperation>& operations) const {
    operations.clear();
    std::string decodeError;
    RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::SyncQueue,
        kQueuePrefix,
        maximum_items,
        [&](const std::string_view, const std::string_view value) {
            std::optional<SyncOperation> operation =
                SyncOperationCodec::Deserialize(std::string(value), decodeError);
            if (!operation.has_value()) return false;
            operations.push_back(std::move(*operation));
            return true;
        });
    if (status.succeeded && !decodeError.empty()) return {false, "invalid_sync_message: " + decodeError};
    return status;
}

RocksStatus MySqlSyncQueue::Acknowledge(const std::vector<std::uint64_t>& sequences) {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    std::vector<RocksMutation> mutations;
    std::map<std::uint64_t, std::uint64_t> acknowledgedByScan;
    mutations.reserve(sequences.size() * 2);
    for (const std::uint64_t sequence : sequences) {
        std::string value;
        const RocksStatus read = store_.Get(RocksColumnFamily::SyncQueue, QueueKey(sequence), value);
        if (!read.succeeded) {
            if (read.message == "not_found") continue;
            return read;
        }
        std::string decodeError;
        std::optional<SyncOperation> operation = SyncOperationCodec::Deserialize(value, decodeError);
        if (!operation.has_value()) return {false, "invalid_sync_message: " + decodeError};
        if (operation->batch_scan_id != 0) ++acknowledgedByScan[operation->batch_scan_id];
        mutations.push_back({RocksColumnFamily::SyncQueue, QueueKey(sequence), std::nullopt});
    }
    for (const auto& [scanId, acknowledged] : acknowledgedByScan) {
        const std::string key = CountKey(kPendingCountPrefix, scanId);
        std::uint64_t pending = 0;
        const RocksStatus countStatus = ReadCount(store_, key, pending);
        if (!countStatus.succeeded) return countStatus;
        if (pending < acknowledged) return {false, "invalid_pending_sync_counter"};
        mutations.push_back({RocksColumnFamily::Default, key, std::to_string(pending - acknowledged)});
    }
    if (mutations.empty()) return {true, {}};
    return store_.WriteBatch(mutations, true);
}

RocksStatus MySqlSyncQueue::SaveRetry(const SyncOperation& operation) {
    if (operation.sequence == 0) return {false, "sync_sequence_required"};
    return store_.Put(RocksColumnFamily::SyncQueue,
                      QueueKey(operation.sequence),
                      SyncOperationCodec::Serialize(operation),
                      true);
}

}  // namespace videosc::dedup
