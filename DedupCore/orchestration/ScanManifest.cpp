#include "ScanManifest.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace videosc::dedup {
namespace {

using Json = nlohmann::ordered_json;

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
    if (length <= 0) throw std::runtime_error("Cannot encode scan manifest value");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode scan manifest value");
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
    if (length <= 0) throw std::runtime_error("Cannot decode scan manifest value");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode scan manifest value");
    }
    return result;
}

/** @brief 把发现结果编码为单行 JSON。 */
std::string EncodeDiscovered(const DiscoveredFile& file) {
    const FilePathRecord& record = file.record;
    Json json = {{"path", WideToUtf8(record.path.wstring())},
                 {"normalized_path", WideToUtf8(record.normalized_path_key)},
                 {"volume_guid", WideToUtf8(record.volume_guid)},
                 {"storage_target_key", WideToUtf8(record.storage_target_key)},
                 {"size_bytes", record.size_bytes},
                 {"extension", WideToUtf8(record.extension)},
                 {"creation_time_utc_ms", record.creation_time_utc_ms},
                 {"last_write_time_utc_ms", record.last_write_time_utc_ms},
                 {"scan_root_priority", record.scan_root_priority},
                 {"scan_id", record.scan_id},
                 {"media_kind", static_cast<int>(file.media_kind)}};
    json["physical_start_byte"] = file.physical_start_byte.has_value()
                                      ? Json(*file.physical_start_byte)
                                      : Json(nullptr);
    return json.dump();
}

/** @brief 从单行 JSON 恢复发现结果。 */
DiscoveredFile DecodeDiscovered(const std::string& line) {
    const Json json = Json::parse(line);
    DiscoveredFile file;
    file.record.path = Utf8ToWide(json.at("path").get<std::string>());
    file.record.normalized_path_key = Utf8ToWide(json.at("normalized_path").get<std::string>());
    file.record.volume_guid = Utf8ToWide(json.at("volume_guid").get<std::string>());
    file.record.storage_target_key = Utf8ToWide(json.at("storage_target_key").get<std::string>());
    file.record.size_bytes = json.at("size_bytes").get<std::uint64_t>();
    file.record.extension = Utf8ToWide(json.at("extension").get<std::string>());
    file.record.creation_time_utc_ms = json.at("creation_time_utc_ms").get<std::int64_t>();
    file.record.last_write_time_utc_ms = json.at("last_write_time_utc_ms").get<std::int64_t>();
    file.record.scan_root_priority = json.at("scan_root_priority").get<std::uint32_t>();
    file.record.scan_id = json.at("scan_id").get<std::uint64_t>();
    file.record.state = FilePathState::Pending;
    file.record.sync_state = SyncState::LocalOnly;
    const int kind = json.at("media_kind").get<int>();
    if (kind < static_cast<int>(MediaKind::Other) || kind > static_cast<int>(MediaKind::Audio)) {
        throw std::runtime_error("Invalid manifest media kind");
    }
    file.media_kind = static_cast<MediaKind>(kind);
    if (!json.at("physical_start_byte").is_null()) {
        file.physical_start_byte = json.at("physical_start_byte").get<std::uint64_t>();
    }
    return file;
}

/** @brief 介质类型稳定名称。 */
const char* MediaTypeName(const StorageMediaType type) {
    if (type == StorageMediaType::Hdd) return "hdd";
    if (type == StorageMediaType::Ssd) return "ssd";
    return "unknown";
}

}  // namespace

/** @brief 单物理盘独立锁和输出流。 */
class ScanManifest::Writer final {
public:
    std::wstring storage_target_key;
    StorageMediaType media_type = StorageMediaType::Unknown;
    std::filesystem::path temporary_path;
    std::filesystem::path final_path;
    std::ofstream stream;
    std::uint64_t file_count = 0;
    mutable std::mutex mutex;
};

ScanManifest::ScanManifest(std::filesystem::path data_directory, const std::uint64_t scan_id)
    : directory_(std::move(data_directory) / L"scan_manifests" /
                 std::filesystem::path(std::to_wstring(scan_id))),
      scan_id_(scan_id) {}

ScanManifest::~ScanManifest() = default;

bool ScanManifest::Begin(std::string& error) {
    try {
        std::error_code filesystemError;
        std::filesystem::remove_all(directory_, filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
        std::filesystem::create_directories(directory_, filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
        std::lock_guard<std::mutex> lock(writers_mutex_);
        writers_.clear();
        completed_ = false;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::shared_ptr<ScanManifest::Writer> ScanManifest::GetOrCreateWriter(
    const DiscoveredFile& file,
    const StorageMediaType media_type,
    std::string& error) {
    std::lock_guard<std::mutex> lock(writers_mutex_);
    for (const auto& writer : writers_) {
        if (writer->storage_target_key == file.record.storage_target_key) return writer;
    }
    auto writer = std::make_shared<Writer>();
    writer->storage_target_key = file.record.storage_target_key;
    writer->media_type = media_type;
    std::ostringstream name;
    name << "disk_" << std::setw(3) << std::setfill('0') << writers_.size() << ".jsonl";
    writer->final_path = directory_ / std::filesystem::path(name.str());
    writer->temporary_path = writer->final_path;
    writer->temporary_path += L".tmp";
    writer->stream.open(writer->temporary_path, std::ios::binary | std::ios::trunc);
    if (!writer->stream.is_open()) {
        error = "Cannot open per-disk scan manifest";
        return {};
    }
    writers_.push_back(writer);
    return writer;
}

bool ScanManifest::Append(const DiscoveredFile& file,
                          const StorageMediaType media_type,
                          std::string& error) {
    try {
        const std::shared_ptr<Writer> writer = GetOrCreateWriter(file, media_type, error);
        if (!writer) return false;
        const std::string line = EncodeDiscovered(file);
        std::lock_guard<std::mutex> lock(writer->mutex);
        writer->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
        writer->stream.put('\n');
        if (!writer->stream.good()) {
            error = "Cannot write per-disk scan manifest";
            return false;
        }
        ++writer->file_count;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ScanManifest::Complete(std::string& error) {
    try {
        Json manifest = {{"version", 1}, {"scan_id", scan_id_}, {"completed", true}};
        manifest["disks"] = Json::array();
        std::lock_guard<std::mutex> writersLock(writers_mutex_);
        for (const auto& writer : writers_) {
            std::lock_guard<std::mutex> writerLock(writer->mutex);
            writer->stream.flush();
            writer->stream.close();
            if (writer->stream.fail()) {
                error = "Cannot finalize per-disk scan manifest";
                return false;
            }
            std::error_code filesystemError;
            std::filesystem::remove(writer->final_path, filesystemError);
            filesystemError.clear();
            std::filesystem::rename(writer->temporary_path, writer->final_path, filesystemError);
            if (filesystemError) {
                error = filesystemError.message();
                return false;
            }
            manifest["disks"].push_back({{"storage_target_key", WideToUtf8(writer->storage_target_key)},
                                          {"media_type", MediaTypeName(writer->media_type)},
                                          {"file_count", writer->file_count},
                                          {"file_name", writer->final_path.filename().string()}});
        }
        const std::filesystem::path temporary = directory_ / L"manifest.json.tmp";
        const std::filesystem::path final = directory_ / L"manifest.json";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const std::string encoded = manifest.dump(2);
        stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        stream.flush();
        stream.close();
        if (stream.fail()) {
            error = "Cannot write scan manifest metadata";
            return false;
        }
        std::error_code filesystemError;
        std::filesystem::remove(final, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, final, filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
        completed_ = true;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ScanManifest::ForEach(const Visitor& visitor, std::string& error) const {
    if (!visitor) {
        error = "Manifest visitor is required";
        return false;
    }
    try {
        const std::vector<ScanManifestFile> manifests = files();
        for (const ScanManifestFile& manifest : manifests) {
            if (!ForEachFile(manifest, visitor, error)) return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ScanManifest::ForEachFile(const ScanManifestFile& manifest,
                               const Visitor& visitor,
                               std::string& error) const {
    if (!visitor) {
        error = "Manifest visitor is required";
        return false;
    }
    try {
        std::ifstream stream(manifest.path, std::ios::binary);
        if (!stream.is_open()) {
            error = "Cannot open completed scan manifest";
            return false;
        }
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            if (!visitor(manifest, DecodeDiscovered(line))) {
                error.clear();
                return true;
            }
        }
        if (!stream.eof()) {
            error = "Cannot read completed scan manifest";
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::vector<ScanManifestFile> ScanManifest::files() const {
    std::vector<ScanManifestFile> result;
    std::lock_guard<std::mutex> lock(writers_mutex_);
    result.reserve(writers_.size());
    for (const auto& writer : writers_) {
        std::lock_guard<std::mutex> writerLock(writer->mutex);
        result.push_back({writer->storage_target_key,
                          writer->media_type,
                          writer->final_path,
                          writer->file_count});
    }
    return result;
}

bool ScanManifest::Cleanup(std::string& error) {
    std::error_code filesystemError;
    std::filesystem::remove_all(directory_, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    error.clear();
    return true;
}

const std::filesystem::path& ScanManifest::directory() const noexcept {
    return directory_;
}

}  // namespace videosc::dedup
