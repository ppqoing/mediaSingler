#pragma once

#include "../config/AppConfig.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief MySQL 操作结果，不包含 SQL 文本、密码或其他敏感配置。 */
struct MySqlStatus {
    bool succeeded = false;
    unsigned int native_error = 0;
    std::string message;
};

/** @brief MySQL 一行结果；NULL 与空字符串保持可区分。 */
using MySqlRow = std::vector<std::optional<std::string>>;

/**
 * @brief MySQL C API 的串行连接封装。
 *
 * 一个实例只持有一个连接并在内部串行化调用。大结果使用 mysql_use_result 流式回调，
 * 不把千万级结果加载到内存。连接错误只返回原生错误，不拼接 SQL 或密码。
 */
class MySqlClient final {
public:
    using RowVisitor = std::function<bool(const MySqlRow& row)>;

    /** @brief 保存连接配置快照；密码只驻留进程内存。 */
    explicit MySqlClient(DatabaseConfig config);
    ~MySqlClient();

    MySqlClient(const MySqlClient&) = delete;
    MySqlClient& operator=(const MySqlClient&) = delete;

    /** @brief 建立连接并切换到 utf8mb4。重复调用时先关闭旧连接。 */
    MySqlStatus Connect();

    /** @brief 关闭连接；可重复调用。 */
    void Disconnect() noexcept;

    /** @return 当前是否持有已建立的连接句柄。 */
    bool is_connected() const noexcept;

    /** @brief 使用 mysql_ping 检查连接。 */
    MySqlStatus Ping();

    /** @brief 执行不返回结果集的 SQL。 */
    MySqlStatus Execute(const std::string& sql);

    /** @brief 在同一连接锁和事务中按顺序执行多条 SQL，任一失败即回滚。 */
    MySqlStatus ExecuteTransaction(const std::vector<std::string>& statements);

    /**
     * @brief 流式读取查询结果。
     * @param sql 查询语句。
     * @param visitor 每行调用一次；返回 false 主动停止读取。
     */
    MySqlStatus Query(const std::string& sql, const RowVisitor& visitor);

    /** @brief 使用当前连接将 UTF-8 文本转换为带单引号的安全 SQL 字面量。 */
    MySqlStatus EscapeLiteral(const std::string& value, std::string& literal);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videosc::dedup
