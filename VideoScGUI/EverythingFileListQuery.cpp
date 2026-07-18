// EverythingFileListQuery.cpp
//
// Async query of all files under given paths via voidtools' Everything SDK
// (Everything64.dll, loaded dynamically via LoadLibrary).
//
// Implementation notes (aligned with the testeverything reference project):
//   - Worker thread loads Everything64.dll once, ensures the Everything
//     service is running (starts Everything.exe with -startup and waits for
//     its IPC window + database to be ready if not), then queries each input
//     path via SDK API.
//   - Uses paged queries (Everything_SetOffset + Everything_SetMax) so that
//     paths with 1M+ files are handled without overflowing the IPC buffer.
//   - For each path, search string is: path:"<path>\"
//       * path: operator limits search to paths containing the given prefix
//       * trailing "\" prevents "D:\a" from matching "D:\abc"
//       * Folders are filtered out via Everything_IsFolderResult
//   - Files for the same disk number are merged and deduplicated, then
//     written to <outputDir>\disk_<n>.txt (UTF-8, one path per line).
//   - Cancellation: checked between paths and during result iteration.

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <new>
#include <exception>

#include "DiskInfo.h"
#include "EverythingFileListQuery.h"
#include "logging/ApplicationErrorLogger.h"

// Everything IPC window class (see everything_ipc.h). The process creates
// this hidden top-level window when running, regardless of UI visibility.
static const wchar_t* EVERYTHING_IPC_WNDCLASS = L"EVERYTHING_TASKBAR_NOTIFICATION";

// Max path length supported by Everything's full-path API (with \\?\ prefix).
static const DWORD MAX_PATH_BUF = 32768;

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 helpers
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !utf8[0]) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), len);
    return wide;
}

static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !wide[0]) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
EverythingFileListQuery::EverythingFileListQuery() = default;

EverythingFileListQuery::~EverythingFileListQuery() {
    Cancel();
    if (m_thread.joinable()) m_thread.join();
    if (m_hDll) {
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
    }
}

// ---------------------------------------------------------------------------
// SetEverythingDllPath / SetEverythingExePath
// ---------------------------------------------------------------------------
void EverythingFileListQuery::SetEverythingDllPath(const std::wstring& path) {
    m_dllPath = path;
}

void EverythingFileListQuery::SetEverythingExePath(const std::wstring& path) {
    m_everythingPath = path;
}

void EverythingFileListQuery::SetDiscoveryConfig(
    const videosc::dedup::DiscoveryConfig& config) {
    m_discoveryConfig = config;
    m_discoveryConfig.query_page_size = std::clamp(config.query_page_size, 128U, 100000U);
    m_discoveryConfig.launch_timeout_seconds =
        std::clamp(config.launch_timeout_seconds, 1U, 600U);
    m_discoveryConfig.db_load_timeout_seconds =
        std::clamp(config.db_load_timeout_seconds, 1U, 3600U);
    m_discoveryConfig.poll_interval_milliseconds =
        std::clamp(config.poll_interval_milliseconds, 10U, 5000U);
    m_dllPath = config.everything_dll_path.wstring();
    m_everythingPath = config.everything_exe_path.wstring();
}

// ---------------------------------------------------------------------------
// Start / IsDone / IsRunning / Cancel / GetResult
// ---------------------------------------------------------------------------
bool EverythingFileListQuery::Start(const std::vector<std::string>& paths,
                                     const std::string& outputDir) {
    if (m_running.load()) return false;
    if (m_thread.joinable()) m_thread.join();

    m_done.store(false);
    m_cancel.store(false);
    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_results.clear();
    }
    m_running.store(true);
    try {
        m_thread = std::thread(&EverythingFileListQuery::WorkerMain, this, paths, outputDir);
    } catch (const std::exception& exception) {
        m_running.store(false);
        m_done.store(true);
        videosc::dedup::ApplicationErrorLogger::Write(
            {"error", "thread_start", "VideoScGUI", "everything_query_start",
             exception.what(), "std::exception", {}, 0});
        return false;
    } catch (...) {
        m_running.store(false);
        m_done.store(true);
        videosc::dedup::ApplicationErrorLogger::Write(
            {"error", "thread_start", "VideoScGUI", "everything_query_start",
             "Everything 查询线程创建失败", "unknown_exception", {}, 0});
        return false;
    }
    return true;
}

bool EverythingFileListQuery::IsDone() const  { return m_done.load(); }
bool EverythingFileListQuery::IsRunning() const { return m_running.load(); }

void EverythingFileListQuery::Cancel() { m_cancel.store(true); }

std::vector<EverythingFileListQuery::DiskResult> EverythingFileListQuery::GetResult() {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return m_results;
}

// ---------------------------------------------------------------------------
// WorkerMain
// ---------------------------------------------------------------------------
void EverythingFileListQuery::WorkerMain(std::vector<std::string> paths,
                                          std::string outputDir) {
    auto fail_all = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_results.clear();
        for (const auto& p : paths) {
            DiskResult r;
            r.diskNumber = -1;
            r.error = msg;
            m_results.push_back(std::move(r));
        }
    };

    try {

    // 1. Load Everything64.dll
    {
        std::wstring err;
        if (!EnsureSdkLoaded(err)) {
            fail_all("Failed to load Everything64.dll: " + WideToUtf8(err.c_str()));
            m_running.store(false);
            m_done.store(true);
            return;
        }
    }

    // 2. Ensure Everything service is running (start if not)
    {
        std::wstring err;
        if (!EnsureEverythingRunning(err)) {
            fail_all("Everything not available: " + WideToUtf8(err.c_str()));
            m_running.store(false);
            m_done.store(true);
            return;
        }
    }

    // 3. Query disk number for each path, group by disk
    //    diskMap: diskNumber -> vector of (pathUtf8, files)
    struct PathResult {
        std::string pathUtf8;
        std::vector<std::string> files;
        bool ok = false;
        std::string error;
    };
    std::vector<PathResult> pathResults(paths.size());

    for (size_t i = 0; i < paths.size(); ++i) {
        if (m_cancel.load()) {
            pathResults[i].error = "cancelled";
            continue;
        }
        pathResults[i].pathUtf8 = paths[i];
        std::string err;
        if (!RunSdkQueryForPath(paths[i], pathResults[i].files, err)) {
            pathResults[i].error = err;
        } else {
            pathResults[i].ok = true;
        }
    }

    // 4. Group by disk number, merge & deduplicate
    std::unordered_map<int, std::unordered_set<std::string>> diskFiles;
    for (size_t i = 0; i < pathResults.size(); ++i) {
        if (!pathResults[i].ok) continue;
        int diskNo = GetPhysicalDiskNumber(pathResults[i].pathUtf8.c_str());
        auto& set = diskFiles[diskNo];
        for (auto& f : pathResults[i].files) {
            set.insert(f);
        }
    }

    // 5. Write output files
    std::vector<DiskResult> results;
    for (auto& kv : diskFiles) {
        int diskNo = kv.first;
        auto& fileSet = kv.second;

        std::vector<std::string> sortedFiles(fileSet.begin(), fileSet.end());
        std::sort(sortedFiles.begin(), sortedFiles.end());

        std::wstring outDirW = Utf8ToWide(outputDir.c_str());
        if (!outDirW.empty() && outDirW.back() != L'\\' && outDirW.back() != L'/') {
            outDirW += L'\\';
        }
        std::wstring outFileW = outDirW + L"disk_"
                              + std::to_wstring(diskNo) + L".txt";
        std::string outFileUtf8 = WideToUtf8(outFileW.c_str());

        // Create output directory if it does not exist (handles relative
        // paths like "file_output" that the caller hasn't pre-created).
        if (!outDirW.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(outDirW, ec);
        }

        std::ofstream ofs(outFileUtf8, std::ios::binary);
        if (!ofs) {
            DiskResult r;
            r.diskNumber = diskNo;
            r.error = "cannot open output file: " + outFileUtf8;
            results.push_back(std::move(r));
            continue;
        }
        for (const auto& f : sortedFiles) {
            ofs << f << "\n";
        }
        ofs.close();

        DiskResult r;
        r.diskNumber = diskNo;
        r.fileCount = (int)sortedFiles.size();
        r.outputFile = outFileUtf8;
        r.success = true;
        results.push_back(std::move(r));
    }

    // 6. Add error entries for failed paths
    for (size_t i = 0; i < pathResults.size(); ++i) {
        if (!pathResults[i].ok && !pathResults[i].error.empty()) {
            DiskResult r;
            r.diskNumber = GetPhysicalDiskNumber(pathResults[i].pathUtf8.c_str());
            r.error = pathResults[i].error;
            results.push_back(std::move(r));
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_results = std::move(results);
    }
    } catch (const std::bad_alloc&) {
        fail_all("Everything 查询内存不足");
        videosc::dedup::ApplicationErrorLogger::Write(
            {"error", "thread_exception", "VideoScGUI", "everything_query",
             "Everything 查询内存不足", "std::bad_alloc", {}, 0});
    } catch (const std::exception& exception) {
        fail_all(std::string("Everything 查询异常：") + exception.what());
        videosc::dedup::ApplicationErrorLogger::Write(
            {"error", "thread_exception", "VideoScGUI", "everything_query",
             exception.what(), "std::exception", {}, 0});
    } catch (...) {
        fail_all("Everything 查询发生未知异常");
        videosc::dedup::ApplicationErrorLogger::Write(
            {"error", "thread_exception", "VideoScGUI", "everything_query",
             "Everything 查询发生未知异常", "unknown_exception", {}, 0});
    }
    m_running.store(false);
    m_done.store(true);
}

// ---------------------------------------------------------------------------
// EnsureSdkLoaded: load Everything64.dll and resolve function pointers
// ---------------------------------------------------------------------------
bool EverythingFileListQuery::EnsureSdkLoaded(std::wstring& error) {
    if (m_hDll && m_fnQuery) return true;  // already loaded

    // Resolve DLL path
    std::wstring dllPath = m_dllPath;
    if (dllPath.empty()) {
        // 1. <exe-dir>\third_party\everything_sdk\Everything64.dll
        wchar_t exeDir[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            std::wstring exePath(exeDir);
            size_t pos = exePath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                dllPath = exePath.substr(0, pos + 1)
                        + L"third_party\\everything_sdk\\Everything64.dll";
            }
        }
        if (dllPath.empty() ||
            GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 2. .\third_party\everything_sdk\Everything64.dll (CWD)
            dllPath = L"third_party\\everything_sdk\\Everything64.dll";
        }
    }

    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        // Try SearchPathW as last resort
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"Everything64.dll", nullptr, MAX_PATH, buf, nullptr)) {
            hDll = LoadLibraryW(buf);
            if (hDll) dllPath = buf;
        }
    }
    if (!hDll) {
        error = L"LoadLibraryW failed for '" + dllPath +
              L"' (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    // Resolve function pointers (DLL exports W-suffixed Unicode variants).
    // typeName is the full typedef name; member is the m_fn* member name.
    #define RESOLVE(typeName, member, exportName) \
        m_fn##member = (typeName)GetProcAddress(hDll, exportName); \
        if (!m_fn##member) { \
            error = L"GetProcAddress failed for " L##exportName; \
            FreeLibrary(hDll); \
            return false; \
        }

    RESOLVE(Everything_SetSearchW_t,             SetSearch,             "Everything_SetSearchW");
    RESOLVE(Everything_SetMatchPath_t,           SetMatchPath,          "Everything_SetMatchPath");
    RESOLVE(Everything_SetMax_t,                 SetMax,                "Everything_SetMax");
    RESOLVE(Everything_SetOffset_t,              SetOffset,             "Everything_SetOffset");
    RESOLVE(Everything_SetRequestFlags_t,        SetRequestFlags,       "Everything_SetRequestFlags");
    RESOLVE(Everything_QueryW_t,                 Query,                 "Everything_QueryW");
    RESOLVE(Everything_GetNumResults_t,          GetNumResults,         "Everything_GetNumResults");
    RESOLVE(Everything_GetNumFileResults_t,      GetNumFileResults,     "Everything_GetNumFileResults");
    RESOLVE(Everything_IsFolderResult_t,         IsFolderResult,        "Everything_IsFolderResult");
    RESOLVE(Everything_IsFileResult_t,           IsFileResult,          "Everything_IsFileResult");
    RESOLVE(Everything_GetResultFullPathNameW_t, GetResultFullPathName, "Everything_GetResultFullPathNameW");
    RESOLVE(Everything_Reset_t,                  Reset,                 "Everything_Reset");
    RESOLVE(Everything_GetLastError_t,           GetLastError,          "Everything_GetLastError");
    #undef RESOLVE

    // Optional exports (newer SDK / 1.4+). GetTotResults / IsDBLoaded /
    // CleanUp may be absent in very old SDK builds; degrade gracefully.
    m_fnGetTotResults = (Everything_GetTotResults_t)
        GetProcAddress(hDll, "Everything_GetTotResultsW");
    if (!m_fnGetTotResults) {
        m_fnGetTotResults = (Everything_GetTotResults_t)
            GetProcAddress(hDll, "Everything_GetTotResults");
    }
    m_fnIsDBLoaded = (Everything_IsDBLoaded_t)
        GetProcAddress(hDll, "Everything_IsDBLoaded");
    m_fnCleanUp = (Everything_CleanUp_t)
        GetProcAddress(hDll, "Everything_CleanUp");

    m_hDll = hDll;
    return true;
}

// ---------------------------------------------------------------------------
// RunSdkQueryForPath: query all files under one path via Everything SDK
//
// Mirrors the testeverything reference project:
//   - Enables match-path mode and uses the path itself as the search string
//     (Everything treats it as a path-prefix match).
//   - Strips ONE trailing backslash/forward slash (unless drive root like
//     "D:\"), because paths are stored without trailing slashes and a
//     trailing slash here would miss files directly in the directory.
//   - Uses paged queries (SetOffset + SetMax) so paths with 1M+ files are
//     handled without overflowing the Everything IPC shared-memory buffer.
// ---------------------------------------------------------------------------
bool EverythingFileListQuery::RunSdkQueryForPath(const std::string& pathUtf8,
                                                  std::vector<std::string>& outFiles,
                                                  std::string& error) {
    if (!m_hDll || !m_fnQuery) {
        error = "SDK not loaded";
        return false;
    }

    std::wstring searchPath = Utf8ToWide(pathUtf8.c_str());

    // Strip ONE trailing backslash/forward slash (unless it is a drive root
    // like "D:\"). Everything's MatchPath mode matches against the path
    // portion only, and paths are stored without a trailing backslash, so a
    // trailing slash here would make us miss files directly in the directory.
    if (searchPath.size() > 3
        && (searchPath.back() == L'\\' || searchPath.back() == L'/')) {
        searchPath.pop_back();
    }

    outFiles.clear();

    // Path buffer: Everything supports long paths (\\?\ prefix) up to 32768.
    std::wstring pathBuf;
    pathBuf.resize(MAX_PATH_BUF);

    const DWORD pageSize = m_discoveryConfig.query_page_size;
    DWORD offset = 0;
    DWORD totalResults = 0;
    bool totalKnown = false;

    // Paged query loop. Each iteration fetches one page of `pageSize` items
    // starting at `offset`. Loop ends when a page returns fewer than pageSize
    // items, returns 0 items, or reaches the known total.
    while (true) {
        if (m_cancel.load()) {
            m_fnReset();
            error = "cancelled";
            return false;
        }

        // Re-configure query for this page (aligned with testeverything).
        // MatchPath mode treats the search string as a path-prefix match.
        m_fnSetSearch(searchPath.c_str());
        m_fnSetMatchPath(TRUE);
        m_fnSetMax(pageSize);
        m_fnSetOffset(offset);

        // Execute query (blocking).
        if (!m_fnQuery(TRUE)) {
            unsigned int ec = m_fnGetLastError();
            error = "Everything_QueryW failed at offset " + std::to_string(offset)
                  + " (error code " + std::to_string(ec) + ")";
            m_fnReset();
            return false;
        }

        DWORD numThisPage = m_fnGetNumResults();

        // Total result count is available right after the first query and is
        // not affected by max/offset. Used as a stop condition.
        if (!totalKnown && m_fnGetTotResults) {
            totalResults = m_fnGetTotResults();
            totalKnown = true;
        }

        if (numThisPage == 0) {
            break;  // no more results
        }

        for (DWORD i = 0; i < numThisPage; ++i) {
            if (m_cancel.load()) {
                m_fnReset();
                error = "cancelled";
                return false;
            }
            // Skip folders, keep only files.
            if (m_fnIsFolderResult(i)) continue;

            unsigned int copied = m_fnGetResultFullPathName(
                i, pathBuf.data(), (unsigned int)pathBuf.size());
            if (copied == 0 || copied >= pathBuf.size()) {
                continue;  // truncated or empty; skip
            }
            outFiles.push_back(WideToUtf8(pathBuf.data()));
        }

        offset += numThisPage;

        // Last page detected: fewer than pageSize items returned.
        if (numThisPage < pageSize) break;
        // Safety: reached the known total.
        if (totalKnown && offset >= totalResults) break;
    }

    m_fnReset();
    return true;
}

// ---------------------------------------------------------------------------
// IsEverythingRunning: detect Everything's IPC window
// ---------------------------------------------------------------------------
// Everything creates a hidden top-level window for IPC:
//   class name: EVERYTHING_TASKBAR_NOTIFICATION
// FindWindowW with a null title matches any window of that class, which is
// what the reference testeverything project does (the window title is not
// guaranteed to be "Everything" across versions / locales).
// ---------------------------------------------------------------------------
bool EverythingFileListQuery::IsEverythingRunning() {
    HWND hwnd = FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr);
    return hwnd != nullptr;
}

// ---------------------------------------------------------------------------
// TryRegExe: read a registry string value, treat as dir, append Everything.exe
// ---------------------------------------------------------------------------
std::wstring EverythingFileListQuery::TryRegExe(HKEY root, const wchar_t* subkey,
                                                  const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    LSTATUS rc = RegQueryValueExW(hKey, valueName, nullptr, &type,
                                   (LPBYTE)buf, &bufSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        return L"";
    }
    std::wstring dir(buf);
    if (dir.empty()) return L"";
    // The value may itself point to Everything.exe directly
    if (dir.size() >= 4 &&
        _wcsicmp(dir.c_str() + dir.size() - 4, L".exe") == 0) {
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
        return L"";
    }
    // Treat as directory: append backslash + Everything.exe
    if (dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    std::wstring exe = dir + L"Everything.exe";
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return exe;
    }
    return L"";
}

// ---------------------------------------------------------------------------
// FindEverythingExe: locate Everything.exe
// ---------------------------------------------------------------------------
std::wstring EverythingFileListQuery::FindEverythingExe() {
    // 1. User-configured path (highest priority)
    if (!m_everythingPath.empty() &&
        GetFileAttributesW(m_everythingPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return m_everythingPath;
    }

    // 2. <exe-dir>\third_party\es\Everything.exe (project layout)
    {
        wchar_t exeDir[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            std::wstring exePath(exeDir);
            size_t pos = exePath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                std::wstring candidate = exePath.substr(0, pos + 1)
                                       + L"third_party\\es\\Everything.exe";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return candidate;
                }
            }
        }
    }

    // 3. .\third_party\es\Everything.exe (CWD-relative)
    {
        std::wstring candidate = L"third_party\\es\\Everything.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    // 4. Try PATH
    {
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"Everything.exe", nullptr, MAX_PATH, buf, nullptr)) {
            if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) {
                return buf;
            }
        }
    }

    // 5. Registry: HKCU / HKLM / WOW6432Node
    struct RegLoc { HKEY root; const wchar_t* subkey; };
    RegLoc regLocs[] = {
        { HKEY_CURRENT_USER,  L"Software\\voidtools\\Everything" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\voidtools\\Everything" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\voidtools\\Everything" },
    };
    const wchar_t* valueNames[] = {
        L"InstallLocation",
        L"Location",
        L"Install_Dir",
        L"Path",
    };
    for (const auto& loc : regLocs) {
        for (const wchar_t* vn : valueNames) {
            std::wstring exe = TryRegExe(loc.root, loc.subkey, vn);
            if (!exe.empty()) return exe;
        }
    }

    // 6. Uninstall registry entries
    const wchar_t* uninstallKeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Everything",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{F7F056A4-5D8E-4F8B-8B9E-9C77A0A1C77B}_is1",
    };
    for (const wchar_t* uk : uninstallKeys) {
        std::wstring exe = TryRegExe(HKEY_LOCAL_MACHINE, uk, L"InstallLocation");
        if (!exe.empty()) return exe;
        exe = TryRegExe(HKEY_LOCAL_MACHINE, uk, L"DisplayLocation");
        if (!exe.empty()) return exe;
    }

    // 7. Common install locations
    const wchar_t* commonDirs[] = {
        L"C:\\Program Files\\Everything",
        L"C:\\Program Files (x86)\\Everything",
        L"D:\\Program Files\\Everything",
        L"D:\\Program Files (x86)\\Everything",
        L"E:\\Program Files\\Everything",
        L"E:\\Program Files (x86)\\Everything",
    };
    for (const wchar_t* dir : commonDirs) {
        std::wstring exe = std::wstring(dir) + L"\\Everything.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return exe;
        }
    }

    return L"";
}

// ---------------------------------------------------------------------------
// EnsureEverythingRunning: start Everything if not running, wait for IPC ready
//
// Mirrors the testeverything reference project:
//   - launches Everything.exe with -startup (minimized to tray, no main window)
//   - uses CREATE_NO_WINDOW so no console is allocated for the child
//   - sets the working directory to Everything.exe's folder so it finds
//     Everything.ini / Everything.lng / plugins next to itself
//   - waits for the IPC window (process ready), then waits for the database
//     to finish loading (Everything_IsDBLoaded) before returning so the
//     first query does not hit an empty / partial index
// ---------------------------------------------------------------------------
bool EverythingFileListQuery::EnsureEverythingRunning(std::wstring& error) {
    const DWORD launchTimeoutMs = m_discoveryConfig.launch_timeout_seconds * 1000U;
    const DWORD dbLoadTimeoutMs = m_discoveryConfig.db_load_timeout_seconds * 1000U;
    const DWORD pollIntervalMs = m_discoveryConfig.poll_interval_milliseconds;
    // Already running? Still wait for DB load below.
    const bool wasAlreadyRunning = IsEverythingRunning();

    if (!wasAlreadyRunning) {
        std::wstring exePath = FindEverythingExe();
        if (exePath.empty()) {
            error = L"Everything.exe not found (checked project layout, PATH, registry, Program Files)";
            return false;
        }

        // -startup: start minimized to tray, no main window.
        std::wstring cmdLine = L"\"" + exePath + L"\" -startup";

        // CreateProcessW may modify the command-line buffer, so use a
        // writable copy.
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        // Working directory = Everything.exe's folder, so it finds
        // Everything.ini / Everything.lng / plugins next to itself.
        std::wstring workDir = exePath;
        size_t slashPos = workDir.find_last_of(L"\\/");
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
            error = L"CreateProcessW failed for Everything.exe: error "
                  + std::to_wstring(GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // Wait for the IPC window to appear.
        DWORD elapsed = 0;
        while (!IsEverythingRunning() && elapsed < launchTimeoutMs) {
            if (m_cancel.load()) {
                error = L"cancelled while waiting for Everything to start";
                return false;
            }
            Sleep(pollIntervalMs);
            elapsed += pollIntervalMs;
        }
        if (!IsEverythingRunning()) {
            error = L"Everything IPC window not ready after "
                  + std::to_wstring(launchTimeoutMs / 1000)
                  + L" seconds";
            return false;
        }
    }

    // Wait for the database to finish loading (first run may build the
    // index). Everything_IsDBLoaded is optional in older SDK builds; if it
    // is missing we just rely on the IPC window being ready.
    if (m_fnIsDBLoaded) {
        DWORD elapsed = 0;
        while (elapsed < dbLoadTimeoutMs) {
            if (!IsEverythingRunning()) {
                error = L"Everything exited unexpectedly while waiting for DB load";
                return false;
            }
            if (m_fnIsDBLoaded()) {
                return true;
            }
            if (m_cancel.load()) {
                error = L"cancelled while waiting for Everything DB load";
                return false;
            }
            Sleep(pollIntervalMs);
            elapsed += pollIntervalMs;
        }
        error = L"Everything database did not load within "
              + std::to_wstring(dbLoadTimeoutMs / 1000)
              + L" seconds";
        return false;
    }

    return true;
}
