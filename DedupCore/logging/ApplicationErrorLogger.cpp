#include "ApplicationErrorLogger.h"
#include "RuntimeLogFeed.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace videosc::dedup {
namespace {

std::mutex g_logger_mutex;
LoggingConfig g_logging_config;
bool g_configured = false;
std::atomic_uint64_t g_error_sequence{0};

/**
 * @brief 生成进程内唯一且便于人工关联的异常 ID。
 * @param utc_ms 当前 UTC 毫秒。
 * @return 由时间、进程、线程和序号组成的 ASCII ID。
 */
std::string MakeErrorId(const std::int64_t utc_ms) {
    std::ostringstream stream;
    stream << "ERR-" << utc_ms << '-' << GetCurrentProcessId() << '-' << GetCurrentThreadId() << '-'
           << g_error_sequence.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

/**
 * @brief 在正常运行上下文采集有限调用栈地址，不执行符号解析。
 * @return 十六进制地址列表；采集失败时为空。
 */
std::vector<std::string> CaptureStackAddresses() {
    std::array<void*, 32> frames{};
    const USHORT count = CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::vector<std::string> result;
    result.reserve(count);
    for (USHORT index = 0; index < count; ++index) {
        std::ostringstream stream;
        stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(frames[index]);
        result.push_back(stream.str());
    }
    return result;
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
 * @brief 在持有记录器锁时滚动 application-errors.log。
 * @param config 当前日志配置。
 * @return 滚动成功或无需滚动返回 true。
 */
bool RotateIfNeeded(const LoggingConfig& config) {
    const std::filesystem::path active = config.directory / L"application-errors.log";
    std::error_code error;
    if (!std::filesystem::exists(active, error)) return !error;
    const std::uint64_t size = std::filesystem::file_size(active, error);
    if (error) return false;
    const std::uint64_t maximum = static_cast<std::uint64_t>(config.rotate_file_mib) * 1024ULL * 1024ULL;
    if (maximum == 0 || size < maximum) return true;

    for (std::uint32_t index = config.rotate_count; index > 0; --index) {
        const std::filesystem::path destination =
            config.directory / (L"application-errors.log." + std::to_wstring(index));
        const std::filesystem::path source =
            index == 1 ? active
                       : config.directory / (L"application-errors.log." + std::to_wstring(index - 1));
        if (!std::filesystem::exists(source, error)) {
            error.clear();
            continue;
        }
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(source, destination, error);
        if (error) return false;
    }
    return true;
}

/**
 * @brief 将应用异常镜像到实时日志源，不参与文件记录器锁和递归错误处理。
 * @param record 应用异常记录。
 * @param error_id 可供用户关联文件日志的异常 ID。
 * @param utc_ms 异常 UTC 时间。
 */
void MirrorApplicationError(const ApplicationErrorRecord& record,
                            const std::string& error_id,
                            const std::int64_t utc_ms) noexcept {
    try {
        RuntimeLogEntry entry;
        entry.utc_ms = utc_ms;
        entry.severity = record.severity == "info"
                             ? RuntimeLogSeverity::Info
                             : (record.severity == "warning" ? RuntimeLogSeverity::Warning
                                                               : RuntimeLogSeverity::Error);
        entry.task = "application";
        entry.stage = record.category;
        entry.operation = record.module + "/" + record.operation;
        entry.native_error = record.native_error;
        entry.subject = error_id;
        entry.message = record.message;
        if (!record.context.empty()) entry.message += "；" + record.context;
        RuntimeLogFeed::Publish(std::move(entry));
    } catch (...) {
        // 应用异常实时镜像不得递归调用异常记录器。
    }
}

}  // namespace

void ApplicationErrorLogger::Configure(const LoggingConfig& config) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        g_logging_config = config;
        g_configured = !config.directory.empty();
        if (g_configured) {
            std::error_code error;
            std::filesystem::create_directories(g_logging_config.directory, error);
        }
    } catch (...) {
        // 异常记录器必须保持 noexcept，配置失败由后续 Write 静默降级。
    }
}

std::string ApplicationErrorLogger::Write(const ApplicationErrorRecord& record) noexcept {
    try {
        const std::int64_t utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
        const std::string error_id = MakeErrorId(utc_ms);
        MirrorApplicationError(record, error_id, utc_ms);
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        if (!g_configured || g_logging_config.directory.empty()) return error_id;

        std::error_code filesystem_error;
        std::filesystem::create_directories(g_logging_config.directory, filesystem_error);
        if (filesystem_error || !RotateIfNeeded(g_logging_config)) return error_id;

        std::ostringstream text;
        text << '[' << UtcText(utc_ms) << "] [应用异常]"
             << " 异常ID=" << error_id
             << " 进程=" << GetCurrentProcessId()
             << " 线程=" << GetCurrentThreadId()
             << " 级别=" << OneLine(record.severity)
             << " 类别=" << OneLine(record.category)
             << " 模块=" << OneLine(record.module)
             << " 操作=" << OneLine(record.operation)
             << " 异常类型=" << OneLine(record.exception_type)
             << " 系统错误=" << record.native_error
             << " 说明=" << OneLine(record.message)
             << " 上下文=" << OneLine(record.context)
             << " 调用栈=";
        const std::vector<std::string> stack = CaptureStackAddresses();
        for (std::size_t index = 0; index < stack.size(); ++index) {
            if (index != 0) text << ',';
            text << stack[index];
        }
        const std::string serialized = text.str() + "\n";
        std::ofstream stream(g_logging_config.directory / L"application-errors.log",
                             std::ios::binary | std::ios::app);
        if (!stream) return error_id;
        stream.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        stream.flush();
        return error_id;
    } catch (...) {
        // 日志失败不能覆盖原始业务异常。
        return {};
    }
}

std::filesystem::path ApplicationErrorLogger::Directory() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        return g_configured ? g_logging_config.directory : std::filesystem::path{};
    } catch (...) {
        return {};
    }
}

}  // namespace videosc::dedup
