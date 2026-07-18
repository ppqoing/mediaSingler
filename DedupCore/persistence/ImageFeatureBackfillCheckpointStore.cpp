#include "ImageFeatureBackfillCheckpointStore.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace videosc::dedup {
namespace {

constexpr std::string_view kCheckpointKey = "image-feature-backfill/current";
constexpr std::uint8_t kCheckpointCodecVersion = 1;

/** @brief 以本机稳定小端格式追加固定宽度数值。 */
template <typename Value>
void Append(std::string& output, const Value value) {
    static_assert(std::is_integral_v<Value>);
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

/** @brief 追加有界字符串。 */
void AppendString(std::string& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Backfill checkpoint string is too long");
    }
    Append(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

/** @brief 回填检查点有界读取器。 */
class Reader final {
public:
    explicit Reader(const std::string& value) : value_(value) {}

    /** @return 读取的固定宽度数值。 @throws std::runtime_error 输入截断。 */
    template <typename Value>
    Value Read() {
        static_assert(std::is_integral_v<Value>);
        Require(sizeof(Value));
        Value result{};
        std::memcpy(&result, value_.data() + offset_, sizeof(Value));
        offset_ += sizeof(Value);
        return result;
    }

    /** @return 读取的有界字符串。 @throws std::runtime_error 输入截断。 */
    std::string ReadString() {
        const std::uint32_t size = Read<std::uint32_t>();
        Require(size);
        std::string result(value_.data() + offset_, size);
        offset_ += size;
        return result;
    }

    /** @throws std::runtime_error 存在尾随字节。 */
    void RequireEnd() const {
        if (offset_ != value_.size()) throw std::runtime_error("Backfill checkpoint has trailing bytes");
    }

private:
    void Require(const std::size_t size) const {
        if (size > value_.size() - offset_) throw std::runtime_error("Backfill checkpoint is truncated");
    }

    const std::string& value_;
    std::size_t offset_ = 0;
};

/** @brief 编码回填检查点。 */
std::string Serialize(const ImageFeatureBackfillCheckpoint& checkpoint) {
    std::string output;
    Append(output, kCheckpointCodecVersion);
    AppendString(output, checkpoint.algorithm_version);
    AppendString(output, checkpoint.last_sha512);
    Append(output, checkpoint.total_images);
    Append(output, checkpoint.completed_images);
    Append(output, checkpoint.failed_images);
    Append(output, checkpoint.no_readable_path_images);
    Append(output, checkpoint.timeout_images);
    Append(output, checkpoint.decode_failed_images);
    Append(output, checkpoint.started_utc_ms);
    Append(output, checkpoint.updated_utc_ms);
    Append(output, static_cast<std::uint8_t>(checkpoint.finished ? 1 : 0));
    return output;
}

/** @brief 解码并校验回填检查点。 */
ImageFeatureBackfillCheckpoint Deserialize(const std::string& value) {
    Reader reader(value);
    if (reader.Read<std::uint8_t>() != kCheckpointCodecVersion) {
        throw std::runtime_error("Unsupported backfill checkpoint version");
    }
    ImageFeatureBackfillCheckpoint checkpoint;
    checkpoint.algorithm_version = reader.ReadString();
    checkpoint.last_sha512 = reader.ReadString();
    checkpoint.total_images = reader.Read<std::uint64_t>();
    checkpoint.completed_images = reader.Read<std::uint64_t>();
    checkpoint.failed_images = reader.Read<std::uint64_t>();
    checkpoint.no_readable_path_images = reader.Read<std::uint64_t>();
    checkpoint.timeout_images = reader.Read<std::uint64_t>();
    checkpoint.decode_failed_images = reader.Read<std::uint64_t>();
    checkpoint.started_utc_ms = reader.Read<std::int64_t>();
    checkpoint.updated_utc_ms = reader.Read<std::int64_t>();
    const std::uint8_t finished = reader.Read<std::uint8_t>();
    if (finished > 1 || checkpoint.completed_images + checkpoint.failed_images > checkpoint.total_images) {
        throw std::runtime_error("Invalid backfill checkpoint counters");
    }
    checkpoint.finished = finished != 0;
    reader.RequireEnd();
    return checkpoint;
}

}  // namespace

ImageFeatureBackfillCheckpointStore::ImageFeatureBackfillCheckpointStore(RocksStore& store) noexcept
    : store_(store) {}

RocksStatus ImageFeatureBackfillCheckpointStore::Save(
    const ImageFeatureBackfillCheckpoint& checkpoint) {
    try {
        return store_.Put(RocksColumnFamily::Checkpoints, kCheckpointKey, Serialize(checkpoint), true);
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

std::optional<ImageFeatureBackfillCheckpoint> ImageFeatureBackfillCheckpointStore::Load(
    std::string& error) const {
    std::string value;
    const RocksStatus loaded = store_.Get(RocksColumnFamily::Checkpoints, kCheckpointKey, value);
    if (!loaded.succeeded) {
        error = loaded.message == "not_found" ? "" : loaded.message;
        return std::nullopt;
    }
    try {
        ImageFeatureBackfillCheckpoint checkpoint = Deserialize(value);
        error.clear();
        return checkpoint;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

RocksStatus ImageFeatureBackfillCheckpointStore::Clear() {
    return store_.Delete(RocksColumnFamily::Checkpoints, kCheckpointKey, true);
}

}  // namespace videosc::dedup
