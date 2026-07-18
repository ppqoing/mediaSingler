#pragma once

#include "CoreModels.h"

#include <optional>
#include <string>

namespace videosc::dedup {

/** @brief RocksDB 热表使用的版本化紧凑二进制模型编解码。 */
class CoreModelCodec final {
public:
    static std::string SerializeFilePath(const FilePathRecord& record);
    static std::optional<FilePathRecord> DeserializeFilePath(const std::string& value, std::string& error);

    static std::string SerializeShaFileData(const ShaFileData& data);
    static std::optional<ShaFileData> DeserializeShaFileData(const std::string& value, std::string& error);
};

}  // namespace videosc::dedup
