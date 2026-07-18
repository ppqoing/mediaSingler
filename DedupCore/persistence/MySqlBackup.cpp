#include "MySqlBackup.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <vector>

namespace videosc::dedup {
namespace {

/** @brief Windows CreateProcess 命令行参数引用规则。 */
std::wstring QuoteArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

/** @brief 当前本地时间生成文件名安全时间戳。 */
std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[64]{};
    swprintf_s(value,
               L"%04u%02u%02u-%02u%02u%02u-%03u",
               time.wYear,
               time.wMonth,
               time.wDay,
               time.wHour,
               time.wMinute,
               time.wSecond,
               time.wMilliseconds);
    return value;
}

/** @brief MySQL TLS 枚举转换为 mysqldump --ssl-mode。 */
const wchar_t* TlsModeName(const MySqlTlsMode mode) {
    switch (mode) {
        case MySqlTlsMode::Disabled: return L"DISABLED";
        case MySqlTlsMode::Preferred: return L"PREFERRED";
        case MySqlTlsMode::Required: return L"REQUIRED";
        case MySqlTlsMode::VerifyCa: return L"VERIFY_CA";
        case MySqlTlsMode::VerifyIdentity: return L"VERIFY_IDENTITY";
    }
    return L"PREFERRED";
}

/** @brief 不区分大小写判断环境项是否为 MYSQL_PWD。 */
bool IsMysqlPasswordEntry(const std::wstring& entry) {
    constexpr wchar_t prefix[] = L"MYSQL_PWD=";
    if (entry.size() < std::size(prefix) - 1) return false;
    for (std::size_t index = 0; index < std::size(prefix) - 1; ++index) {
        if (std::towupper(entry[index]) != prefix[index]) return false;
    }
    return true;
}

/** @brief 复制当前环境并仅替换子进程 MYSQL_PWD。 */
bool BuildEnvironment(const std::wstring& password,
                      std::vector<wchar_t>& environment,
                      std::string& error) {
    LPWCH source = GetEnvironmentStringsW();
    if (!source) {
        error = "GetEnvironmentStringsW failed";
        return false;
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* current = source; *current != L'\0'; current += std::wcslen(current) + 1) {
        std::wstring entry(current);
        if (!IsMysqlPasswordEntry(entry)) entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(source);
    entries.push_back(L"MYSQL_PWD=" + password);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::size_t characters = 1;
    for (const std::wstring& entry : entries) characters += entry.size() + 1;
    environment.assign(characters, L'\0');
    std::size_t offset = 0;
    for (const std::wstring& entry : entries) {
        std::copy(entry.begin(), entry.end(), environment.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += entry.size() + 1;
    }
    error.clear();
    return true;
}

/** @brief 追加 --name=value 参数并应用 Windows 引用。 */
void AppendOption(std::wstring& command, const wchar_t* name, const std::wstring& value) {
    command.push_back(L' ');
    command += name;
    command += QuoteArgument(value);
}

}  // namespace

MySqlBackupResult MySqlBackup::Create(const DatabaseConfig& config,
                                      const CancellationCallback& should_cancel) {
    MySqlBackupResult result;
    if (config.mysqldump_path.empty() || config.backup_directory.empty() || config.database_name.empty()) {
        result.message = "mysqldump path, backup directory and database name are required";
        return result;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(config.backup_directory, filesystemError);
    if (filesystemError) {
        result.message = filesystemError.message();
        return result;
    }
    result.backup_file = config.backup_directory /
                         (config.database_name + L"-" + Timestamp() + L".sql");

    std::wstring command = QuoteArgument(config.mysqldump_path.wstring());
    AppendOption(command, L"--host=", config.host);
    AppendOption(command, L"--port=", std::to_wstring(config.port));
    AppendOption(command, L"--user=", config.user_name);
    AppendOption(command, L"--ssl-mode=", TlsModeName(config.tls_mode));
    if (!config.tls_ca_path.empty()) AppendOption(command, L"--ssl-ca=", config.tls_ca_path.wstring());
    if (!config.tls_certificate_path.empty()) {
        AppendOption(command, L"--ssl-cert=", config.tls_certificate_path.wstring());
    }
    if (!config.tls_private_key_path.empty()) {
        AppendOption(command, L"--ssl-key=", config.tls_private_key_path.wstring());
    }
    command += L" --default-character-set=utf8mb4 --single-transaction --routines --events --triggers";
    AppendOption(command, L"--result-file=", result.backup_file.wstring());
    command += L" --databases ";
    command += QuoteArgument(config.database_name);

    std::vector<wchar_t> environment;
    if (!BuildEnvironment(config.password, environment, result.message)) return result;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommand.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        environment.data(),
                                        nullptr,
                                        &startup,
                                        &process);
    SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
    if (!created) {
        result.process_error = GetLastError();
        result.message = "CreateProcessW failed";
        std::filesystem::remove(result.backup_file, filesystemError);
        return result;
    }
    const std::uint64_t timeout64 = static_cast<std::uint64_t>(config.command_timeout_seconds) * 1000ULL;
    const DWORD timeout = static_cast<DWORD>((std::min<std::uint64_t>)(timeout64, MAXDWORD - 1ULL));
    const ULONGLONG started = GetTickCount64();
    DWORD waited = WAIT_TIMEOUT;
    while (true) {
        waited = WaitForSingleObject(process.hProcess, 0);
        if (waited != WAIT_TIMEOUT) break;
        if (should_cancel && should_cancel()) {
            result.cancelled = true;
            result.message = "mysqldump cancelled";
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeout) {
            result.timed_out = true;
            result.message = "mysqldump timed out";
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        const DWORD remaining = static_cast<DWORD>(timeout - elapsed);
        waited = WaitForSingleObject(process.hProcess, (std::min)(remaining, 100UL));
        if (waited != WAIT_TIMEOUT) break;
    }
    if (!result.cancelled && !result.timed_out && waited != WAIT_OBJECT_0) {
        result.process_error = GetLastError();
        result.message = "WaitForSingleObject failed";
    } else if (!result.cancelled && !result.timed_out) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
            result.process_error = GetLastError();
            result.message = "GetExitCodeProcess failed";
        } else {
            result.exit_code = exitCode;
            result.succeeded = exitCode == 0;
            result.message = result.succeeded ? "ok" : "mysqldump returned a non-zero exit code";
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const bool validFile = result.succeeded && std::filesystem::exists(result.backup_file, filesystemError) &&
                           !filesystemError && std::filesystem::file_size(result.backup_file, filesystemError) > 0 &&
                           !filesystemError;
    if (!validFile) {
        result.succeeded = false;
        if (result.message == "ok") result.message = "mysqldump did not create a non-empty backup";
        filesystemError.clear();
        std::filesystem::remove(result.backup_file, filesystemError);
    }
    return result;
}

}  // namespace videosc::dedup
