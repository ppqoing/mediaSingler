#pragma once

#include "MySqlClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videosc::dedup {

/** @brief 当前应用 MySQL 模式版本。 */
inline constexpr std::uint32_t kCurrentMySqlSchemaVersion = 3;

/** @brief 初始化或检查数据库表的结果。 */
struct MySqlSchemaResult {
    MySqlStatus status;
    std::uint32_t previous_version = 0;
    std::uint32_t current_version = 0;
    bool created_initial_schema = false;
    bool migrated_schema = false;
};

/**
 * @brief 去重工具 MySQL 模式初始化器。
 *
 * Initialize 只执行幂等建表、已知迁移、契约校验与版本登记。
 * ResetBusinessTables 是单独的受控入口，只删除编译期固定白名单内的 VideoSc 业务表。
 * GUI 编排必须在已有表上调用初始化入口前完成 mysqldump 备份。
 */
class MySqlSchema final {
public:
    /** @brief 返回按执行顺序排列的幂等建表语句，供初始化和安全检查复用。 */
    static const std::vector<std::string>& InitializationStatements();

    /** @brief 返回固定白名单业务表的受控重置语句，禁止包含数据库级删除。 */
    static const std::vector<std::string>& ResetStatements();

    /** @brief 只确保 schema/data 两张版本元数据表存在，不登记任何数据为 ready。 */
    static MySqlStatus EnsureMetadataTables(MySqlClient& client);

    /** @brief 读取已登记的最高 schema 版本；空表返回零。 */
    static MySqlStatus ReadCurrentVersion(MySqlClient& client, std::uint32_t& version);

    /** @brief 初始化缺失表并拒绝高于当前程序的模式版本。 */
    static MySqlSchemaResult Initialize(MySqlClient& client);

    /** @brief 只删除固定 VideoSc 业务表，保留数据库和 schema 版本表。 */
    static MySqlStatus ResetBusinessTables(MySqlClient& client);

    /** @brief 使用 information_schema 核对程序实际引用的表、列和关键索引。 */
    static MySqlStatus ValidateCurrentSchema(MySqlClient& client);
};

}  // namespace videosc::dedup
