#include "CoreModelCodec.h"

#include <Windows.h>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace videosc::dedup {
namespace {

constexpr std::uint8_t kFilePathVersion = 1;
constexpr std::uint8_t kShaDataVersion = 2;
constexpr std::uint8_t kLegacyShaDataVersion = 1;
constexpr std::uint32_t kMaximumStringBytes = 1024U * 1024U;

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
    if (length <= 0) throw std::runtime_error("Cannot encode model string as UTF-8");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        throw std::runtime_error("Cannot encode model string as UTF-8");
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
    if (length <= 0) throw std::runtime_error("Cannot decode model UTF-8 string");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length) != length) {
        throw std::runtime_error("Cannot decode model UTF-8 string");
    }
    return result;
}

/** @brief 以小端序追加整数或 IEEE 浮点原始位。 */
template <typename Value>
void Append(std::string& output, const Value value) {
    static_assert(std::is_integral_v<Value> || std::is_floating_point_v<Value>);
    std::array<std::uint8_t, sizeof(Value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(Value));
    output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/** @brief 追加 uint32 长度 UTF-8 字符串。 */
void AppendString(std::string& output, const std::string& value) {
    if (value.size() > kMaximumStringBytes) throw std::runtime_error("Model string is too long");
    Append(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

/** @brief 有界二进制读取器，任何截断都抛出统一异常。 */
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

    std::string ReadString() {
        const std::uint32_t size = Read<std::uint32_t>();
        if (size > kMaximumStringBytes) throw std::runtime_error("Model string is too long");
        Require(size);
        std::string result(value_.data() + offset_, size);
        offset_ += size;
        return result;
    }

    void ReadBytes(void* destination, const std::size_t size) {
        Require(size);
        std::memcpy(destination, value_.data() + offset_, size);
        offset_ += size;
    }

    void RequireEnd() const {
        if (offset_ != value_.size()) throw std::runtime_error("Model record has trailing bytes");
    }

private:
    void Require(const std::size_t size) const {
        if (size > value_.size() - offset_) throw std::runtime_error("Model record is truncated");
    }

    const std::string& value_;
    std::size_t offset_ = 0;
};

/** @brief 校验枚举值位于闭区间。 */
template <typename Enum>
Enum CheckedEnum(const std::uint8_t value, const Enum first, const Enum last) {
    if (value < static_cast<std::uint8_t>(first) || value > static_cast<std::uint8_t>(last)) {
        throw std::runtime_error("Invalid model enum value");
    }
    return static_cast<Enum>(value);
}

}  // namespace

std::string CoreModelCodec::SerializeFilePath(const FilePathRecord& record) {
    std::string output;
    output.reserve(512);
    Append(output, kFilePathVersion);
    Append(output, record.path_id);
    Append(output, record.scan_id);
    Append(output, record.identity.volume_serial);
    Append(output, record.identity.file_id_high);
    Append(output, record.identity.file_id_low);
    Append(output, record.size_bytes);
    Append(output, record.creation_time_utc_ms);
    Append(output, record.last_write_time_utc_ms);
    Append(output, record.scan_root_priority);
    Append(output, static_cast<std::uint8_t>(record.state));
    Append(output, static_cast<std::uint8_t>(record.sync_state));
    Append(output, static_cast<std::uint8_t>(record.sha512.has_value() ? 1 : 0));
    if (record.sha512.has_value()) {
        output.append(reinterpret_cast<const char*>(record.sha512->data()), record.sha512->size());
    }
    AppendString(output, WideToUtf8(record.path.wstring()));
    AppendString(output, WideToUtf8(record.normalized_path_key));
    AppendString(output, WideToUtf8(record.volume_guid));
    AppendString(output, WideToUtf8(record.storage_target_key));
    AppendString(output, WideToUtf8(record.extension));
    return output;
}

std::optional<FilePathRecord> CoreModelCodec::DeserializeFilePath(const std::string& value, std::string& error) {
    try {
        Reader reader(value);
        if (reader.Read<std::uint8_t>() != kFilePathVersion) throw std::runtime_error("Unsupported file path record");
        FilePathRecord record;
        record.path_id = reader.Read<std::uint64_t>();
        record.scan_id = reader.Read<std::uint64_t>();
        record.identity.volume_serial = reader.Read<std::uint64_t>();
        record.identity.file_id_high = reader.Read<std::uint64_t>();
        record.identity.file_id_low = reader.Read<std::uint64_t>();
        record.size_bytes = reader.Read<std::uint64_t>();
        record.creation_time_utc_ms = reader.Read<std::int64_t>();
        record.last_write_time_utc_ms = reader.Read<std::int64_t>();
        record.scan_root_priority = reader.Read<std::uint32_t>();
        record.state = CheckedEnum(reader.Read<std::uint8_t>(), FilePathState::Pending, FilePathState::ReadTimeout);
        record.sync_state = CheckedEnum(reader.Read<std::uint8_t>(), SyncState::LocalOnly, SyncState::FailedRetryable);
        const std::uint8_t hasSha = reader.Read<std::uint8_t>();
        if (hasSha > 1) throw std::runtime_error("Invalid SHA-512 presence flag");
        if (hasSha != 0) {
            Sha512Digest digest{};
            reader.ReadBytes(digest.data(), digest.size());
            record.sha512 = digest;
        }
        record.path = Utf8ToWide(reader.ReadString());
        record.normalized_path_key = Utf8ToWide(reader.ReadString());
        record.volume_guid = Utf8ToWide(reader.ReadString());
        record.storage_target_key = Utf8ToWide(reader.ReadString());
        record.extension = Utf8ToWide(reader.ReadString());
        reader.RequireEnd();
        error.clear();
        return record;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::string CoreModelCodec::SerializeShaFileData(const ShaFileData& data) {
    std::string output;
    output.reserve(256);
    Append(output, kShaDataVersion);
    output.append(reinterpret_cast<const char*>(data.sha512.data()), data.sha512.size());
    Append(output, data.content_size_bytes);
    Append(output, static_cast<std::uint8_t>(data.media_kind));
    Append(output, data.width);
    Append(output, data.height);
    Append(output, static_cast<std::uint8_t>(data.image_dhash.has_value() ? 1 : 0));
    Append(output, data.image_dhash.value_or(0));
    Append(output, data.video_duration_ms);
    Append(output, data.video_frame_rate);
    Append(output, data.video_bitrate);
    for (const std::uint64_t hash : data.video_dhashes) Append(output, hash);
    Append(output, static_cast<std::uint8_t>(data.has_video_dhashes ? 1 : 0));
    Append(output, static_cast<std::uint8_t>(data.static_visual ? 1 : 0));
    AppendString(output, data.mime_type);
    AppendString(output, data.container_name);
    AppendString(output, data.video_codec);
    AppendString(output, data.pixel_format);
    AppendString(output, WideToUtf8(data.contact_sheet_path.wstring()));
    AppendString(output, data.media_algorithm_version);
    Append(output, static_cast<std::uint8_t>(data.image_pdq_hash.has_value() ? 1 : 0));
    const PdqHash256 emptyPdq{};
    const PdqHash256& pdq = data.image_pdq_hash.has_value() ? *data.image_pdq_hash : emptyPdq;
    output.append(reinterpret_cast<const char*>(pdq.data()), pdq.size());
    Append(output, data.image_pdq_quality.value_or(0));
    Append(output, static_cast<std::uint8_t>(data.has_image_zoned_phashes ? 1 : 0));
    for (const std::uint64_t hash : data.image_zoned_phashes) Append(output, hash);
    Append(output, data.image_perceptual_algorithm_version);
    Append(output, data.image_structural_algorithm_version);
    return output;
}

std::optional<ShaFileData> CoreModelCodec::DeserializeShaFileData(const std::string& value, std::string& error) {
    try {
        Reader reader(value);
        const std::uint8_t version = reader.Read<std::uint8_t>();
        if (version != kLegacyShaDataVersion && version != kShaDataVersion) {
            throw std::runtime_error("Unsupported content record");
        }
        ShaFileData data;
        reader.ReadBytes(data.sha512.data(), data.sha512.size());
        data.content_size_bytes = reader.Read<std::uint64_t>();
        data.media_kind = CheckedEnum(reader.Read<std::uint8_t>(), MediaKind::Other, MediaKind::Audio);
        data.width = reader.Read<std::uint32_t>();
        data.height = reader.Read<std::uint32_t>();
        const std::uint8_t hasImageDhash = reader.Read<std::uint8_t>();
        const std::uint64_t imageDhash = reader.Read<std::uint64_t>();
        if (hasImageDhash > 1) throw std::runtime_error("Invalid image dHash presence flag");
        if (hasImageDhash != 0) data.image_dhash = imageDhash;
        data.video_duration_ms = reader.Read<std::int64_t>();
        data.video_frame_rate = reader.Read<double>();
        data.video_bitrate = reader.Read<std::uint64_t>();
        for (std::uint64_t& hash : data.video_dhashes) hash = reader.Read<std::uint64_t>();
        data.has_video_dhashes = reader.Read<std::uint8_t>() != 0;
        data.static_visual = reader.Read<std::uint8_t>() != 0;
        data.mime_type = reader.ReadString();
        data.container_name = reader.ReadString();
        data.video_codec = reader.ReadString();
        data.pixel_format = reader.ReadString();
        data.contact_sheet_path = Utf8ToWide(reader.ReadString());
        data.media_algorithm_version = reader.ReadString();
        if (version >= 2) {
            const std::uint8_t hasPdq = reader.Read<std::uint8_t>();
            PdqHash256 pdq{};
            reader.ReadBytes(pdq.data(), pdq.size());
            const std::uint8_t pdqQuality = reader.Read<std::uint8_t>();
            const std::uint8_t hasZonedPHashes = reader.Read<std::uint8_t>();
            if (hasPdq > 1 || hasZonedPHashes > 1) {
                throw std::runtime_error("Invalid image perceptual presence flag");
            }
            if (hasPdq != 0) {
                data.image_pdq_hash = pdq;
                data.image_pdq_quality = pdqQuality;
            }
            for (std::uint64_t& hash : data.image_zoned_phashes) hash = reader.Read<std::uint64_t>();
            data.has_image_zoned_phashes = hasZonedPHashes != 0;
            data.image_perceptual_algorithm_version = reader.Read<std::uint32_t>();
            data.image_structural_algorithm_version = reader.Read<std::uint32_t>();
        }
        reader.RequireEnd();
        error.clear();
        return data;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace videosc::dedup
