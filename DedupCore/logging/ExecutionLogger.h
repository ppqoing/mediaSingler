#pragma once

#include "../config/AppConfig.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace videosc::dedup {

/** @brief 一条任务生命周期或阶段摘要，不包含逐文件成功事件。 */
struct ExecutionEventRecord {
    std::uint64_t task_id = 0;
    std::string task;
    std::string event;
    std::string stage;
    std::string message;
    std::uint64_t processed_items = 0;
    std::uint64_t total_items = 0;
};

/** @brief 一条可追溯到具体任务、阶段和文件的执行失败记录。 */
struct ExecutionFailureRecord {
    std::uint64_t task_id = 0;
    std::uint64_t path_id = 0;
    std::string task;
    std::string stage;
    std::string operation;
    std::filesystem::path path;
    std::wstring storage_target_key;
    std::string media_kind;
    std::string status;
    std::uint32_t status_code = 0;
    std::uint32_t native_error = 0;
    std::uint64_t failed_offset = 0;
    std::uint64_t bytes_read = 0;
    std::string detail;
};

/**
 * @brief 独立滚动执行日志，分别维护常规轨迹和失败记录。
 *
 * 每条 UTF-8 易读文本写入后立即刷盘；调用方只有在 WriteFailure 成功后才能增加 GUI 失败计数。
 */
class ExecutionLogger final {
public:
    /** @param config 包含独立执行目录和滚动参数的配置快照。 */
    explicit ExecutionLogger(LoggingConfig config);

    /** @brief 创建目录并验证两个日志文件均可追加。 */
    bool EnsureWritable(std::string& error);

    /** @brief 追加并刷盘一条任务轨迹。 */
    bool WriteEvent(const ExecutionEventRecord& record, std::string& error);

    /** @brief 追加并刷盘一条失败记录。 */
    bool WriteFailure(const ExecutionFailureRecord& record, std::string& error);

private:
    bool RotateIfNeeded(const std::filesystem::path& active, std::string& error);
    bool AppendLine(const std::filesystem::path& file, const std::string& line, std::string& error);

    LoggingConfig config_;
};

}  // namespace videosc::dedup
