#pragma once

#include "../config/AppConfig.h"
#include "../models/CoreModels.h"
#include "../scheduling/FileHasher.h"

#include <mutex>
#include <string>

namespace videosc::dedup {

/**
 * @brief 线程安全的文件读取失败滚动日志。
 *
 * 坏块、读取错误、无进度超时和文件变化均写入 scan-errors.log；扫描继续处理后续文件，
 * 日志不写数据库，也不包含 MySQL 凭据。
 */
class ScanErrorLogger final {
public:
    explicit ScanErrorLogger(LoggingConfig config);

    /** @brief 创建日志目录并验证 scan-errors.log 可追加。 */
    bool EnsureWritable(std::string& error);

    /** @brief 写入并刷盘一条完整文件读取失败记录。 */
    bool Write(const FilePathRecord& record,
               const FileHashResult& result,
               std::string& error);

private:
    bool RotateIfNeeded(std::string& error);

    LoggingConfig config_;
    std::mutex mutex_;
};

}  // namespace videosc::dedup
