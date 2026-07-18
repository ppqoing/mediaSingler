#include "CrashHandler.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <csignal>
#include <crtdbg.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace {

constexpr std::size_t kMaximumPath = 32768;
wchar_t g_crash_directory[kMaximumPath]{};
std::mutex g_directory_mutex;
LONG g_handling_crash = 0;

/**
 * @brief 取得当前模块目录，失败时退回当前目录。
 * @return 可执行文件所在目录。
 */
std::filesystem::path ApplicationDirectory() {
    std::wstring buffer(512, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

/**
 * @brief 将目录复制到致命处理器使用的固定缓冲，避免崩溃时分配路径对象。
 * @param directory 已创建或准备创建的 crash 目录。
 */
void StoreCrashDirectory(const std::filesystem::path& directory) {
    const std::wstring value = std::filesystem::absolute(directory).wstring();
    std::lock_guard<std::mutex> lock(g_directory_mutex);
    wcsncpy_s(g_crash_directory, value.c_str(), _TRUNCATE);
}

/**
 * @brief 生成一次崩溃的共同文件名前缀。
 * @param output 接收绝对前缀的固定缓冲。
 * @param output_count 缓冲 wchar_t 数量。
 * @param source 诊断来源标记。
 */
void BuildCrashPrefix(wchar_t* output, const std::size_t output_count, const wchar_t* source) noexcept {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t directory[kMaximumPath]{};
    wcsncpy_s(directory, g_crash_directory, _TRUNCATE);
    swprintf_s(output,
               output_count,
               L"%s\\VideoScGUI-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu-%s",
               directory,
               time.wYear,
               time.wMonth,
               time.wDay,
               time.wHour,
               time.wMinute,
               time.wSecond,
               GetCurrentProcessId(),
               GetCurrentThreadId(),
               source);
}

/**
 * @brief 在已损坏进程中使用低依赖 Win32 API 写入 UTF-16 元数据和小型转储。
 * @param source 故障来源。
 * @param exception_code SEH/CRT 对应的异常码。
 * @param pointers 可选的当前线程异常上下文。
 */
void WriteFatalDiagnostics(const wchar_t* source,
                           const DWORD exception_code,
                           EXCEPTION_POINTERS* pointers) noexcept {
    if (InterlockedCompareExchange(&g_handling_crash, 1, 0) != 0) return;

    wchar_t prefix[kMaximumPath]{};
    BuildCrashPrefix(prefix, _countof(prefix), source);
    wchar_t dump_path[kMaximumPath]{};
    wchar_t text_path[kMaximumPath]{};
    swprintf_s(dump_path, L"%s.dmp", prefix);
    swprintf_s(text_path, L"%s.txt", prefix);

    const HANDLE text_file = CreateFileW(text_path,
                                         GENERIC_WRITE,
                                         FILE_SHARE_READ,
                                         nullptr,
                                         CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
    if (text_file != INVALID_HANDLE_VALUE) {
        wchar_t content[2048]{};
        const void* address = pointers != nullptr && pointers->ExceptionRecord != nullptr
                                  ? pointers->ExceptionRecord->ExceptionAddress
                                  : nullptr;
        swprintf_s(content,
                   L"VideoScGUI 致命异常\r\n来源: %s\r\n异常码: 0x%08lX\r\n异常地址: %p\r\n进程: %lu\r\n线程: %lu\r\n转储: %s\r\n",
                   source,
                   exception_code,
                   address,
                   GetCurrentProcessId(),
                   GetCurrentThreadId(),
                   dump_path);
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(text_file, &bom, sizeof(bom), &written, nullptr);
        WriteFile(text_file,
                  content,
                  static_cast<DWORD>(wcslen(content) * sizeof(wchar_t)),
                  &written,
                  nullptr);
        FlushFileBuffers(text_file);
        CloseHandle(text_file);
    }

    const HANDLE dump_file = CreateFileW(dump_path,
                                         GENERIC_WRITE,
                                         FILE_SHARE_READ,
                                         nullptr,
                                         CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
    if (dump_file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception_info{};
        exception_info.ThreadId = GetCurrentThreadId();
        exception_info.ExceptionPointers = pointers;
        exception_info.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(),
                          GetCurrentProcessId(),
                          dump_file,
                          type,
                          pointers == nullptr ? nullptr : &exception_info,
                          nullptr,
                          nullptr);
        FlushFileBuffers(dump_file);
        CloseHandle(dump_file);
    }
}

/** @brief Win32 未处理异常最终入口。 */
LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* pointers) noexcept {
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr
                           ? pointers->ExceptionRecord->ExceptionCode
                           : 0;
    WriteFatalDiagnostics(L"unhandled-seh", code, pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

/** @brief C++ terminate 最终入口，不尝试展开已失效的对象图。 */
[[noreturn]] void TerminateHandler() noexcept {
    WriteFatalDiagnostics(L"std-terminate", 0xE06D7363, nullptr);
    TerminateProcess(GetCurrentProcess(), 0xE06D7363);
    __assume(false);
}

/** @brief CRT 纯虚函数调用最终入口。 */
void PureCallHandler() noexcept {
    WriteFatalDiagnostics(L"purecall", 0xC0000005, nullptr);
    TerminateProcess(GetCurrentProcess(), 0xC0000005);
}

/** @brief CRT 非法参数最终入口；不记录表达式以避免泄露调用数据。 */
void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) noexcept {
    WriteFatalDiagnostics(L"invalid-parameter", 0xC0000417, nullptr);
    TerminateProcess(GetCurrentProcess(), 0xC0000417);
}

/** @brief SIGABRT 最终入口。 */
void AbortSignalHandler(int) noexcept {
    WriteFatalDiagnostics(L"sigabrt", 0x40000015, nullptr);
    TerminateProcess(GetCurrentProcess(), 0x40000015);
}

/**
 * @brief 查找指定目录中最后写入的崩溃元数据。
 * @param directory crash 目录。
 * @return 最近的 txt；无记录时为空。
 */
std::filesystem::path FindLatestMetadata(const std::filesystem::path& directory) {
    std::filesystem::path latest;
    std::filesystem::file_time_type latest_time{};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || entry.path().extension() != L".txt") continue;
        const auto write_time = entry.last_write_time(error);
        if (error) {
            error.clear();
            continue;
        }
        if (latest.empty() || write_time > latest_time) {
            latest = entry.path();
            latest_time = write_time;
        }
    }
    return latest;
}

}  // namespace

bool CrashHandler::Install(const std::filesystem::path& logging_directory) noexcept {
    try {
        SetLogDirectory(logging_directory);
        SetUnhandledExceptionFilter(HandleUnhandledException);
        std::set_terminate(TerminateHandler);
        _set_purecall_handler(PureCallHandler);
        _set_invalid_parameter_handler(InvalidParameterHandler);
        std::signal(SIGABRT, AbortSignalHandler);
        return true;
    } catch (...) {
        return false;
    }
}

void CrashHandler::SetLogDirectory(const std::filesystem::path& logging_directory) noexcept {
    try {
        std::filesystem::path crash_directory =
            (logging_directory.empty() ? ApplicationDirectory() / L"logs" : logging_directory) / L"crash";
        std::error_code error;
        std::filesystem::create_directories(crash_directory, error);
        if (error) {
            wchar_t temporary[kMaximumPath]{};
            const DWORD length = GetTempPathW(static_cast<DWORD>(_countof(temporary)), temporary);
            crash_directory = length > 0 && length < _countof(temporary)
                                  ? std::filesystem::path(temporary) / L"VideoScGUI" / L"crash"
                                  : ApplicationDirectory() / L"logs" / L"crash";
            error.clear();
            std::filesystem::create_directories(crash_directory, error);
        }
        StoreCrashDirectory(crash_directory);
    } catch (...) {
        try {
            StoreCrashDirectory(ApplicationDirectory() / L"logs" / L"crash");
        } catch (...) {
            g_crash_directory[0] = L'\0';
        }
    }
}

bool CrashHandler::LaunchExternalReporter() noexcept {
    try {
        if (IsDebuggerPresent()) return false;
        const std::filesystem::path reporter = ApplicationDirectory() / L"VideoScCrashReporter.exe";
        if (!std::filesystem::exists(reporter)) return false;
        const std::filesystem::path directory = CrashDirectory();
        std::wstring command = L"\"" + reporter.wstring() + L"\" --pid " +
                               std::to_wstring(GetCurrentProcessId()) + L" --directory \"" +
                               directory.wstring() + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(reporter.c_str(),
                            mutable_command.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_NO_WINDOW,
                            nullptr,
                            ApplicationDirectory().c_str(),
                            &startup,
                            &process)) {
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    } catch (...) {
        return false;
    }
}

std::filesystem::path CrashHandler::CrashDirectory() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_directory_mutex);
        return std::filesystem::path(g_crash_directory);
    } catch (...) {
        return {};
    }
}

std::filesystem::path CrashHandler::LatestUnreviewedCrash() noexcept {
    try {
        const std::filesystem::path directory = CrashDirectory();
        const std::filesystem::path latest = FindLatestMetadata(directory);
        if (latest.empty()) return {};
        std::ifstream marker(directory / L"crash-reviewed.marker", std::ios::binary);
        std::string reviewed((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        return reviewed == latest.filename().u8string() ? std::filesystem::path{} : latest;
    } catch (...) {
        return {};
    }
}

void CrashHandler::MarkLatestCrashReviewed() noexcept {
    try {
        const std::filesystem::path directory = CrashDirectory();
        const std::filesystem::path latest = FindLatestMetadata(directory);
        if (latest.empty()) return;
        std::ofstream marker(directory / L"crash-reviewed.marker", std::ios::binary | std::ios::trunc);
        marker << latest.filename().u8string();
    } catch (...) {
        // 标记失败不影响崩溃文件本身。
    }
}
