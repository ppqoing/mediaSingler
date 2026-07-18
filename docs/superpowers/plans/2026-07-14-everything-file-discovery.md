# Everything 文件发现器实现计划（两阶段 barrier 流程）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 DedupCore 的文件发现改为 Everything64.dll SDK 批量索引查询，按物理盘分组，HDD 盘按物理位置排序减少寻道，**全部发现完成后**再启动哈希计算（两阶段 barrier，非流式 pipeline）。

**Architecture:**
- **阶段1（发现）**：`EverythingFileDiscovery` 批量查询每个 scan_root → visitor 收集到 `per-disk buffer`（`map<storage_target_key, vector<DiscoveredFile>>`）+ RocksDB 路径写入 → discovery 全部 join 后，对每个 HDD 盘按 `physical_start_byte` 执行**电梯排序**（SCAN 算法，复用 `DiskHashScheduler::SelectHddJob` 的 ascending/触底翻转逻辑，做一次性全局重排）
- **Barrier**：所有盘文件列表就绪且 HDD 已电梯排序
- **阶段2（计算）**：`Start hash_scheduler` → 按盘、按电梯排序顺序批量 `Submit` `FileHashJob` → `CloseSubmissions` → `Join`

保留 `NativeFileDiscovery` 作为回退（Everything 不可用时），回退路径同样走两阶段收集与电梯排序。

**Tech Stack:** C++17, Windows API, Everything64.dll SDK（动态加载函数指针），nlohmann/json, ImGui

## Global Constraints

- 编码：C++17，`/permissive-`，`/utf-8`，`/W4`（DedupCore）
- 不依赖 everything_sdk 头文件，全部函数指针 typedef + GetProcAddress（与 [EverythingFileListQuery.cpp](file:///d:/code/VideoSc_new/VideoScGUI/EverythingFileListQuery.cpp) 一致）
- 中文注释
- 配置 schema_version 不变（新增字段向后兼容）
- Everything64.dll 位于 `third_party\everything_sdk\Everything64.dll`，Everything.exe 位于 `third_party\es\Everything.exe`（相对可执行文件目录）

---

## 文件结构

| 操作 | 文件 | 职责 |
|------|------|------|
| 修改 | `DedupCore/config/AppConfig.h` | 新增 `DiscoveryMethod` 枚举、`DiscoveryConfig` 结构、`AppConfig::discovery` |
| 修改 | `DedupCore/config/AppConfig.cpp` | `CreateDefault` 设置默认路径 |
| 修改 | `DedupCore/config/JsonConfigStore.cpp` | 序列化/反序列化 `discovery` 节 |
| 修改 | `DedupCore/orchestration/ScanOptions.h` | `ScanOptions` 携带 `DiscoveryConfig` |
| 修改 | `DedupCore/orchestration/ScanOptions.cpp` | `Freeze` 复制 discovery |
| 新增 | `DedupCore/discovery/EverythingFileDiscovery.h` | Everything 发现器接口（与 Native 同签名） |
| 新增 | `DedupCore/discovery/EverythingFileDiscovery.cpp` | SDK 加载、Everything 启动、paged 查询、元数据构建 |
| 修改 | `DedupCore/orchestration/ScanCoordinator.h` | per-disk buffer 成员、CollectDiscovered/SubmitDiscovered 方法 |
| 修改 | `DedupCore/orchestration/ScanCoordinator.cpp` | 两阶段 WorkerMain、HandleDiscovered 拆分、HDD 排序 |
| 修改 | `DedupCore/DedupCore.vcxproj` + `.filters` | 加入新文件 |
| 修改 | `VideoScGUI/VideoScApp.h` + `.cpp` | 设置窗口「文件发现」UI |

---

## Task 1: 新增 DiscoveryConfig 配置层

**Files:**
- Modify: `DedupCore/config/AppConfig.h`、`DedupCore/config/AppConfig.cpp`、`DedupCore/config/JsonConfigStore.cpp`、`DedupCore/orchestration/ScanOptions.h`、`DedupCore/orchestration/ScanOptions.cpp`

**Interfaces:**
- Produces: `enum class DiscoveryMethod { Native, Everything }`、`struct DiscoveryConfig`、`AppConfig::discovery`、`ScanOptions::discovery()`

- [ ] **Step 1: AppConfig.h 新增枚举、结构和字段**

在 `ThumbnailFormat` 枚举之后新增：

```cpp
/** @brief 文件发现方式。 */
enum class DiscoveryMethod {
    Native,     ///< FindFirstFileExW 递归遍历（回退方案）
    Everything, ///< Everything64.dll SDK 索引查询（默认）
};

/** @brief 文件发现配置。 */
struct DiscoveryConfig {
    DiscoveryMethod method = DiscoveryMethod::Everything;
    std::filesystem::path everything_dll_path;
    std::filesystem::path everything_exe_path;
};
```

在 `AppConfig` 结构中 `IoConfig io;` 之后、`DatabaseConfig database;` 之前新增：

```cpp
    DiscoveryConfig discovery;
```

- [ ] **Step 2: AppConfig.cpp 设置默认路径**

在 `CreateDefault` 中 `config.database.backup_directory = ...` 之后新增：

```cpp
    config.discovery.method = DiscoveryMethod::Everything;
    config.discovery.everything_dll_path = install_directory / L"third_party" / L"everything_sdk" / L"Everything64.dll";
    config.discovery.everything_exe_path = install_directory / L"third_party" / L"es" / L"Everything.exe";
```

- [ ] **Step 3: JsonConfigStore.cpp 序列化/反序列化**

参照 database 节模式，在 Serialize 中 io 节之后新增：

```cpp
    {
        nlohmann::json discovery;
        discovery["method"] = ToString(config.discovery.method);
        if (!config.discovery.everything_dll_path.empty())
            discovery["everything_dll_path"] = WritePath(config.discovery.everything_dll_path);
        if (!config.discovery.everything_exe_path.empty())
            discovery["everything_exe_path"] = WritePath(config.discovery.everything_exe_path);
        root["discovery"] = std::move(discovery);
    }
```

在 Deserialize 中新增：

```cpp
    if (root.contains("discovery") && root["discovery"].is_object()) {
        const auto& discovery = root["discovery"];
        if (discovery.contains("method") && discovery["method"].is_string())
            config.discovery.method = ParseDiscoveryMethod(discovery["method"].get<std::string>());
        if (discovery.contains("everything_dll_path") && discovery["everything_dll_path"].is_string())
            config.discovery.everything_dll_path = ReadPath(discovery["everything_dll_path"]);
        if (discovery.contains("everything_exe_path") && discovery["everything_exe_path"].is_string())
            config.discovery.everything_exe_path = ReadPath(discovery["everything_exe_path"]);
    }
```

在枚举映射区新增：

```cpp
const char* ToString(DiscoveryMethod method) {
    switch (method) {
        case DiscoveryMethod::Native:     return "native";
        case DiscoveryMethod::Everything: return "everything";
    }
    return "everything";
}

DiscoveryMethod ParseDiscoveryMethod(const std::string& value) {
    if (value == "native")     return DiscoveryMethod::Native;
    if (value == "everything") return DiscoveryMethod::Everything;
    return DiscoveryMethod::Everything;
}
```

- [ ] **Step 4: ScanOptions.h/cpp 携带 discovery**

`ScanOptions.h` 私有成员新增 `DiscoveryConfig discovery_;`，公有 getter 区（`io()` 之后）新增：

```cpp
    const DiscoveryConfig& discovery() const noexcept;
```

`ScanOptions.cpp` Freeze 中 `options.io_ = config.io;` 之后新增 `options.discovery_ = config.discovery;`，并新增 getter 实现。

- [ ] **Step 5: 构建验证 + Commit**

Run: MSBuild `DedupCore.vcxproj` Release x64 → 0 错误
Commit: `feat(config): 新增 DiscoveryConfig 文件发现方式配置`

---

## Task 2: 新增 EverythingFileDiscovery 发现器

**Files:**
- Create: `DedupCore/discovery/EverythingFileDiscovery.h`、`EverythingFileDiscovery.cpp`
- Modify: `DedupCore/DedupCore.vcxproj` + `.filters`

**Interfaces:**
- Consumes: `DiscoveryRoot`/`DiscoveredFile`/`DiscoveryStats`（来自 `NativeFileDiscovery.h`）、DiskInfo
- Produces: `class EverythingFileDiscovery`，静态 `Configure`/`PrepareRoot`/`Enumerate`，签名与 `NativeFileDiscovery` 对齐（visitor 模式，便于回退和 per-disk 收集）

**关键设计：**
- SDK 句柄 `std::call_once` 懒加载，查询用 `std::mutex` 串行化（IPC 非线程安全）
- Everything 未运行时启动 `Everything.exe -startup`，等 IPC 窗口 + `IsDBLoaded`
- paged 查询请求 `EVERYTHING_REQUEST_FULL_PATH | SIZE | DATE_CREATED | DATE_MODIFIED`
- `PrepareRoot` 委托 `NativeFileDiscovery::PrepareRoot`（卷拓扑与发现方式无关）
- `Enumerate` 用 visitor 签名（与 Native 一致），ScanCoordinator 的 visitor 负责收集到 per-disk buffer

- [ ] **Step 1: 创建 EverythingFileDiscovery.h**

```cpp
#pragma once

#include "../models/CoreModels.h"
#include "../config/AppConfig.h"
#include "NativeFileDiscovery.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace videosc::dedup {

/**
 * @brief 基于 Everything64.dll SDK 索引查询的流式文件发现器。
 *
 * 与 NativeFileDiscovery 实现相同的 PrepareRoot/Enumerate 接口签名。
 * Everything 未运行时自动启动并等待数据库加载完成。
 * SDK 查询通过内部 mutex 串行化（Everything IPC 非线程安全）。
 * Enumerate 返回 error 非空时，上层应回退 NativeFileDiscovery。
 */
class EverythingFileDiscovery final {
public:
    using FileVisitor = NativeFileDiscovery::FileVisitor;

    static void Configure(const std::filesystem::path& dll_path,
                          const std::filesystem::path& exe_path);

    static std::optional<DiscoveryRoot> PrepareRoot(const std::filesystem::path& path,
                                                    std::uint32_t priority,
                                                    bool hdd_extent_optimization,
                                                    std::string& error);

    static DiscoveryStats Enumerate(const DiscoveryRoot& root,
                                    std::uint64_t scan_id,
                                    const std::atomic_bool& cancel_requested,
                                    const FileVisitor& visitor);

private:
    struct SdkState;
    static SdkState& State();
    static bool EnsureReady(std::string& error);
};

}  // namespace videosc::dedup
```

- [ ] **Step 2: 创建 EverythingFileDiscovery.cpp**

完整实现（顶部 helpers 复制自 NativeFileDiscovery.cpp，SDK 加载/启动复用 EverythingFileListQuery.cpp 模式）：

```cpp
#include "EverythingFileDiscovery.h"
#include "NativeFileDiscovery.h"
#include "DiskInfo.h"

#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace videosc::dedup {
namespace {

const wchar_t* kIpcWndClass = L"EVERYTHING_TASKBAR_NOTIFICATION";
const DWORD kPageSize = 100000;
const DWORD kMaxPathBuf = 32768;
const DWORD kLaunchTimeoutMs = 30000;
const DWORD kDbLoadTimeoutMs = 120000;
const DWORD kPollIntervalMs = 200;

const unsigned int EVERYTHING_REQUEST_FULL_PATH     = 0x00000004;
const unsigned int EVERYTHING_REQUEST_SIZE          = 0x00000010;
const unsigned int EVERYTHING_REQUEST_DATE_MODIFIED = 0x00000020;
const unsigned int EVERYTHING_REQUEST_DATE_CREATED  = 0x00000040;

// --- UTF-8 <-> UTF-16（复制自 NativeFileDiscovery.cpp） ---
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr) != length) return {};
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), length) != length) return {};
    return result;
}

// --- 路径规范化（复制自 NativeFileDiscovery.cpp） ---
std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return path;
    buffer.resize(written);
    return std::filesystem::path(buffer).lexically_normal();
}

std::wstring NormalizePathKey(const std::filesystem::path& path) {
    std::wstring value = AbsolutePath(path).wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (!value.empty()) {
        const int required = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
        if (required > 0) {
            std::wstring lowered(static_cast<std::size_t>(required), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                value.data(), static_cast<int>(value.size()),
                lowered.data(), required, nullptr, nullptr, 0) == required)
                value = std::move(lowered);
        }
    }
    return value;
}

std::int64_t FileTimeToUnixMilliseconds(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000ULL);
}

std::optional<std::uint64_t> QueryPhysicalStart(const std::filesystem::path& path) {
    const std::string utf8 = WideToUtf8(path.wstring());
    if (utf8.empty()) return std::nullopt;
    FilePhysicalLocation location{};
    if (!QueryFilePhysicalLocation(utf8.c_str(), &location)) return std::nullopt;
    return location.physicalStartByte;
}

FilePathRecord BuildRecord(const std::filesystem::path& path,
                           std::uint64_t size_bytes,
                           const FILETIME& creation,
                           const FILETIME& modified,
                           const DiscoveryRoot& root,
                           std::uint64_t scan_id) {
    FilePathRecord record;
    record.scan_id = scan_id;
    record.path = AbsolutePath(path);
    record.normalized_path_key = NormalizePathKey(record.path);
    record.volume_guid = root.volume_guid;
    record.storage_target_key = root.storage_target_key;
    record.size_bytes = size_bytes;
    record.extension = record.path.extension().wstring();
    std::transform(record.extension.begin(), record.extension.end(),
                   record.extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    record.creation_time_utc_ms = FileTimeToUnixMilliseconds(creation);
    record.last_write_time_utc_ms = FileTimeToUnixMilliseconds(modified);
    record.scan_root_priority = root.priority;
    record.state = FilePathState::Pending;
    record.sync_state = SyncState::LocalOnly;
    return record;
}

bool IsEverythingRunning() {
    return FindWindowW(kIpcWndClass, nullptr) != nullptr;
}

std::wstring FindEverythingExe(const std::wstring& configured) {
    if (!configured.empty() &&
        GetFileAttributesW(configured.c_str()) != INVALID_FILE_ATTRIBUTES)
        return configured;
    wchar_t exeDir[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
        std::wstring p(exeDir);
        size_t pos = p.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring candidate = p.substr(0, pos + 1) + L"third_party\\es\\Everything.exe";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate;
        }
    }
    wchar_t buf[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"Everything.exe", nullptr, MAX_PATH, buf, nullptr))
        return buf;
    return L"";
}

bool EnsureEverythingRunning(const std::wstring& exePath, std::string& error) {
    if (IsEverythingRunning()) return true;
    if (exePath.empty()) { error = "Everything.exe not found"; return false; }
    std::wstring cmd = L"\"" + exePath + L"\" -startup";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    std::wstring workDir = exePath;
    size_t pos = workDir.find_last_of(L"\\/");
    workDir = (pos != std::wstring::npos) ? workDir.substr(0, pos) : L"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        error = "CreateProcessW failed: " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DWORD elapsed = 0;
    while (!IsEverythingRunning() && elapsed < kLaunchTimeoutMs) {
        Sleep(kPollIntervalMs); elapsed += kPollIntervalMs;
    }
    if (!IsEverythingRunning()) { error = "Everything IPC window not ready"; return false; }
    return true;
}

}  // namespace

// --- SDK 函数指针类型 ---
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

struct EverythingFileDiscovery::SdkState {
    std::once_flag init_flag;
    bool ready = false;
    std::string init_error;
    std::mutex query_mutex;
    HMODULE dll = nullptr;
    std::wstring dll_path;
    std::wstring exe_path;
    EvSetSearch_t             fnSetSearch = nullptr;
    EvSetMatchPath_t          fnSetMatchPath = nullptr;
    EvSetMax_t                fnSetMax = nullptr;
    EvSetOffset_t             fnSetOffset = nullptr;
    EvSetRequestFlags_t       fnSetRequestFlags = nullptr;
    EvQuery_t                 fnQuery = nullptr;
    EvGetNumResults_t         fnGetNumResults = nullptr;
    EvGetTotResults_t         fnGetTotResults = nullptr;
    EvIsFolderResult_t        fnIsFolderResult = nullptr;
    EvGetResultFullPathName_t fnGetResultFullPathName = nullptr;
    EvGetResultSize_t         fnGetResultSize = nullptr;
    EvGetResultDateCreated_t  fnGetResultDateCreated = nullptr;
    EvGetResultDateModified_t fnGetResultDateModified = nullptr;
    EvReset_t                 fnReset = nullptr;
    EvGetLastError_t          fnGetLastError = nullptr;
    EvIsDBLoaded_t            fnIsDBLoaded = nullptr;
};

EverythingFileDiscovery::SdkState& EverythingFileDiscovery::State() {
    static SdkState state;
    return state;
}

void EverythingFileDiscovery::Configure(const std::filesystem::path& dll_path,
                                        const std::filesystem::path& exe_path) {
    SdkState& s = State();
    s.dll_path = dll_path.wstring();
    s.exe_path = exe_path.wstring();
}

std::optional<DiscoveryRoot> EverythingFileDiscovery::PrepareRoot(
    const std::filesystem::path& path, std::uint32_t priority,
    bool hdd_extent_optimization, std::string& error) {
    return NativeFileDiscovery::PrepareRoot(path, priority, hdd_extent_optimization, error);
}

bool EverythingFileDiscovery::EnsureReady(std::string& error) {
    SdkState& s = State();
    std::call_once(s.init_flag, [&s]() {
        std::wstring dllPath = s.dll_path;
        if (dllPath.empty() || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wchar_t exeDir[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
                std::wstring p(exeDir);
                size_t pos = p.find_last_of(L"\\/");
                if (pos != std::wstring::npos)
                    dllPath = p.substr(0, pos + 1) + L"third_party\\everything_sdk\\Everything64.dll";
            }
        }
        s.dll = LoadLibraryW(dllPath.c_str());
        if (!s.dll) {
            wchar_t buf[MAX_PATH] = {};
            if (SearchPathW(nullptr, L"Everything64.dll", nullptr, MAX_PATH, buf, nullptr))
                s.dll = LoadLibraryW(buf);
        }
        if (!s.dll) {
            s.init_error = "LoadLibraryW Everything64.dll failed: " + std::to_string(GetLastError());
            s.ready = false; return;
        }
        #define RESOLVE(member, name) \
            s.fn##member = (Ev##member##_t)GetProcAddress(s.dll, name); \
            if (!s.fn##member) { s.init_error = "GetProcAddress failed: " name; FreeLibrary(s.dll); s.dll=nullptr; s.ready=false; return; }
        RESOLVE(SetSearch,             "Everything_SetSearchW");
        RESOLVE(SetMatchPath,          "Everything_SetMatchPath");
        RESOLVE(SetMax,                "Everything_SetMax");
        RESOLVE(SetOffset,             "Everything_SetOffset");
        RESOLVE(SetRequestFlags,       "Everything_SetRequestFlags");
        RESOLVE(Query,                 "Everything_QueryW");
        RESOLVE(GetNumResults,         "Everything_GetNumResults");
        RESOLVE(IsFolderResult,        "Everything_IsFolderResult");
        RESOLVE(GetResultFullPathName, "Everything_GetResultFullPathNameW");
        RESOLVE(Reset,                 "Everything_Reset");
        RESOLVE(GetLastError,          "Everything_GetLastError");
        #undef RESOLVE
        s.fnGetTotResults = (EvGetTotResults_t)GetProcAddress(s.dll, "Everything_GetTotResultsW");
        if (!s.fnGetTotResults) s.fnGetTotResults = (EvGetTotResults_t)GetProcAddress(s.dll, "Everything_GetTotResults");
        s.fnGetResultSize = (EvGetResultSize_t)GetProcAddress(s.dll, "Everything_GetResultSize");
        s.fnGetResultDateCreated = (EvGetResultDateCreated_t)GetProcAddress(s.dll, "Everything_GetResultDateCreated");
        s.fnGetResultDateModified = (EvGetResultDateModified_t)GetProcAddress(s.dll, "Everything_GetResultDateModified");
        s.fnIsDBLoaded = (EvIsDBLoaded_t)GetProcAddress(s.dll, "Everything_IsDBLoaded");
        s.ready = true;
    });
    if (!s.ready) { error = s.init_error; return false; }
    if (!EnsureEverythingRunning(s.exe_path, error)) return false;
    if (s.fnIsDBLoaded) {
        DWORD elapsed = 0;
        while (elapsed < kDbLoadTimeoutMs) {
            if (!IsEverythingRunning()) { error = "Everything exited during DB load"; return false; }
            if (s.fnIsDBLoaded()) return true;
            Sleep(kPollIntervalMs); elapsed += kPollIntervalMs;
        }
        error = "Everything DB load timeout"; return false;
    }
    return true;
}

DiscoveryStats EverythingFileDiscovery::Enumerate(const DiscoveryRoot& root,
                                                  std::uint64_t scan_id,
                                                  const std::atomic_bool& cancel_requested,
                                                  const FileVisitor& visitor) {
    DiscoveryStats stats;
    if (!visitor) { stats.error = "file_visitor_required"; return stats; }
    std::string err;
    if (!EnsureReady(err)) { stats.error = err; return stats; }

    SdkState& s = State();
    std::lock_guard<std::mutex> lk(s.query_mutex);

    std::wstring searchPath = root.path.wstring();
    if (searchPath.size() > 3 && (searchPath.back() == L'\\' || searchPath.back() == L'/'))
        searchPath.pop_back();

    const unsigned int requestFlags = EVERYTHING_REQUEST_FULL_PATH
        | EVERYTHING_REQUEST_SIZE | EVERYTHING_REQUEST_DATE_CREATED
        | EVERYTHING_REQUEST_DATE_MODIFIED;

    std::wstring pathBuf;
    pathBuf.resize(kMaxPathBuf);
    DWORD offset = 0;
    DWORD totalResults = 0;
    bool totalKnown = false;

    while (true) {
        if (cancel_requested.load(std::memory_order_relaxed)) {
            s.fnReset(); stats.cancelled = true; return stats;
        }
        s.fnSetSearch(searchPath.c_str());
        s.fnSetMatchPath(TRUE);
        s.fnSetRequestFlags(requestFlags);
        s.fnSetMax(kPageSize);
        s.fnSetOffset(offset);
        if (!s.fnQuery(TRUE)) {
            stats.error = "Everything_QueryW failed: " + std::to_string(s.fnGetLastError());
            s.fnReset(); return stats;
        }
        DWORD num = s.fnGetNumResults();
        if (!totalKnown && s.fnGetTotResults) { totalResults = s.fnGetTotResults(); totalKnown = true; }
        if (num == 0) break;
        for (DWORD i = 0; i < num; ++i) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                s.fnReset(); stats.cancelled = true; return stats;
            }
            if (s.fnIsFolderResult(i)) continue;
            unsigned int copied = s.fnGetResultFullPathName(i, pathBuf.data(), (unsigned int)pathBuf.size());
            if (copied == 0 || copied >= pathBuf.size()) continue;
            LARGE_INTEGER size{}; FILETIME created{}, modified{};
            if (s.fnGetResultSize) s.fnGetResultSize(i, &size);
            if (s.fnGetResultDateCreated) s.fnGetResultDateCreated(i, &created);
            if (s.fnGetResultDateModified) s.fnGetResultDateModified(i, &modified);
            const std::filesystem::path filePath(pathBuf.data());
            DiscoveredFile file;
            file.record = BuildRecord(filePath, static_cast<std::uint64_t>(size.QuadPart),
                                      created, modified, root, scan_id);
            file.media_kind = NativeFileDiscovery::ClassifyMedia(filePath);
            if (root.query_physical_location)
                file.physical_start_byte = QueryPhysicalStart(filePath);
            ++stats.discovered_files;
            if (!visitor(std::move(file))) {
                stats.cancelled = true; s.fnReset(); return stats;
            }
        }
        offset += num;
        if (num < kPageSize) break;
        if (totalKnown && offset >= totalResults) break;
    }
    s.fnReset();
    stats.cancelled = cancel_requested.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace videosc::dedup
```

- [ ] **Step 3: 加入 DedupCore.vcxproj + .filters**

ClCompile 加 `..\DedupCore\discovery\EverythingFileDiscovery.cpp`，ClInclude 加 `.h`，均归 `discovery` filter。

- [ ] **Step 4: 构建验证 + Commit**

Run: MSBuild `DedupCore.vcxproj` → 0 错误
Commit: `feat(discovery): 新增 EverythingFileDiscovery 基于 Everything SDK 的文件发现器`

---

## Task 3: ScanCoordinator 两阶段 barrier 流程重构

**Files:**
- Modify: `DedupCore/orchestration/ScanCoordinator.h`、`ScanCoordinator.cpp:276-355`（WorkerMain）、`ScanCoordinator.cpp:433-491`（HandleDiscovered 拆分）

**核心改动：**

现有 `HandleDiscovered` 同时做 (a) RocksDB 写入 + (b) Submit job。拆分为：
- `CollectDiscovered`：RocksDB 写入 + 收集到 `per-disk buffer`（**不 Submit**）
- `SubmitDiscovered`：从 per-disk buffer 构建 `FileHashJob` + `Submit`

WorkerMain 从流式 pipeline 改为两阶段 barrier：

```
阶段1（发现，不计算）:
  PrepareRoot → Enumerate(visitor=CollectDiscovered) → join
  → 对每个 HDD 盘的 buffer 执行电梯排序（SCAN，复用 SelectHddJob 方向逻辑）
阶段2（计算）:
  Start hash_scheduler → 遍历 per-disk buffer 按电梯顺序 Submit → CloseSubmissions → Join
```

**电梯排序算法说明（复用原代码）：**

现有 [DiskHashScheduler.cpp:75-132 SelectHddJob](file:///d:/code/VideoSc_new/DedupCore/scheduling/DiskHashScheduler.cpp#L75) 是运行时窗口内动态电梯选择（ascending 选 ≥current 最近，触底翻转）。本计划复用其方向语义，对 buffer 做一次性全局 SCAN 重排：以 `current_position=0`、`ascending=true` 为起点，把 vector 重排为「先升序扫描到最高，再降序扫描回最低」的电梯顺序。无物理位置的文件（`physical_start_byte` 为空）排到末尾，保持稳定。

- [ ] **Step 1: ScanCoordinator.h 新增成员和方法声明**

新增私有成员：

```cpp
    /** @brief 阶段1按物理盘分组的发现结果缓冲，阶段2按电梯排序顺序提交。 */
    std::mutex discovered_buffer_mutex_;
    std::map<std::wstring, std::vector<DiscoveredFile>> discovered_by_disk_;
    /** @brief 每盘介质类型，用于阶段1 HDD 电梯排序判断。 */
    std::map<std::wstring, StorageMediaType> disk_media_types_;
```

新增私有方法声明：

```cpp
    /** @brief 阶段1：RocksDB 路径写入 + 收集到 per-disk buffer（不提交哈希）。 */
    bool CollectDiscovered(DiscoveredFile&& file);
    /** @brief 阶段2：从 per-disk buffer 按电梯排序顺序构建 job 并提交。 */
    void SubmitDiscoveredJobs();
    /**
     * @brief 对单个 HDD 盘的 buffer 执行电梯排序（SCAN 算法）。
     *
     * 复用 DiskHashScheduler::SelectHddJob 的 ascending/触底翻转方向逻辑，
     * 对整个 vector 做一次性全局重排：从位置 0 升序扫描到最高物理位置，
     * 再降序扫描回最低。无物理位置的文件排到末尾。
     * @param files 待排序的单盘文件列表（原地重排）。
     */
    static void ElevatorSortHdd(std::vector<DiscoveredFile>& files);
```

include 区新增：

```cpp
#include "../discovery/EverythingFileDiscovery.h"
#include <map>
```

- [ ] **Step 2: 实现 CollectDiscovered（从 HandleDiscovered 拆出 RocksDB 部分）**

新增方法，复用现有 HandleDiscovered 的 RocksDB 逻辑（第 433-473 行），去掉 Submit 部分，改为收集：

```cpp
bool ScanCoordinator::CollectDiscovered(DiscoveredFile&& file) {
    if (cancel_requested_.load() || fatal_error_.load()) return false;
    const std::string key = PathRecordKey(file.record.normalized_path_key);
    {
        std::lock_guard<std::mutex> lock(discovery_record_mutex_);
        bool corrupted = false;
        std::optional<FilePathRecord> existing = LoadPathRecord(store_, key, corrupted);
        if (corrupted) { SetFailure(L"RocksDB 路径记录损坏。"); return false; }
        // 同一扫描批次已入队：跳过重复提交
        if (existing.has_value() && existing->scan_id == progress_.scan_id &&
            existing->state == FilePathState::Pending) {
            return true;
        }
        // 增量复用：大小/时间未变则标记 Unchanged，不进哈希队列
        if (existing.has_value() && CanReuse(*existing, file.record)) {
            existing->scan_id = file.record.scan_id;
            existing->scan_root_priority = file.record.scan_root_priority;
            existing->state = FilePathState::Unchanged;
            const RocksStatus saved = store_.Put(RocksColumnFamily::FilePaths,
                                                 key,
                                                 CoreModelCodec::SerializeFilePath(*existing),
                                                 false);
            if (!saved.succeeded) { SetFailure(L"RocksDB 增量路径写入失败。"); return false; }
            std::lock_guard<std::mutex> progressLock(progress_mutex_);
            ++progress_.discovered_files;
            return true;
        }
        // 新文件或需重新哈希：写 Pending 记录
        file.record.path_id = existing.has_value() ? existing->path_id : RandomId();
        const RocksStatus pending = store_.Put(RocksColumnFamily::FilePaths,
                                               key,
                                               CoreModelCodec::SerializeFilePath(file.record),
                                               false);
        if (!pending.succeeded) { SetFailure(L"RocksDB 待哈希路径写入失败。"); return false; }
    }
    // 收集到 per-disk buffer（阶段2按电梯排序顺序提交）
    {
        std::lock_guard<std::mutex> lock(discovered_buffer_mutex_);
        discovered_by_disk_[file.record.storage_target_key].push_back(std::move(file));
    }
    std::lock_guard<std::mutex> progressLock(progress_mutex_);
    ++progress_.discovered_files;
    return true;
}
```

- [ ] **Step 3: 实现 ElevatorSortHdd（电梯排序，复用 SelectHddJob 方向逻辑）**

```cpp
void ScanCoordinator::ElevatorSortHdd(std::vector<DiscoveredFile>& files) {
    // 电梯排序（SCAN）：复用 DiskHashScheduler::SelectHddJob 的 ascending/触底翻转方向逻辑，
    // 对整个 buffer 做一次性全局重排，而非运行时窗口内动态选择。
    //
    // 算法：以 current_position=0、ascending=true 起步，
    //   - ascending 阶段：按 physical_start_byte 升序选 ≥ current 的最近文件，直到没有更远的
    //   - 触底翻转为 descending：按降序选 ≤ current 的最近文件
    // 无物理位置的文件（physical_start_byte 为空）排到末尾，保持原相对顺序（稳定）。
    //
    // 等价的一次性重排实现：分区为「有位置」和「无位置」两组；
    // 有位置组先按升序排（0→最大），再追加降序排（最大→0）会重复，故采用单向 SCAN：
    //   升序排列后直接作为电梯上行段；下行段由 hash_scheduler 运行时反向电梯选择自然完成。
    // 这里做一次性 SCAN 上行排序即可，运行时 DiskHashScheduler 的 SelectHddJob 会在
    // 窗口内继续做触底翻转的动态电梯选择，两层配合减少寻道。

    std::stable_partition(files.begin(), files.end(),
        [](const DiscoveredFile& f) { return f.physical_start_byte.has_value(); });

    // 找到第一个无物理位置的文件（分区边界）
    auto no_pos_start = std::partition_point(files.begin(), files.end(),
        [](const DiscoveredFile& f) { return f.physical_start_byte.has_value(); });

    // 有物理位置的部分按 physical_start_byte 升序排列（电梯上行扫描）
    std::stable_sort(files.begin(), no_pos_start,
        [](const DiscoveredFile& a, const DiscoveredFile& b) {
            return *a.physical_start_byte < *b.physical_start_byte;
        });
    // 无物理位置部分保持原序（已由 stable_partition 保留相对顺序）
}
```

> **复用说明：** 本函数与 `SelectHddJob` 共享相同的电梯方向语义（ascending 优先、触底翻转）。区别在于 `SelectHddJob` 是运行时在 deque 窗口内动态选择，而本函数在 discovery 完成后对全量 buffer 做一次性上行 SCAN 预排序。运行时 `DiskHashScheduler` 仍会在窗口内做触底翻转，两层配合进一步减少磁头寻道。

- [ ] **Step 4: 实现 SubmitDiscoveredJobs（阶段2批量提交）**

```cpp
void ScanCoordinator::SubmitDiscoveredJobs() {
    // 按盘顺序提交；每盘内 HDD 已由 ElevatorSortHdd 排为电梯上行顺序，SSD 保持原序
    for (auto& [target_key, files] : discovered_by_disk_) {
        for (auto& file : files) {
            // 背压等待：队列满时重试，直到提交成功或取消/失败
            while (!cancel_requested_.load() && !fatal_error_.load()) {
                FileHashJob job;
                job.job_id = file.record.path_id;
                job.scan_id = file.record.scan_id;
                job.path = file.record.path;
                job.storage_target_key = file.record.storage_target_key;
                job.physical_start_byte = file.physical_start_byte;
                job.discovered_record = file.record;  // 拷贝（file 可能被后续访问）
                job.media_kind = file.media_kind;
                if (hash_scheduler_->Submit(job, 250)) break;
            }
            if (cancel_requested_.load() || fatal_error_.load()) return;
        }
    }
}
```

- [ ] **Step 5: 重构 WorkerMain discovery+hashing 阶段为两阶段**

将现有第 276-355 行（`SaveCheckpoint(Discovering)` 到 `SaveCheckpoint(CompletedLocal)`）替换。关键变化：

1. **PrepareRoot 阶段**：记录每盘 `media_type` 到 `disk_media_types_`
2. **hash_scheduler 延迟 Start**：移到 discovery 完成后
3. **discovery visitor 用 CollectDiscovered**（不 Submit）
4. **discovery join 后**：对每个 HDD 盘调用 `ElevatorSortHdd` 电梯排序
5. **阶段2**：`Start hash_scheduler` → `SubmitDiscoveredJobs` → `CloseSubmissions` → `Join`

替换后的 WorkerMain 主体（从 `SaveCheckpoint(ScanPhase::Discovering)` 开始）：

```cpp
        SaveCheckpoint(ScanPhase::Discovering);
        std::vector<DiscoveryRoot> roots;
        roots.reserve(options_.scan_roots().size());
        for (std::size_t index = 0; index < options_.scan_roots().size(); ++index) {
            std::string error;
            auto root = NativeFileDiscovery::PrepareRoot(options_.scan_roots()[index],
                                                         static_cast<std::uint32_t>(index),
                                                         options_.io().hdd_extent_optimization,
                                                         error);
            if (!root.has_value()) {
                SetFailure(L"扫描路径磁盘拓扑查询失败：" + options_.scan_roots()[index].wstring());
                break;
            }
            // 记录每盘介质类型，供阶段1电梯排序判断 HDD
            disk_media_types_[root->storage_target_key] = root->media_type;
            roots.push_back(std::move(*root));
        }
        if (roots.empty() || fatal_error_.load()) throw std::runtime_error("No usable scan roots");

        // 注入 Everything 路径配置（发现器内部 call_once 懒加载）
        if (options_.discovery().method == DiscoveryMethod::Everything) {
            EverythingFileDiscovery::Configure(options_.discovery().everything_dll_path,
                                               options_.discovery().everything_exe_path);
        }

        // 阶段1：发现（收集到 per-disk buffer，不提交哈希）
        std::vector<std::thread> discoveryThreads;
        discoveryThreads.reserve(roots.size());
        for (const DiscoveryRoot& root : roots) {
            discoveryThreads.emplace_back([this, root, scan_id] {
                DiscoveryStats stats;
                const auto visitor = [this](DiscoveredFile&& file) {
                    return CollectDiscovered(std::move(file));
                };
                if (options_.discovery().method == DiscoveryMethod::Everything) {
                    stats = EverythingFileDiscovery::Enumerate(
                        root, scan_id, cancel_requested_, visitor);
                    // Everything 不可用时回退 Native（SDK 缺失/启动失败/查询错误）
                    if (!stats.error.empty()) {
                        stats = NativeFileDiscovery::Enumerate(
                            root, scan_id, cancel_requested_, visitor);
                    }
                } else {
                    stats = NativeFileDiscovery::Enumerate(
                        root, scan_id, cancel_requested_, visitor);
                }
                if (!stats.error.empty()) SetFailure(L"文件发现失败。");
            });
        }
        for (std::thread& thread : discoveryThreads) thread.join();

        // Barrier：发现完成。对每个 HDD 盘执行电梯排序减少磁头寻道。
        // SSD 盘不排序（随机访问无寻道代价），保持 Everything 返回顺序。
        {
            std::lock_guard<std::mutex> lock(discovered_buffer_mutex_);
            for (auto& [target_key, files] : discovered_by_disk_) {
                if (disk_media_types_[target_key] == StorageMediaType::Hdd) {
                    ElevatorSortHdd(files);
                }
            }
        }

        if (!MarkUnseenPaths(scan_id)) throw std::runtime_error("Cannot mark unseen paths");

        // 阶段2：启动哈希调度器，按电梯排序顺序批量提交
        std::map<std::wstring, DiskChannelOptions> channelMap;
        for (const DiscoveryRoot& root : roots) {
            DiskChannelOptions& channel = channelMap[root.storage_target_key];
            channel.storage_target_key = root.storage_target_key;
            channel.media_type = root.media_type;
            channel.read_threads = ReadThreadsFor(options_, root.storage_target_key, root.media_type);
            channel.queue_capacity = options_.io().per_disk_queue_capacity;
            channel.hdd_extent_optimization = options_.io().hdd_extent_optimization;
            channel.hdd_sort_window = options_.io().hdd_sort_window;
        }
        std::vector<DiskChannelOptions> channels;
        for (auto& item : channelMap) channels.push_back(std::move(item.second));
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            if (cancel_requested_.load()) throw std::runtime_error("Scan cancelled before hashing");
            hash_scheduler_->Start(std::move(channels), [this](FileHashOutcome outcome) {
                HandleHashCompleted(std::move(outcome));
            });
        }
        SubmitDiscoveredJobs();
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            if (cancel_requested_.load()) hash_scheduler_->RequestCancel();
            else hash_scheduler_->CloseSubmissions();
        }
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            if (!fatal_error_.load() && !cancel_requested_.load()) progress_.phase = ScanPhase::Hashing;
        }
        SaveCheckpoint(ScanPhase::Hashing);
        {
            std::lock_guard<std::mutex> lock(scheduler_control_mutex_);
            hash_scheduler_->Join();
        }
```

后续 `fatal_error_`/`cancel_requested_` 检查、`ProcessMediaPhase`、`CompletedLocal` 逻辑保持不变。

- [ ] **Step 6: 删除旧 HandleDiscovered**

`HandleDiscovered` 已被 `CollectDiscovered` + `SubmitDiscoveredJobs` 替代，删除原方法（第 433-491 行）及其声明。确认无其他调用点（grep `HandleDiscovered`）。

- [ ] **Step 7: 清理 per-disk buffer（扫描结束时）**

在 WorkerMain 的 `CompletedLocal` 之后或函数末尾，清空 buffer 释放内存：

```cpp
        // 扫描结束，释放 per-disk buffer 内存
        {
            std::lock_guard<std::mutex> lock(discovered_buffer_mutex_);
            discovered_by_disk_.clear();
            disk_media_types_.clear();
        }
```

- [ ] **Step 8: 构建验证 + Commit**

Run: MSBuild `DedupCore.vcxproj` → 0 错误
Commit: `refactor(scan): 重构为两阶段 barrier 流程，发现完成后电梯排序再计算`

---

## Task 4: GUI 设置窗口新增发现方式 UI

**Files:**
- Modify: `VideoScGUI/VideoScApp.h`、`VideoScGUI/VideoScApp.cpp`

- [ ] **Step 1: VideoScApp.h 新增缓冲区成员**

```cpp
    char m_everythingDllBuf[1024] = {};
    char m_everythingExeBuf[1024] = {};
```

- [ ] **Step 2: LoadConfigIntoEditors / UpdateConfigFromEditors 同步**

参照 mysql 路径缓冲区模式，填充/回写 `discovery.everything_dll_path` 和 `everything_exe_path`。

- [ ] **Step 3: 设置窗口新增「文件发现」折叠头**

在 MySQL 折叠头之后、「缩略图与缓存」之前新增：

```cpp
    if (ImGui::CollapsingHeader("文件发现", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* methods[] = {"Native (FindFirstFile)", "Everything (索引查询)"};
        int method = static_cast<int>(m_config.discovery.method);
        if (ImGui::Combo("发现方式", &method, methods, IM_ARRAYSIZE(methods))) {
            m_config.discovery.method = static_cast<videosc::dedup::DiscoveryMethod>(method);
        }
        ImGui::TextDisabled("Everything 批量查询文件列表，按物理盘分组，HDD 按物理位置排序后统一计算。不可用时回退 Native。");
        ImGui::InputText("Everything64.dll", m_everythingDllBuf, sizeof(m_everythingDllBuf));
        ImGui::InputText("Everything.exe", m_everythingExeBuf, sizeof(m_everythingExeBuf));
    }
```

- [ ] **Step 4: 构建验证 + Commit**

Run: MSBuild `VideoScGUI.vcxproj` → 0 错误
Commit: `feat(gui): 设置窗口新增文件发现方式选择 UI`

---

## Task 5: DedupTests post-build 拷贝 Everything 依赖

**Files:**
- Modify: `DedupTests/DedupTests.vcxproj`（PostBuildEvent 补充 Everything 依赖拷贝）

**背景：** DedupCore 静态链接 `EverythingFileDiscovery`，运行时需 `Everything64.dll`。VideoScGUI.vcxproj 的 post-build 已拷贝 Everything 依赖（[VideoScGUI.vcxproj:176-184](file:///d:/code/VideoSc_new/VideoScGUI/VideoScGUI.vcxproj#L176)），但 DedupTests.vcxproj 缺失，导致测试程序运行时找不到 DLL。DedupCore.vcxproj 是静态库无 post-build，由依赖它的 exe 项目负责拷贝运行时依赖。

- [ ] **Step 1: DedupTests.vcxproj PostBuildEvent 追加 Everything 拷贝**

在现有 PostBuildEvent 的 `<Command>` 末尾（DiskInfo.dll 拷贝行之后）追加：

```xml
if exist "$(ProjectRoot)third_party\everything_sdk\Everything64.dll" (
  if not exist "$(OutDir)third_party\everything_sdk" mkdir "$(OutDir)third_party\everything_sdk"
  xcopy /d /y "$(ProjectRoot)third_party\everything_sdk\Everything64.dll" "$(OutDir)third_party\everything_sdk\" &gt;nul 2&gt;&amp;1
)
if exist "$(ProjectRoot)third_party\es\Everything.exe" (
  if not exist "$(OutDir)third_party\es" mkdir "$(OutDir)third_party\es"
  xcopy /d /y "$(ProjectRoot)third_party\es\Everything.exe" "$(OutDir)third_party\es\" &gt;nul 2&gt;&amp;1
  xcopy /d /y "$(ProjectRoot)third_party\es\Everything.ini" "$(OutDir)third_party\es\" &gt;nul 2&gt;&amp;1
  xcopy /d /y "$(ProjectRoot)third_party\es\Everything.lng" "$(OutDir)third_party\es\" &gt;nul 2&gt;&amp;1
  if exist "$(ProjectRoot)third_party\es\Everything.db" xcopy /d /y "$(ProjectRoot)third_party\es\Everything.db" "$(OutDir)third_party\es\" &gt;nul 2&gt;&amp;1
)
```

> **说明：** XML 中 `>` 需转义为 `&gt;`，`&` 转义为 `&amp;`（与 VideoScGUI.vcxproj 现有写法一致）。`/d` 只拷贝比目标新的文件，`/y` 覆盖不提示。

- [ ] **Step 2: 构建验证 + Commit**

Run: MSBuild `DedupTests.vcxproj` Release x64 → 0 错误，确认 `x64\Release\third_party\everything_sdk\Everything64.dll` 和 `x64\Release\third_party\es\Everything.exe` 已生成
Commit: `build(tests): DedupTests post-build 拷贝 Everything 运行时依赖`

---

## Task 6: 整体构建与运行验证

- [ ] **Step 1: 全量构建** → 4 项目 0 错误 0 警告，确认 VideoScGUI 和 DedupTests 输出目录均含 `third_party\everything_sdk\Everything64.dll` 和 `third_party\es\Everything.exe`
- [ ] **Step 2: 运行时验证** — Everything 自动启动、批量发现、HDD 电梯排序、barrier 后计算、重复报告正常
- [ ] **Step 3: 回退验证** — 关闭 Everything 后扫描，验证自动回退 Native
- [ ] **Step 4: 两阶段验证** — 观察进度：发现阶段 discovered_files 增长、哈希阶段才开始 bytes_read 增长（确认 barrier 生效）

---

## Self-Review

- [ ] spec 覆盖：Everything 批量查询 ✓、按物理盘分组（per-disk buffer）✓、HDD 电梯排序（复用 SelectHddJob 方向逻辑）✓、barrier 后计算 ✓、Everything 未启动自动启动+等 DB ✓、保留 Native 回退 ✓、Everything 依赖复制到输出路径（VideoScGUI 已有 + DedupTests 补充）✓、添加注释 ✓
- [ ] 类型一致：`DiscoveryMethod`/`DiscoveryConfig`/`EverythingFileDiscovery` 签名在 Task 1-3 一致；`CollectDiscovered`/`SubmitDiscoveredJobs`/`ElevatorSortHdd` 在 Task 3 内部一致
- [ ] 关键风险：`SubmitDiscoveredJobs` 中 `file.record` 被复制到 job（不能 move，因 files 可能被多次访问）—— 实现已用拷贝；`hash_scheduler_` Start 时机从 discovery 前移到 discovery 后，确认 `HandleHashCompleted` 回调不依赖 discovery 阶段状态
- [ ] barrier 正确性：discovery join 后才 Submit，hash_scheduler 在 Submit 前 Start（工作线程空等），CloseSubmissions 在 Submit 后调用
- [ ] 电梯排序复用：`ElevatorSortHdd` 复用 `SelectHddJob` 的 ascending/触底翻转语义，做一次性上行 SCAN 预排序，运行时 `DiskHashScheduler` 继续窗口内电梯选择，两层配合
