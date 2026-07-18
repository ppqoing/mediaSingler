#include "ExecutionLogger.h"
#include "RuntimeLogFeed.h"

#include <Windows.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace videosc::dedup {
namespace {

/** @brief 严格转换日志文本；非法 UTF-16 返回空值，避免日志路径导致任务崩溃。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                           static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

/** @return 当前 UTC Unix 毫秒。 */
std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::mutex& ProcessLogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string OneLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
    return value;
}

std::string UtcText(const std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm value{};
    if (gmtime_s(&value, &seconds) != 0) return std::to_string(milliseconds);
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << (milliseconds % 1000) << " UTC";
    return output.str();
}

/**
 * @brief 将执行轨迹镜像到有界实时日志源。
 * @param record 原执行轨迹。
 * @param utc_ms 与文件日志一致的 UTC 时间。
 * @param file_written 文件日志是否写入成功。
 * @param write_error 文件写入错误。
 */
void MirrorExecutionEvent(const ExecutionEventRecord& record,
                          const std::int64_t utc_ms,
                          const bool file_written,
                          const std::string& write_error) noexcept {
    try {
        RuntimeLogEntry entry;
        entry.utc_ms = utc_ms;
        entry.severity = record.event == "failed"
                             ? RuntimeLogSeverity::Error
                             : (record.event == "cancelled" ||
                                        record.event == "completed_with_warnings"
                                    ? RuntimeLogSeverity::Warning
                                    : RuntimeLogSeverity::Info);
        entry.task_id = record.task_id;
        entry.task = record.task;
        entry.stage = record.stage;
        entry.operation = record.event;
        entry.message = record.message;
        RuntimeLogFeed::Publish(std::move(entry));
        if (!file_written) {
            RuntimeLogEntry failure;
            failure.utc_ms = utc_ms;
            failure.severity = RuntimeLogSeverity::Error;
            failure.task_id = record.task_id;
            failure.task = "logging";
            failure.stage = "execution_log";
            failure.operation = "write_event";
            failure.message = "执行轨迹落盘失败：" + write_error;
            RuntimeLogFeed::Publish(std::move(failure));
        }
    } catch (...) {
        // 实时镜像失败不能改变文件日志写入结果。
    }
}

/**
 * @brief 将执行失败记录镜像到有界实时日志源。
 * @param record 原失败记录。
 * @param utc_ms 与文件日志一致的 UTC 时间。
 * @param file_written 文件日志是否写入成功。
 * @param write_error 文件写入错误。
 */
void MirrorExecutionFailure(const ExecutionFailureRecord& record,
                            const std::int64_t utc_ms,
                            const bool file_written,
                            const std::string& write_error) noexcept {
    try {
        RuntimeLogEntry entry;
        entry.utc_ms = utc_ms;
        entry.severity = RuntimeLogSeverity::Error;
        entry.task_id = record.task_id;
        entry.task = record.task;
        entry.stage = record.stage;
        entry.operation = record.operation;
        entry.status_code = record.status_code;
        entry.native_error = record.native_error;
        entry.subject = WideToUtf8(record.path.wstring());
        if (entry.subject.empty()) entry.subject = WideToUtf8(record.storage_target_key);
        entry.message = record.detail;
        RuntimeLogFeed::Publish(std::move(entry));
        if (!file_written) {
            RuntimeLogEntry failure;
            failure.utc_ms = utc_ms;
            failure.severity = RuntimeLogSeverity::Error;
            failure.task_id = record.task_id;
            failure.task = "logging";
            failure.stage = "execution_failures_log";
            failure.operation = "write_failure";
            failure.message = "执行失败记录落盘失败：" + write_error;
            RuntimeLogFeed::Publish(std::move(failure));
        }
    } catch (...) {
        // 实时镜像失败不能覆盖原始业务错误。
    }
}

}  // namespace

ExecutionLogger::ExecutionLogger(LoggingConfig config) : config_(std::move(config)) {}

bool ExecutionLogger::EnsureWritable(std::string& error) {
    std::lock_guard<std::mutex> lock(ProcessLogMutex());
    std::error_code filesystemError;
    std::filesystem::create_directories(config_.execution_directory, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    if (config_.retention_days != 0) {
        const auto cutoff = std::filesystem::file_time_type::clock::now() -
                            std::chrono::hours(static_cast<std::int64_t>(24) * config_.retention_days);
        for (const auto& entry : std::filesystem::directory_iterator(config_.execution_directory,
                                                                      filesystemError)) {
            if (filesystemError) break;
            if (!entry.is_regular_file(filesystemError)) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"execution.log", 0) != 0 &&
                name.rfind(L"execution-failures.log", 0) != 0) {
                continue;
            }
            if (entry.last_write_time(filesystemError) < cutoff) {
                std::filesystem::remove(entry.path(), filesystemError);
            }
            filesystemError.clear();
        }
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
    }
    for (const wchar_t* name : {L"execution.log", L"execution-failures.log"}) {
        std::ofstream stream(config_.execution_directory / name, std::ios::binary | std::ios::app);
        if (!stream) {
            error = "cannot_open_execution_log";
            return false;
        }
        stream.flush();
        if (!stream) {
            error = "cannot_flush_execution_log";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ExecutionLogger::RotateIfNeeded(const std::filesystem::path& active, std::string& error) {
    std::error_code filesystemError;
    const std::uint64_t size = std::filesystem::exists(active, filesystemError)
                                   ? std::filesystem::file_size(active, filesystemError)
                                   : 0;
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    if (size < static_cast<std::uint64_t>(config_.rotate_file_mib) * 1024ULL * 1024ULL) return true;
    for (std::uint32_t index = config_.rotate_count; index > 0; --index) {
        const std::filesystem::path destination = active.wstring() + L"." + std::to_wstring(index);
        const std::filesystem::path source = index == 1
                                                 ? active
                                                 : std::filesystem::path(active.wstring() + L"." +
                                                                         std::to_wstring(index - 1));
        if (!std::filesystem::exists(source, filesystemError)) {
            filesystemError.clear();
            continue;
        }
        std::filesystem::remove(destination, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(source, destination, filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
    }
    return true;
}

bool ExecutionLogger::AppendLine(const std::filesystem::path& file,
                                 const std::string& line,
                                 std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(config_.execution_directory, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    if (!RotateIfNeeded(file, error)) return false;
    std::ofstream stream(file, std::ios::binary | std::ios::app);
    if (!stream) {
        error = "cannot_append_execution_log";
        return false;
    }
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.put('\n');
    stream.flush();
    if (!stream) {
        error = "cannot_flush_execution_log";
        return false;
    }
    error.clear();
    return true;
}

bool ExecutionLogger::WriteEvent(const ExecutionEventRecord& record, std::string& error) {
    std::lock_guard<std::mutex> lock(ProcessLogMutex());
    const std::int64_t utcMilliseconds = UtcNowMilliseconds();
    std::ostringstream line;
    line << '[' << UtcText(utcMilliseconds) << "] [执行]"
         << " 任务ID=" << record.task_id
         << " 任务=" << OneLine(record.task)
         << " 事件=" << OneLine(record.event)
         << " 阶段=" << OneLine(record.stage)
         << " 进度=" << record.processed_items << '/' << record.total_items
         << " 说明=" << OneLine(record.message);
    const bool written = AppendLine(config_.execution_directory / L"execution.log", line.str(), error);
    MirrorExecutionEvent(record, utcMilliseconds, written, error);
    return written;
}

bool ExecutionLogger::WriteFailure(const ExecutionFailureRecord& record, std::string& error) {
    std::lock_guard<std::mutex> lock(ProcessLogMutex());
    const std::int64_t utcMilliseconds = UtcNowMilliseconds();
    std::ostringstream line;
    line << '[' << UtcText(utcMilliseconds) << "] [失败]"
         << " 任务ID=" << record.task_id
         << " 路径ID=" << record.path_id
         << " 任务=" << OneLine(record.task)
         << " 阶段=" << OneLine(record.stage)
         << " 操作=" << OneLine(record.operation)
         << " 状态=" << OneLine(record.status)
         << " 状态码=" << record.status_code
         << " 系统错误=" << record.native_error
         << " 失败偏移=" << record.failed_offset
         << " 已读字节=" << record.bytes_read
         << " 介质=" << OneLine(record.media_kind)
         << " 磁盘=" << OneLine(WideToUtf8(record.storage_target_key))
         << " 路径=" << OneLine(WideToUtf8(record.path.wstring()))
         << " 原因=" << OneLine(record.detail);
    const bool written =
        AppendLine(config_.execution_directory / L"execution-failures.log", line.str(), error);
    MirrorExecutionFailure(record, utcMilliseconds, written, error);
    return written;
}

}  // namespace videosc::dedup
