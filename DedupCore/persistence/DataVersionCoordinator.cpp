#include "DataVersionCoordinator.h"

#include "MySqlSchema.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <string_view>
#include <type_traits>

#pragma comment(lib, "bcrypt.lib")

namespace videosc::dedup {
namespace {

constexpr std::string_view kRocksDataVersionKey = "metadata/data_version";

/** @brief 返回当前 Unix epoch 毫秒，用于本地诊断时间。 */
std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** @brief 生成非零随机 generation；失败时使用进程与单调时钟组合兜底。 */
std::uint64_t RandomGenerationId() {
    std::uint64_t value = 0;
    do {
        if (BCryptGenRandom(nullptr,
                            reinterpret_cast<PUCHAR>(&value),
                            sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            value = static_cast<std::uint64_t>(GetTickCount64()) ^
                    (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        }
    } while (value == 0);
    return value;
}

/** @brief 把无符号十进制字段解析到目标类型。 */
template <typename T>
bool ParseUnsigned(const std::string_view text, T& value) {
    static_assert(std::is_unsigned_v<T>);
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) {
        return false;
    }
    value = static_cast<T>(parsed);
    return true;
}

/** @brief 编码本地版本记录；字段顺序是持久化契约的一部分。 */
std::string SerializeRecord(const DataVersionRecord& record) {
    return std::to_string(record.data_version) + '|' +
           std::to_string(record.generation_id) + '|' +
           std::to_string(static_cast<std::uint32_t>(record.state)) + '|' +
           std::to_string(record.updated_at_utc_ms);
}

/** @brief 解析并严格校验本地版本记录。 */
bool DeserializeRecord(const std::string_view value, DataVersionRecord& record) {
    std::array<std::string_view, 4> fields{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const std::size_t separator = value.find('|', begin);
        if (index + 1 == fields.size()) {
            if (separator != std::string_view::npos) return false;
            fields[index] = value.substr(begin);
        } else {
            if (separator == std::string_view::npos) return false;
            fields[index] = value.substr(begin, separator - begin);
            begin = separator + 1;
        }
    }

    std::uint32_t state = 0;
    std::uint64_t updated = 0;
    if (!ParseUnsigned(fields[0], record.data_version) ||
        !ParseUnsigned(fields[1], record.generation_id) ||
        !ParseUnsigned(fields[2], state) ||
        !ParseUnsigned(fields[3], updated) ||
        record.data_version == 0 || record.generation_id == 0 ||
        (state != static_cast<std::uint32_t>(DataVersionState::Rebuilding) &&
         state != static_cast<std::uint32_t>(DataVersionState::Ready)) ||
        updated > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    record.state = static_cast<DataVersionState>(state);
    record.updated_at_utc_ms = static_cast<std::int64_t>(updated);
    return true;
}

/** @brief 把 MySQL 可选列转换为必需的无符号整数。 */
template <typename T>
bool ParseRequiredColumn(const MySqlRow& row, const std::size_t index, T& value) {
    return index < row.size() && row[index].has_value() && ParseUnsigned(*row[index], value);
}

}  // namespace

DataVersionCoordinator::DataVersionCoordinator(RocksStore& store, MySqlClient& client)
    : store_(store), client_(client) {}

DataVersionDecision DataVersionCoordinator::Evaluate(
    const std::optional<DataVersionRecord>& rocksdb,
    const std::optional<DataVersionRecord>& mysql) noexcept {
    if ((rocksdb.has_value() && rocksdb->data_version > kCurrentDataVersion) ||
        (mysql.has_value() && mysql->data_version > kCurrentDataVersion)) {
        return DataVersionDecision::RejectNewerData;
    }
    if (!rocksdb.has_value() || !mysql.has_value() ||
        rocksdb->data_version < kCurrentDataVersion ||
        mysql->data_version < kCurrentDataVersion ||
        rocksdb->data_version != mysql->data_version ||
        rocksdb->generation_id == 0 || rocksdb->generation_id != mysql->generation_id) {
        return DataVersionDecision::ResetRequired;
    }
    if (rocksdb->state == DataVersionState::Ready &&
        mysql->state == DataVersionState::Ready) {
        return DataVersionDecision::ReuseReady;
    }
    // 同 generation 的混合状态可能来自 ready 两步提交之间的退出，保持不可报告并允许安全收尾。
    return DataVersionDecision::ResumeRebuild;
}

DataVersionResult DataVersionCoordinator::Inspect() const {
    DataVersionResult result = ReadBoth();
    if (result.succeeded) result.decision = Evaluate(result.rocksdb, result.mysql);
    return result;
}

DataVersionResult DataVersionCoordinator::ResetForCurrentVersion() {
    DataVersionResult result;
    const MySqlStatus reset = MySqlSchema::ResetBusinessTables(client_);
    if (!reset.succeeded) {
        result.native_error = reset.native_error;
        result.message = "MySQL 业务表重置失败：" + reset.message;
        return result;
    }
    const MySqlSchemaResult schema = MySqlSchema::Initialize(client_);
    if (!schema.status.succeeded) {
        result.native_error = schema.status.native_error;
        result.message = "MySQL 业务表重建失败：" + schema.status.message;
        return result;
    }
    const RocksStatus cleared = store_.ClearAll(4096, true);
    if (!cleared.succeeded) {
        result.message = "RocksDB 全量清理失败：" + cleared.message;
        return result;
    }

    DataVersionRecord record;
    record.data_version = kCurrentDataVersion;
    record.generation_id = RandomGenerationId();
    record.state = DataVersionState::Rebuilding;
    record.updated_at_utc_ms = UtcNowMilliseconds();
    const MySqlStatus mysqlWritten = WriteMySql(record);
    if (!mysqlWritten.succeeded) {
        result.native_error = mysqlWritten.native_error;
        result.message = "MySQL rebuilding 版本写入失败：" + mysqlWritten.message;
        return result;
    }
    const RocksStatus rocksWritten = WriteRocks(record);
    if (!rocksWritten.succeeded) {
        result.message = "RocksDB rebuilding 版本写入失败：" + rocksWritten.message;
        return result;
    }
    result.succeeded = true;
    result.decision = DataVersionDecision::ResumeRebuild;
    result.rocksdb = record;
    result.mysql = record;
    result.message = "旧派生数据已清理，等待全量扫描和 MySQL 同步完成";
    return result;
}

DataVersionResult DataVersionCoordinator::MarkReady(const std::uint64_t generation_id) {
    DataVersionResult current = ReadBoth();
    if (!current.succeeded) return current;
    if (generation_id == 0 || !current.rocksdb.has_value() || !current.mysql.has_value() ||
        current.rocksdb->data_version != kCurrentDataVersion ||
        current.mysql->data_version != kCurrentDataVersion ||
        current.rocksdb->generation_id != generation_id ||
        current.mysql->generation_id != generation_id) {
        current.succeeded = false;
        current.message = "数据 generation 已变化，拒绝提交 ready";
        return current;
    }

    DataVersionRecord ready;
    ready.data_version = kCurrentDataVersion;
    ready.generation_id = generation_id;
    ready.state = DataVersionState::Ready;
    ready.updated_at_utc_ms = UtcNowMilliseconds();
    const MySqlStatus mysqlWritten = WriteMySql(ready);
    if (!mysqlWritten.succeeded) {
        current.succeeded = false;
        current.native_error = mysqlWritten.native_error;
        current.message = "MySQL ready 版本写入失败：" + mysqlWritten.message;
        return current;
    }
    const RocksStatus rocksWritten = WriteRocks(ready);
    if (!rocksWritten.succeeded) {
        current.succeeded = false;
        current.message = "RocksDB ready 版本写入失败：" + rocksWritten.message;
        current.mysql = ready;
        return current;
    }
    current.succeeded = true;
    current.decision = DataVersionDecision::ReuseReady;
    current.rocksdb = ready;
    current.mysql = ready;
    current.message = "全量数据版本已提交为 ready";
    return current;
}

const char* DataVersionCoordinator::StateName(const DataVersionState state) noexcept {
    return state == DataVersionState::Ready ? "ready" : "rebuilding";
}

DataVersionResult DataVersionCoordinator::ReadBoth() const {
    DataVersionResult result;
    const RocksStatus rocks = ReadRocks(result.rocksdb);
    if (!rocks.succeeded) {
        result.message = "读取 RocksDB 数据版本失败：" + rocks.message;
        return result;
    }
    // 先拒绝本地高版本，避免旧程序为了读取另一端而创建或修改任何 MySQL 元数据。
    if (result.rocksdb.has_value() &&
        result.rocksdb->data_version > kCurrentDataVersion) {
        result.succeeded = true;
        result.decision = DataVersionDecision::RejectNewerData;
        result.message = "RocksDB 数据版本高于当前程序";
        return result;
    }
    const MySqlStatus mysql = ReadMySql(result.mysql);
    if (!mysql.succeeded) {
        result.native_error = mysql.native_error;
        result.message = "读取 MySQL 数据版本失败：" + mysql.message;
        return result;
    }
    result.succeeded = true;
    result.decision = Evaluate(result.rocksdb, result.mysql);
    return result;
}

RocksStatus DataVersionCoordinator::ReadRocks(
    std::optional<DataVersionRecord>& record) const {
    record.reset();
    std::string value;
    const RocksStatus status = store_.Get(RocksColumnFamily::Default, kRocksDataVersionKey, value);
    if (!status.succeeded) {
        return status.message == "not_found" ? RocksStatus{true, {}} : status;
    }
    DataVersionRecord parsed;
    if (!DeserializeRecord(value, parsed)) return {false, "invalid_data_version_record"};
    record = parsed;
    return {true, {}};
}

RocksStatus DataVersionCoordinator::WriteRocks(const DataVersionRecord& record) {
    return store_.Put(RocksColumnFamily::Default,
                      kRocksDataVersionKey,
                      SerializeRecord(record),
                      true);
}

MySqlStatus DataVersionCoordinator::ReadMySql(
    std::optional<DataVersionRecord>& record) const {
    record.reset();
    MySqlStatus parseStatus{true, 0, {}};
    const MySqlStatus queried = client_.Query(
        "SELECT data_version,generation_id,data_state FROM videosc_data_version WHERE singleton_id=1",
        [&](const MySqlRow& row) {
            DataVersionRecord parsed;
            std::uint32_t state = 0;
            if (row.size() != 3 ||
                !ParseRequiredColumn(row, 0, parsed.data_version) ||
                !ParseRequiredColumn(row, 1, parsed.generation_id) ||
                !ParseRequiredColumn(row, 2, state) ||
                parsed.data_version == 0 || parsed.generation_id == 0 ||
                (state != static_cast<std::uint32_t>(DataVersionState::Rebuilding) &&
                 state != static_cast<std::uint32_t>(DataVersionState::Ready))) {
                parseStatus = {false, 0, "invalid_mysql_data_version_record"};
                return false;
            }
            parsed.state = static_cast<DataVersionState>(state);
            record = parsed;
            return false;
        });
    if (!queried.succeeded) return queried;
    if (!parseStatus.succeeded) return parseStatus;
    return {true, 0, {}};
}

MySqlStatus DataVersionCoordinator::WriteMySql(const DataVersionRecord& record) {
    return client_.Execute(
        "INSERT INTO videosc_data_version(singleton_id,data_version,generation_id,data_state) VALUES (1," +
        std::to_string(record.data_version) + ',' + std::to_string(record.generation_id) + ',' +
        std::to_string(static_cast<std::uint32_t>(record.state)) + ") AS incoming "
        "ON DUPLICATE KEY UPDATE data_version=incoming.data_version,generation_id=incoming.generation_id,"
        "data_state=incoming.data_state");
}

}  // namespace videosc::dedup
