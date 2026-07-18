#include "ScanCheckpointStore.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace videosc::dedup {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::uint32_t kCheckpointRecordVersion = 1;

/** @brief 创建保持数字排序的 scan/20位十进制键。 */
std::string CheckpointKey(const std::uint64_t scan_id) {
    char key[32]{};
    std::snprintf(key, sizeof(key), "scan/%020llu", static_cast<unsigned long long>(scan_id));
    return key;
}

/** @brief 获取 Unix epoch 毫秒时间。 */
std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** @brief 把检查点编码为稳定 JSON。 */
std::string SerializeCheckpoint(const ScanCheckpoint& checkpoint) {
    Json json = {{"record_version", checkpoint.record_version},
                 {"scan_id", checkpoint.scan_id},
                 {"phase", static_cast<int>(checkpoint.phase)},
                 {"scan_options_json", checkpoint.scan_options_json},
                 {"discovery_cursor", checkpoint.discovery_cursor},
                 {"discovered_files", checkpoint.discovered_files},
                 {"completed_files", checkpoint.completed_files},
                 {"failed_files", checkpoint.failed_files},
                 {"media_completed_files", checkpoint.media_completed_files},
                 {"image_feature_failed_contents", checkpoint.image_feature_failed_contents},
                 {"next_sync_sequence", checkpoint.next_sync_sequence},
                 {"updated_utc_ms", checkpoint.updated_utc_ms}};
    return json.dump();
}

/** @brief 解析并校验单个检查点 JSON。 */
ScanCheckpoint DeserializeCheckpoint(const std::string_view value) {
    const Json json = Json::parse(value);
    ScanCheckpoint checkpoint;
    checkpoint.record_version = json.at("record_version").get<std::uint32_t>();
    if (checkpoint.record_version != kCheckpointRecordVersion) {
        throw std::runtime_error("unsupported checkpoint record version");
    }
    checkpoint.scan_id = json.at("scan_id").get<std::uint64_t>();
    const int phase = json.at("phase").get<int>();
    if (phase < static_cast<int>(ScanPhase::Idle) || phase > static_cast<int>(ScanPhase::FlushingSyncTail)) {
        throw std::runtime_error("invalid checkpoint phase");
    }
    checkpoint.phase = static_cast<ScanPhase>(phase);
    checkpoint.scan_options_json = json.at("scan_options_json").get<std::string>();
    checkpoint.discovery_cursor = json.at("discovery_cursor").get<std::string>();
    checkpoint.discovered_files = json.at("discovered_files").get<std::uint64_t>();
    checkpoint.completed_files = json.at("completed_files").get<std::uint64_t>();
    checkpoint.failed_files = json.at("failed_files").get<std::uint64_t>();
    checkpoint.media_completed_files = json.at("media_completed_files").get<std::uint64_t>();
    checkpoint.image_feature_failed_contents =
        json.value("image_feature_failed_contents", std::uint64_t{0});
    checkpoint.next_sync_sequence = json.at("next_sync_sequence").get<std::uint64_t>();
    checkpoint.updated_utc_ms = json.at("updated_utc_ms").get<std::int64_t>();
    return checkpoint;
}

/** @brief 判断任务状态是否允许断点恢复。 */
bool IsResumable(const ScanPhase phase) {
    return phase == ScanPhase::Discovering || phase == ScanPhase::Hashing ||
           phase == ScanPhase::ExtractingMedia || phase == ScanPhase::Syncing ||
           phase == ScanPhase::Planning || phase == ScanPhase::FlushingSyncTail ||
           phase == ScanPhase::Paused || phase == ScanPhase::CompletedLocal ||
           phase == ScanPhase::Interrupted;
}

}  // namespace

ScanCheckpointStore::ScanCheckpointStore(RocksStore& store) : store_(store) {}

RocksStatus ScanCheckpointStore::Save(const ScanCheckpoint& checkpoint) {
    ScanCheckpoint value = checkpoint;
    value.record_version = kCheckpointRecordVersion;
    value.updated_utc_ms = UtcNowMilliseconds();
    return store_.Put(RocksColumnFamily::Checkpoints,
                      CheckpointKey(value.scan_id),
                      SerializeCheckpoint(value),
                      true);
}

RocksStatus ScanCheckpointStore::SaveWithMutations(const ScanCheckpoint& checkpoint,
                                                   std::vector<RocksMutation> mutations) {
    ScanCheckpoint value = checkpoint;
    value.record_version = kCheckpointRecordVersion;
    value.updated_utc_ms = UtcNowMilliseconds();
    mutations.push_back({RocksColumnFamily::Checkpoints,
                         CheckpointKey(value.scan_id),
                         SerializeCheckpoint(value)});
    return store_.WriteBatch(mutations, true);
}

ScanCheckpointLoadResult ScanCheckpointStore::Load(const std::uint64_t scan_id) const {
    std::string value;
    RocksStatus status = store_.Get(RocksColumnFamily::Checkpoints, CheckpointKey(scan_id), value);
    if (!status.succeeded) return {std::move(status), std::nullopt};
    try {
        return {{true, {}}, DeserializeCheckpoint(value)};
    } catch (const std::exception& exception) {
        return {{false, exception.what()}, std::nullopt};
    }
}

RocksStatus ScanCheckpointStore::ListResumable(
    const std::size_t maximum_items,
    std::vector<ScanCheckpoint>& checkpoints) const {
    checkpoints.clear();
    RocksStatus iteration_status{true, {}};
    const RocksStatus status = store_.ForEachPrefix(
        RocksColumnFamily::Checkpoints,
        "scan/",
        0,
        [&](const std::string_view, const std::string_view value) {
            try {
                ScanCheckpoint checkpoint = DeserializeCheckpoint(value);
                if (IsResumable(checkpoint.phase)) {
                    checkpoints.push_back(std::move(checkpoint));
                    if (maximum_items != 0 && checkpoints.size() >= maximum_items) return false;
                }
                return true;
            } catch (const std::exception& exception) {
                iteration_status = {false, exception.what()};
                return false;
            }
        });
    return status.succeeded ? iteration_status : status;
}

RocksStatus ScanCheckpointStore::MarkInterrupted(const std::uint64_t scan_id) {
    ScanCheckpointLoadResult load = Load(scan_id);
    if (!load.status.succeeded || !load.checkpoint.has_value()) return load.status;
    load.checkpoint->phase = ScanPhase::Interrupted;
    return Save(*load.checkpoint);
}

}  // namespace videosc::dedup
