#pragma once

#include "MySqlClient.h"
#include "RocksStore.h"

#include <cstdint>
#include <optional>
#include <string>

namespace videosc::dedup {

/** @brief 当前程序可复用的派生数据契约版本。 */
inline constexpr std::uint32_t kCurrentDataVersion = 1;

/** @brief 跨 RocksDB 与 MySQL 共享的数据重建状态。 */
enum class DataVersionState : std::uint32_t {
    Rebuilding = 1,
    Ready = 2,
};

/** @brief 单个存储中的数据版本、重建代际与可复用状态。 */
struct DataVersionRecord {
    std::uint32_t data_version = 0;
    std::uint64_t generation_id = 0;
    DataVersionState state = DataVersionState::Rebuilding;
    std::int64_t updated_at_utc_ms = 0;
};

/** @brief 双存储版本比较后的唯一运行决策。 */
enum class DataVersionDecision {
    ReuseReady,
    ResumeRebuild,
    ResetRequired,
    RejectNewerData,
};

/** @brief 数据版本检查或状态写入结果。 */
struct DataVersionResult {
    bool succeeded = false;
    unsigned int native_error = 0;
    std::string message;
    DataVersionDecision decision = DataVersionDecision::ResetRequired;
    std::optional<DataVersionRecord> rocksdb;
    std::optional<DataVersionRecord> mysql;
};

/**
 * @brief 协调 RocksDB 与 MySQL 派生数据版本的一致性。
 *
 * 本类只管理版本记录和受控全量重建，不决定扫描内容，也不允许删除数据库或源文件。
 */
class DataVersionCoordinator final {
public:
    /**
     * @brief 绑定已打开的 RocksDB 和已连接的 MySQL。
     * @param store 生命周期必须覆盖本对象。
     * @param client 生命周期必须覆盖本对象。
     */
    DataVersionCoordinator(RocksStore& store, MySqlClient& client);

    /**
     * @brief 纯比较两端版本记录，不执行任何 I/O。
     * @param rocksdb RocksDB 版本；空值表示尚未登记。
     * @param mysql MySQL 版本；空值表示尚未登记。
     * @return 当前程序必须执行的兼容决策。
     */
    static DataVersionDecision Evaluate(
        const std::optional<DataVersionRecord>& rocksdb,
        const std::optional<DataVersionRecord>& mysql) noexcept;

    /** @brief 读取两端版本并返回兼容决策。 */
    DataVersionResult Inspect() const;

    /**
     * @brief 清理两端 VideoSc 派生数据并创建新的 rebuilding generation。
     * @return 只有 MySQL 重建、RocksDB 清理和两端版本写入全部成功时才成功。
     */
    DataVersionResult ResetForCurrentVersion();

    /**
     * @brief 把指定 generation 在两端提交为 ready。
     * @param generation_id 必须与两端当前记录一致且非零。
     */
    DataVersionResult MarkReady(std::uint64_t generation_id);

    /** @brief 返回稳定的中文状态名。 */
    static const char* StateName(DataVersionState state) noexcept;

private:
    DataVersionResult ReadBoth() const;
    RocksStatus ReadRocks(std::optional<DataVersionRecord>& record) const;
    RocksStatus WriteRocks(const DataVersionRecord& record);
    MySqlStatus ReadMySql(std::optional<DataVersionRecord>& record) const;
    MySqlStatus WriteMySql(const DataVersionRecord& record);

    RocksStore& store_;
    MySqlClient& client_;
};

}  // namespace videosc::dedup
