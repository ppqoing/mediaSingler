#include "EverythingFileDiscovery.h"

#include "DiskInfo.h"
#include "NativeFileDiscovery.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace videosc::dedup {
namespace {

// ---- 常量 ----
constexpr const wchar_t* kIpcWndClass = L"EVERYTHING_TASKBAR_NOTIFICATION";
constexpr unsigned int kMaxPathBuf = 32768;      // Everything 全路径 API 支持的最大长度

// Everything SDK request flags（SDK 文档常量）
constexpr unsigned int EVERYTHING_REQUEST_FULL_PATH = 0x00000004;
constexpr unsigned int EVERYTHING_REQUEST_SIZE = 0x00000010;
constexpr unsigned int EVERYTHING_REQUEST_DATE_MODIFIED = 0x00000020;
constexpr unsigned int EVERYTHING_REQUEST_DATE_CREATED = 0x00000040;

// ---- SDK 函数指针 typedef（Ev 前缀避免冲突） ----
typedef void  (WINAPI* EvSetSearch_t)(const wchar_t*);
typedef void  (WINAPI* EvSetMatchPath_t)(int);
typedef void  (WINAPI* EvSetMax_t)(unsigned int);
typedef void  (WINAPI* EvSetOffset_t)(unsigned int);
typedef void  (WINAPI* EvSetRequestFlags_t)(unsigned int);
typedef int   (WINAPI* EvQuery_t)(int);
typedef unsigned int (WINAPI* EvGetNumResults_t)(void);
typedef DWORD (WINAPI* EvGetTotResults_t)(void);
typedef int   (WINAPI* EvIsFolderResult_t)(unsigned int);
typedef unsigned int (WINAPI* EvGetResultFullPathName_t)(unsigned int, wchar_t*, unsigned int);
typedef void  (WINAPI* EvGetResultSize_t)(unsigned int, LARGE_INTEGER*);
typedef void  (WINAPI* EvGetResultDateCreated_t)(unsigned int, FILETIME*);
typedef void  (WINAPI* EvGetResultDateModified_t)(unsigned int, FILETIME*);
typedef void  (WINAPI* EvReset_t)(void);
typedef unsigned int (WINAPI* EvGetLastError_t)(void);
typedef int   (WINAPI* EvIsDBLoaded_t)(void);
typedef DWORD (WINAPI* EvGetBuildNumber_t)(void);
typedef DWORD (WINAPI* EvGetMajorVersion_t)(void);

// ---- 辅助函数（从 NativeFileDiscovery.cpp 复制，匿名命名空间内部副本） ----

/** @brief 严格 UTF-16 到 UTF-8。 */
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

/** @brief 严格 UTF-8 到 UTF-16。 */
std::wstring Utf8ToWide(const char* value) {
    if (value == nullptr || value[0] == '\0') return {};
    const int sourceLength = static_cast<int>(std::strlen(value));
    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value,
                                           sourceLength,
                                           nullptr,
                                           0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value,
                            sourceLength,
                            result.data(),
                            length) != length) {
        return {};
    }
    return result;
}

/** @brief Windows FILETIME 转 Unix UTC 毫秒。 */
std::int64_t FileTimeToUnixMilliseconds(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000ULL);
}

/** @brief 获取不带通配符的绝对规范显示路径。 */
std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return path;
    buffer.resize(written);
    return std::filesystem::path(buffer).lexically_normal();
}

/** @brief Windows 路径键使用统一分隔符和 invariant 小写。 */
std::wstring NormalizePathKey(const std::filesystem::path& path) {
    std::wstring value = AbsolutePath(path).wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (!value.empty()) {
        const int required = LCMapStringEx(LOCALE_NAME_INVARIANT,
                                           LCMAP_LOWERCASE,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr,
                                           0);
        if (required > 0) {
            std::wstring lowered(static_cast<std::size_t>(required), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT,
                              LCMAP_LOWERCASE,
                              value.data(),
                              static_cast<int>(value.size()),
                              lowered.data(),
                              required,
                              nullptr,
                              nullptr,
                              0) == required) {
                value = std::move(lowered);
            }
        }
    }
    return value;
}

/** @brief 为单个文件查询 HDD 调度位置，失败时安全退化为空。 */
std::optional<std::uint64_t> QueryPhysicalStart(const std::filesystem::path& path) {
    const std::string utf8 = WideToUtf8(path.wstring());
    if (utf8.empty()) return std::nullopt;
    FilePhysicalLocation location{};
    if (!QueryFilePhysicalLocation(utf8.c_str(), &location)) return std::nullopt;
    return location.physicalStartByte;
}

/** @brief 由 Everything SDK 元数据创建路径记录。 */
FilePathRecord BuildRecord(const std::filesystem::path& path,
                           std::uint64_t size_bytes,
                           const FILETIME& ftCreation,
                           const FILETIME& ftLastWrite,
                           const DiscoveryRoot& root,
                           const std::uint64_t scanId) {
    FilePathRecord record;
    record.scan_id = scanId;
    record.path = AbsolutePath(path);
    record.normalized_path_key = NormalizePathKey(record.path);
    record.volume_guid = root.volume_guid;
    record.storage_target_key = root.storage_target_key;
    record.size_bytes = size_bytes;
    record.extension = record.path.extension().wstring();
    std::transform(record.extension.begin(), record.extension.end(), record.extension.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    record.creation_time_utc_ms = FileTimeToUnixMilliseconds(ftCreation);
    record.last_write_time_utc_ms = FileTimeToUnixMilliseconds(ftLastWrite);
    record.scan_root_priority = root.priority;
    record.state = FilePathState::Pending;
    record.sync_state = SyncState::LocalOnly;
    return record;
}

}  // namespace

// ---- SdkState 定义 ----
struct EverythingFileDiscovery::SdkState {
    std::once_flag init_flag;
    bool ready = false;
    std::string init_error;
    std::mutex ready_mutex;
    std::mutex query_mutex;
    HMODULE dll = nullptr;
    std::wstring dll_path;
    std::wstring exe_path;
    unsigned int query_page_size = 4096;
    DWORD launch_timeout_ms = 30000;
    DWORD db_load_timeout_ms = 120000;
    DWORD poll_interval_ms = 200;

    EvSetSearch_t fnSetSearch = nullptr;
    EvSetMatchPath_t fnSetMatchPath = nullptr;
    EvSetMax_t fnSetMax = nullptr;
    EvSetOffset_t fnSetOffset = nullptr;
    EvSetRequestFlags_t fnSetRequestFlags = nullptr;
    EvQuery_t fnQuery = nullptr;
    EvGetNumResults_t fnGetNumResults = nullptr;
    EvGetTotResults_t fnGetTotResults = nullptr;
    EvIsFolderResult_t fnIsFolderResult = nullptr;
    EvGetResultFullPathName_t fnGetResultFullPathName = nullptr;
    EvGetResultSize_t fnGetResultSize = nullptr;
    EvGetResultDateCreated_t fnGetResultDateCreated = nullptr;
    EvGetResultDateModified_t fnGetResultDateModified = nullptr;
    EvReset_t fnReset = nullptr;
    EvGetLastError_t fnGetLastError = nullptr;
    EvIsDBLoaded_t fnIsDBLoaded = nullptr;
    EvGetBuildNumber_t fnGetBuildNumber = nullptr;
    EvGetMajorVersion_t fnGetMajorVersion = nullptr;
};

// ---- State() ----
EverythingFileDiscovery::SdkState& EverythingFileDiscovery::State() {
    static SdkState instance;
    return instance;
}

// ---- Configure() ----
void EverythingFileDiscovery::Configure(const DiscoveryConfig& config) {
    SdkState& state = State();
    std::lock_guard<std::mutex> readyLock(state.ready_mutex);
    state.dll_path = config.everything_dll_path.wstring();
    state.exe_path = config.everything_exe_path.wstring();
    state.query_page_size = std::clamp(config.query_page_size, 128U, 100000U);
    state.launch_timeout_ms = std::clamp(config.launch_timeout_seconds, 1U, 600U) * 1000U;
    state.db_load_timeout_ms = std::clamp(config.db_load_timeout_seconds, 1U, 3600U) * 1000U;
    state.poll_interval_ms = std::clamp(config.poll_interval_milliseconds, 10U, 5000U);
}

// ---- PrepareRoot()（委托 NativeFileDiscovery，卷拓扑与发现方式无关） ----
std::optional<DiscoveryRoot> EverythingFileDiscovery::PrepareRoot(const std::filesystem::path& path,
                                                                   const std::uint32_t priority,
                                                                   const bool hdd_extent_optimization,
                                                                   std::string& error) {
    return NativeFileDiscovery::PrepareRoot(path, priority, hdd_extent_optimization, error);
}

// ---- EnsureReady() ----
bool EverythingFileDiscovery::EnsureReady(const std::atomic_bool& cancel_requested,
                                          bool& cancelled,
                                          std::string& error) {
    SdkState& state = State();
    std::lock_guard<std::mutex> readyLock(state.ready_mutex);
    cancelled = false;
    if (cancel_requested.load(std::memory_order_relaxed)) {
        cancelled = true;
        error.clear();
        return false;
    }

    // 查找 Everything.exe 路径（配置路径 → exe 目录 → PATH 搜索）
    auto findEverythingExe = [&]() -> std::wstring {
        if (!state.exe_path.empty() &&
            GetFileAttributesW(state.exe_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return state.exe_path;
        }
        wchar_t exeDir[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            std::wstring exePath(exeDir);
            const std::size_t pos = exePath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                std::wstring candidate = exePath.substr(0, pos + 1)
                                       + L"third_party\\es\\Everything.exe";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return candidate;
                }
            }
        }
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"Everything.exe", nullptr, MAX_PATH, buf, nullptr)) {
            return buf;
        }
        return L"";
    };

    // 检测 IPC 窗口，未运行则启动 Everything.exe -startup
    auto ensureEverythingRunning = [&](std::string& err) -> bool {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            cancelled = true;
            return false;
        }
        if (FindWindowW(kIpcWndClass, nullptr) != nullptr) return true;

        const std::wstring exePath = findEverythingExe();
        if (exePath.empty()) {
            err = "Everything.exe not found";
            return false;
        }

        std::wstring cmdLine = L"\"" + exePath + L"\" -startup";
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        std::wstring workDir = exePath;
        const std::size_t slashPos = workDir.find_last_of(L"\\/");
        if (slashPos != std::wstring::npos) {
            workDir.resize(slashPos);
        } else {
            workDir.clear();
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};

        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr,
                            workDir.empty() ? nullptr : workDir.c_str(),
                            &si, &pi)) {
            err = "CreateProcessW failed for Everything.exe: " + std::to_string(GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        DWORD elapsed = 0;
        while (elapsed < state.launch_timeout_ms) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                cancelled = true;
                return false;
            }
            if (FindWindowW(kIpcWndClass, nullptr) != nullptr) return true;
            Sleep(state.poll_interval_ms);
            elapsed += state.poll_interval_ms;
        }
        err = "Everything IPC window not ready after timeout";
        return false;
    };

    // 等待数据库加载完成
    auto waitForDbLoaded = [&](std::string& err) -> bool {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            cancelled = true;
            return false;
        }
        if (!state.fnIsDBLoaded) {
            // 旧版 SDK 无 IsDBLoaded，无法确认索引就绪；不阻断，查询可能返回0结果
            // 上层通过0结果回退 Native 兜底
            return true;
        }
        DWORD elapsed = 0;
        while (elapsed < state.db_load_timeout_ms) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                cancelled = true;
                return false;
            }
            if (state.fnIsDBLoaded()) return true;
            if (FindWindowW(kIpcWndClass, nullptr) == nullptr) {
                err = "Everything exited during DB load";
                return false;
            }
            Sleep(state.poll_interval_ms);
            elapsed += state.poll_interval_ms;
        }
        err = "Everything DB load timeout";
        return false;
    };

    // Step 1: 一次性初始化（加载 DLL + 解析函数指针）
    std::call_once(state.init_flag, [&]() {
        // 解析 DLL 路径：配置路径 → exe 目录\third_party\everything_sdk\ → PATH 搜索
        std::wstring dllPath = state.dll_path;
        if (dllPath.empty()) {
            wchar_t exeDir[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
                std::wstring exePath(exeDir);
                const std::size_t pos = exePath.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    dllPath = exePath.substr(0, pos + 1)
                            + L"third_party\\everything_sdk\\Everything64.dll";
                }
            }
        }
        if (!dllPath.empty()) {
            state.dll = LoadLibraryW(dllPath.c_str());
        }
        if (!state.dll) {
            state.dll = LoadLibraryW(L"Everything64.dll");
        }
        if (!state.dll) {
            state.init_error = "Everything64.dll not found";
            return;
        }

        // 解析必需函数指针（失败则 FreeLibrary 并设 init_error）
        #define RESOLVE(typeName, member, exportName) \
            do { \
                state.fn##member = (typeName)GetProcAddress(state.dll, exportName); \
                if (!state.fn##member) { \
                    state.init_error = "GetProcAddress failed for " exportName; \
                    FreeLibrary(state.dll); \
                    state.dll = nullptr; \
                    return; \
                } \
            } while (0)

        RESOLVE(EvSetSearch_t,             SetSearch,             "Everything_SetSearchW");
        RESOLVE(EvSetMatchPath_t,          SetMatchPath,          "Everything_SetMatchPath");
        RESOLVE(EvSetMax_t,                SetMax,                "Everything_SetMax");
        RESOLVE(EvSetOffset_t,             SetOffset,             "Everything_SetOffset");
        RESOLVE(EvSetRequestFlags_t,       SetRequestFlags,       "Everything_SetRequestFlags");
        RESOLVE(EvQuery_t,                 Query,                 "Everything_QueryW");
        RESOLVE(EvGetNumResults_t,         GetNumResults,         "Everything_GetNumResults");
        RESOLVE(EvIsFolderResult_t,        IsFolderResult,        "Everything_IsFolderResult");
        RESOLVE(EvGetResultFullPathName_t, GetResultFullPathName, "Everything_GetResultFullPathNameW");
        RESOLVE(EvReset_t,                 Reset,                 "Everything_Reset");
        RESOLVE(EvGetLastError_t,          GetLastError,          "Everything_GetLastError");

        #undef RESOLVE

        // 解析可选函数指针（旧版 SDK 可能缺失，不失败）
        state.fnGetTotResults = (EvGetTotResults_t)GetProcAddress(state.dll, "Everything_GetTotResultsW");
        if (!state.fnGetTotResults) {
            state.fnGetTotResults = (EvGetTotResults_t)GetProcAddress(state.dll, "Everything_GetTotResults");
        }
        state.fnGetResultSize = (EvGetResultSize_t)GetProcAddress(state.dll, "Everything_GetResultSize");
        state.fnGetResultDateCreated = (EvGetResultDateCreated_t)GetProcAddress(state.dll, "Everything_GetResultDateCreated");
        state.fnGetResultDateModified = (EvGetResultDateModified_t)GetProcAddress(state.dll, "Everything_GetResultDateModified");
        state.fnIsDBLoaded = (EvIsDBLoaded_t)GetProcAddress(state.dll, "Everything_IsDBLoaded");
        state.fnGetBuildNumber = (EvGetBuildNumber_t)GetProcAddress(state.dll, "Everything_GetBuildNumber");
        state.fnGetMajorVersion = (EvGetMajorVersion_t)GetProcAddress(state.dll, "Everything_GetMajorVersion");

        state.ready = true;
    });

    if (!state.ready) {
        error = state.init_error;
        return false;
    }

    // Step 2: 确保 Everything 进程运行
    if (!ensureEverythingRunning(error)) return false;

    // Step 3: 等待数据库加载完成
    if (!waitForDbLoaded(error)) return false;

    return true;
}

// ---- Enumerate() ----
DiscoveryStats EverythingFileDiscovery::Enumerate(const DiscoveryRoot& root,
                                                   const std::uint64_t scan_id,
                                                   const std::atomic_bool& cancel_requested,
                                                   const FileVisitor& visitor) {
    auto results = EnumerateRoots({root}, scan_id, cancel_requested, visitor);
    return results.empty() ? DiscoveryStats{} : std::move(results.front().stats);
}

namespace {

/** @brief 从 Everything 全局结果页复制出的最小文件元数据。 */
struct IndexedFileMetadata {
    std::filesystem::path path;
    std::uint64_t size_bytes = 0;
    FILETIME creation_time{};
    FILETIME last_write_time{};
};

std::wstring BuildRootSearchTerm(const DiscoveryRoot& root) {
    std::wstring path = AbsolutePath(root.path).wstring();
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (root.is_directory && !path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    return L"\"" + path + L"\"";
}

bool PathBelongsToRoot(const std::wstring& file_key,
                       const std::wstring& root_key,
                       const bool root_is_directory) {
    if (!root_is_directory) return file_key == root_key;
    if (file_key == root_key) return true;
    if (root_key.empty() || file_key.size() <= root_key.size()) return false;
    if (file_key.compare(0, root_key.size(), root_key) != 0) return false;
    return root_key.back() == L'\\' || file_key[root_key.size()] == L'\\';
}

std::optional<std::size_t> MatchRootIndex(
    const std::filesystem::path& path,
    const std::vector<DiscoveryRoot>& roots,
    const std::vector<std::wstring>& root_keys,
    const std::vector<bool>& covered) {
    const std::wstring fileKey = NormalizePathKey(path);
    for (std::size_t index = 0; index < roots.size(); ++index) {
        if (covered[index]) continue;
        if (PathBelongsToRoot(fileKey, root_keys[index], roots[index].is_directory)) {
            return index;
        }
    }
    return std::nullopt;
}

void MarkActiveRootsCancelled(std::vector<EverythingRootResult>& results) {
    for (auto& result : results) {
        if (!result.covered_by_higher_priority) result.stats.cancelled = true;
    }
}

}  // namespace

std::vector<EverythingRootResult> EverythingFileDiscovery::EnumerateRoots(
    const std::vector<DiscoveryRoot>& roots,
    const std::uint64_t scan_id,
    const std::atomic_bool& cancel_requested,
    const FileVisitor& visitor,
    const StageVisitor& stage_visitor) {
    std::vector<EverythingRootResult> results(roots.size());
    if (roots.empty()) return results;
    if (!visitor) {
        for (auto& result : results) result.stats.error = "file_visitor_required";
        return results;
    }

    std::vector<std::wstring> rootKeys;
    rootKeys.reserve(roots.size());
    std::vector<bool> covered(roots.size(), false);
    for (const auto& root : roots) rootKeys.push_back(NormalizePathKey(root.path));

    // 配置顺序就是优先级。后续根若完全落在更早的目录根中，不再重复查询。
    for (std::size_t index = 0; index < roots.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (covered[earlier]) continue;
            if (PathBelongsToRoot(rootKeys[index], rootKeys[earlier], roots[earlier].is_directory)) {
                covered[index] = true;
                results[index].covered_by_higher_priority = true;
                break;
            }
        }
    }

    std::wstring search = L"file: <";
    bool hasSearchTerm = false;
    for (std::size_t index = 0; index < roots.size(); ++index) {
        if (covered[index]) continue;
        if (hasSearchTerm) search += L"|";
        search += BuildRootSearchTerm(roots[index]);
        hasSearchTerm = true;
        if (stage_visitor) stage_visitor(roots[index].priority, EverythingDiscoveryStage::Preparing);
    }
    search += L">";
    if (!hasSearchTerm) return results;

    bool readyCancelled = false;
    std::string readyError;
    if (!EnsureReady(cancel_requested, readyCancelled, readyError)) {
        if (readyCancelled) {
            MarkActiveRootsCancelled(results);
        } else {
            for (auto& result : results) {
                if (!result.covered_by_higher_priority) {
                    result.stats.error = "EnsureReady failed: " + readyError;
                }
            }
        }
        return results;
    }

    SdkState& state = State();
    const unsigned int pageSize = state.query_page_size;
    const unsigned int requestFlags = EVERYTHING_REQUEST_FULL_PATH
                                    | EVERYTHING_REQUEST_SIZE
                                    | EVERYTHING_REQUEST_DATE_MODIFIED
                                    | EVERYTHING_REQUEST_DATE_CREATED;
    unsigned int offset = 0;
    unsigned int totalResults = 0;
    bool totalKnown = false;

    while (true) {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            MarkActiveRootsCancelled(results);
            return results;
        }

        for (std::size_t index = 0; index < roots.size(); ++index) {
            if (!covered[index] && stage_visitor) {
                stage_visitor(roots[index].priority, EverythingDiscoveryStage::QueryingIndex);
            }
        }

        std::vector<IndexedFileMetadata> page;
        unsigned int numThisPage = 0;
        unsigned int queryError = 0;
        {
            // Everything SDK 使用全局查询状态；锁内只执行 IPC 与元数据复制。
            std::lock_guard<std::mutex> queryLock(state.query_mutex);
            state.fnReset();
            state.fnSetSearch(search.c_str());
            state.fnSetMatchPath(TRUE);
            state.fnSetRequestFlags(requestFlags);
            state.fnSetMax(pageSize);
            state.fnSetOffset(offset);

            if (!state.fnQuery(TRUE)) {
                queryError = state.fnGetLastError();
                state.fnReset();
            } else {
                numThisPage = state.fnGetNumResults();
                if (!totalKnown && state.fnGetTotResults) {
                    totalResults = state.fnGetTotResults();
                    totalKnown = true;
                }

                page.reserve(numThisPage);
                std::wstring pathBuffer(kMaxPathBuf, L'\0');
                for (unsigned int resultIndex = 0; resultIndex < numThisPage; ++resultIndex) {
                    if (state.fnIsFolderResult(resultIndex)) continue;
                    const unsigned int copied = state.fnGetResultFullPathName(
                        resultIndex, pathBuffer.data(), kMaxPathBuf);
                    if (copied == 0 || copied >= kMaxPathBuf) continue;

                    IndexedFileMetadata metadata;
                    metadata.path = std::filesystem::path(pathBuffer.data());
                    LARGE_INTEGER size{};
                    if (state.fnGetResultSize) state.fnGetResultSize(resultIndex, &size);
                    metadata.size_bytes = static_cast<std::uint64_t>(size.QuadPart);
                    if (state.fnGetResultDateCreated) {
                        state.fnGetResultDateCreated(resultIndex, &metadata.creation_time);
                    }
                    if (state.fnGetResultDateModified) {
                        state.fnGetResultDateModified(resultIndex, &metadata.last_write_time);
                    }
                    page.push_back(std::move(metadata));
                }
                state.fnReset();
            }
        }

        if (queryError != 0) {
            const std::string message = "Everything_QueryW failed: " + std::to_string(queryError);
            for (auto& result : results) {
                if (!result.covered_by_higher_priority) result.stats.error = message;
            }
            return results;
        }
        if (numThisPage == 0) break;

        std::vector<bool> stageReported(roots.size(), false);
        for (auto& metadata : page) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                MarkActiveRootsCancelled(results);
                return results;
            }

            const auto rootIndex = MatchRootIndex(metadata.path, roots, rootKeys, covered);
            if (!rootIndex.has_value()) continue;
            const DiscoveryRoot& root = roots[*rootIndex];
            if (!stageReported[*rootIndex] && stage_visitor) {
                stage_visitor(root.priority,
                              root.query_physical_location
                                  ? EverythingDiscoveryStage::QueryingPhysicalLocation
                                  : EverythingDiscoveryStage::ProcessingResults);
                stageReported[*rootIndex] = true;
            }

            DiscoveredFile file;
            file.record = BuildRecord(metadata.path,
                                      metadata.size_bytes,
                                      metadata.creation_time,
                                      metadata.last_write_time,
                                      root,
                                      scan_id);
            file.media_kind = NativeFileDiscovery::ClassifyMedia(metadata.path);
            if (root.query_physical_location) {
                file.physical_start_byte = QueryPhysicalStart(metadata.path);
            }

            ++results[*rootIndex].stats.discovered_files;
            if (!visitor(std::move(file))) {
                MarkActiveRootsCancelled(results);
                return results;
            }
        }

        offset += numThisPage;
        if (numThisPage < pageSize) break;
        if (totalKnown && offset >= totalResults) break;
    }

    if (cancel_requested.load(std::memory_order_relaxed)) {
        MarkActiveRootsCancelled(results);
        return results;
    }

    unsigned int build = 0;
    unsigned int major = 0;
    int dbLoaded = -1;
    {
        std::lock_guard<std::mutex> queryLock(state.query_mutex);
        if (state.fnGetBuildNumber) build = state.fnGetBuildNumber();
        if (state.fnGetMajorVersion) major = state.fnGetMajorVersion();
        if (state.fnIsDBLoaded) dbLoaded = state.fnIsDBLoaded();
    }
    const std::string searchUtf8 = WideToUtf8(search);
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (covered[index] || results[index].stats.discovered_files != 0) continue;
        results[index].stats.error =
            "Everything returned 0 files for root: " + WideToUtf8(roots[index].path.wstring()) +
            "\n  - Everything version: " + std::to_string(major) + "." + std::to_string(build) +
            "\n  - IsDBLoaded: " + std::to_string(dbLoaded) +
            "\n  - batch query total results: " + std::to_string(totalResults) +
            "\n  - search string: " + searchUtf8;
    }
    return results;
}

}  // namespace videosc::dedup
