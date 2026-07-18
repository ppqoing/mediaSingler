#include "AppConfig.h"

#include <Windows.h>

#include <stdexcept>
#include <vector>

namespace videosc::dedup {

AppConfig AppConfig::CreateDefault(const std::filesystem::path& install_directory) {
    AppConfig config;
    config.thumbnails.root_directory = install_directory / L"thumbnails";
    config.rocksdb.directory = install_directory / L"data" / L"rocksdb";
    config.logging.directory = install_directory / L"logs";
    config.logging.execution_directory = install_directory / L"execution-logs";
    config.database.backup_directory = install_directory / L"backups";
    config.discovery.method = DiscoveryMethod::Everything;
    config.discovery.everything_dll_path = install_directory / L"third_party" / L"everything_sdk" / L"Everything64.dll";
    config.discovery.everything_exe_path = install_directory / L"third_party" / L"es" / L"Everything.exe";
    return config;
}

std::filesystem::path GetApplicationDirectory() {
    std::vector<wchar_t> buffer(512);
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768) {
            throw std::runtime_error("Executable path is too long");
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace videosc::dedup
