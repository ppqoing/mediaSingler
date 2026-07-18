#pragma once

#include "ScanOptions.h"

#include <optional>
#include <string>

namespace videosc::dedup {

/**
 * @brief 把不可变 ScanOptions 编码为 RocksDB 任务快照 JSON。
 *
 * 快照包含恢复所需的路径、线程、I/O、数据库端点、缩略图和资源预算，但类型层面不存在密码字段。
 */
class ScanOptionsCodec final {
public:
    /**
     * @brief 序列化任务快照。
     * @param options 不可变任务选项。
     * @return UTF-8 JSON。
     * @throws std::runtime_error Unicode 转换或 JSON 序列化失败时抛出。
     */
    static std::string Serialize(const ScanOptions& options);

    /**
     * @brief 反序列化并重新执行配置校验。
     * @param json UTF-8 快照 JSON。
     * @param error 失败时写入错误文本。
     * @return 有效快照，版本、算法或字段非法时返回空。
     */
    static std::optional<ScanOptions> Deserialize(const std::string& json, std::string& error);
};

}  // namespace videosc::dedup
