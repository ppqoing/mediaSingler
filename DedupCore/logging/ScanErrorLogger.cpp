#include "ScanErrorLogger.h"
#include "RuntimeLogFeed.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace videosc::dedup {
namespace {

/** @brief 严格 UTF-16 到 UTF-8；非法路径用空字符串避免日志写入崩溃。 */
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        return {};
    }
    return result;
}

/** @brief 文件哈希状态的稳定日志枚举名。 */
const char* StatusName(const FileHashStatus status) {
    switch (status) {
        case FileHashStatus::Succeeded: return "succeeded";
        case FileHashStatus::InvalidArgument: return "invalid_argument";
        case FileHashStatus::OpenFailed: return "open_failed";
        case FileHashStatus::ReadFailed: return "read_failed";
        case FileHashStatus::ReadTimeout: return "read_timeout";
        case FileHashStatus::Cancelled: return "cancelled";
        case FileHashStatus::FileChanged: return "file_changed";
        case FileHashStatus::CryptoFailed: return "crypto_failed";
        case FileHashStatus::UnexpectedFailure: return "unexpected_failure";
    }
    return "unknown";
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
 * @brief 将扫描读取错误镜像到实时日志源。
 * @param record 扫描路径记录。
 * @param result 文件读取失败状态。
 * @param utc_ms 与文件日志一致的 UTC 时间。
 * @param file_written 扫描错误日志是否落盘成功。
 * @param write_error 文件写入错误。
 */
void MirrorScanFailure(const FilePathRecord& record,
                       const FileHashResult& result,
                       const std::int64_t utc_ms,
                       const bool file_written,
                       const std::string& write_error) noexcept {
    try {
        RuntimeLogEntry entry;
        entry.utc_ms = utc_ms;
        entry.severity = RuntimeLogSeverity::Error;
        entry.task_id = record.scan_id;
        entry.task = "scan";
        entry.stage = "sha512_read";
        entry.operation = StatusName(result.status);
        entry.native_error = result.system_error;
        entry.subject = WideToUtf8(record.path.wstring());
        entry.message = "文件读取失败，偏移=" + std::to_string(result.failed_offset) +
                        "，已读字节=" + std::to_string(result.bytes_read);
        RuntimeLogFeed::Publish(std::move(entry));
        if (!file_written) {
            RuntimeLogEntry failure;
            failure.utc_ms = utc_ms;
            failure.severity = RuntimeLogSeverity::Error;
            failure.task_id = record.scan_id;
            failure.task = "logging";
            failure.stage = "scan_errors_log";
            failure.operation = "write_failure";
            failure.message = "扫描错误日志落盘失败：" + write_error;
            RuntimeLogFeed::Publish(std::move(failure));
        }
    } catch (...) {
        // 实时镜像失败不能改变扫描错误处置。
    }
}

}  // namespace

ScanErrorLogger::ScanErrorLogger(LoggingConfig config) : config_(std::move(config)) {}

bool ScanErrorLogger::EnsureWritable(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code filesystemError;
    std::filesystem::create_directories(config_.directory, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    std::ofstream stream(config_.directory / L"scan-errors.log", std::ios::binary | std::ios::app);
    if (!stream) {
        error = "cannot_open_scan_error_log";
        return false;
    }
    stream.flush();
    error.clear();
    return static_cast<bool>(stream);
}

bool ScanErrorLogger::RotateIfNeeded(std::string& error) {
    const std::filesystem::path active = config_.directory / L"scan-errors.log";
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
        const std::filesystem::path destination =
            config_.directory / (L"scan-errors.log." + std::to_wstring(index));
        const std::filesystem::path source = index == 1
                                                 ? active
                                                 : config_.directory /
                                                       (L"scan-errors.log." + std::to_wstring(index - 1));
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

bool ScanErrorLogger::Write(const FilePathRecord& record,
                            const FileHashResult& result,
                            std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code filesystemError;
    std::filesystem::create_directories(config_.directory, filesystemError);
    if (filesystemError) {
        error = filesystemError.message();
        return false;
    }
    if (!RotateIfNeeded(error)) return false;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    std::ostringstream text;
    text << '[' << UtcText(now) << "] [扫描读取失败]"
         << " 扫描ID=" << record.scan_id
         << " 路径ID=" << record.path_id
         << " 状态=" << StatusName(result.status)
         << " 系统错误=" << result.system_error
         << " 失败偏移=" << result.failed_offset
         << " 已读字节=" << result.bytes_read
         << " 文件大小=" << record.size_bytes
         << " 磁盘=" << OneLine(WideToUtf8(record.storage_target_key))
         << " 路径=" << OneLine(WideToUtf8(record.path.wstring())) << '\n';
    const std::string line = text.str();
    std::ofstream stream(config_.directory / L"scan-errors.log", std::ios::binary | std::ios::app);
    if (!stream) {
        error = "cannot_append_scan_error_log";
        MirrorScanFailure(record, result, now, false, error);
        return false;
    }
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    if (!stream) {
        error = "cannot_flush_scan_error_log";
        MirrorScanFailure(record, result, now, false, error);
        return false;
    }
    error.clear();
    MirrorScanFailure(record, result, now, true, error);
    return true;
}

}  // namespace videosc::dedup
