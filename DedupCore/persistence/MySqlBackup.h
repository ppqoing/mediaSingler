#pragma once

#include "../config/AppConfig.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace videosc::dedup {

/** @brief mysqldump 备份结果；失败时 incomplete 文件会被删除。 */
struct MySqlBackupResult {
    bool succeeded = false;
    bool cancelled = false;
    bool timed_out = false;
    std::uint32_t process_error = 0;
    std::uint32_t exit_code = 0;
    std::filesystem::path backup_file;
    std::string message;
};

/**
 * @brief 使用 CreateProcessW 调用 mysqldump，并把密码仅放入子进程环境块。
 *
 * 命令行不包含密码，也不通过 shell 重定向；输出由 mysqldump --result-file 直接写入配置目录。
 */
class MySqlBackup final {
public:
    using CancellationCallback = std::function<bool()>;

    /**
     * @brief 为当前 DatabaseConfig 创建带时间戳的完整逻辑备份。
     * @param config 数据库和 mysqldump 配置快照。
     * @param should_cancel 定期检查的关闭请求；取消时终止子进程并删除未完成文件。
     */
    static MySqlBackupResult Create(const DatabaseConfig& config,
                                    const CancellationCallback& should_cancel = {});
};

}  // namespace videosc::dedup
