// EverythingFileListQuery.h
//
// Async query of all files under given paths via voidtools' Everything SDK
// (Everything64.dll, loaded dynamically via LoadLibrary). Results are grouped
// by physical disk number and written to <outputDir>\disk_<n>.txt (one path
// per line, UTF-8, deduplicated).
//
// Requirements:
//   - Everything64.dll must exist (default: third_party\everything_sdk\Everything64.dll).
//   - Everything.exe must exist (default: third_party\es\Everything.exe).
//   If the Everything service is not running, it will be started automatically
//   and the worker waits for its IPC window to be ready before querying.
//
// Usage (ImGui frame loop):
//   EverythingFileListQuery q;
//   q.Start({"D:\\foo", "E:\\bar"}, "C:\\out");
//   while (!q.IsDone()) { /* render frames */ }
//   auto results = q.GetResult();

#ifndef EVERYTHINGFILELISTQUERY_H
#define EVERYTHINGFILELISTQUERY_H

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

#include "config/AppConfig.h"

class EverythingFileListQuery {
public:
    // Result for one physical disk number.
    struct DiskResult {
        int         diskNumber = -1;   // physical disk number (>=0) or negative error code
        int         fileCount  = 0;    // number of files written to output file
        std::string outputFile;        // UTF-8 path of output file (disk_<n>.txt)
        bool        success    = false;
        std::string error;             // error message if failed
    };

    EverythingFileListQuery();
    ~EverythingFileListQuery();

    // Set Everything64.dll full path. Default: third_party\everything_sdk\Everything64.dll
    // (relative to executable directory).
    void SetEverythingDllPath(const std::wstring& path);

    // Set Everything.exe full path. If empty (default), auto-detect via
    // FindEverythingExe (project layout / PATH / registry / Program Files).
    void SetEverythingExePath(const std::wstring& path);

    // Set the shared Everything paging and readiness-wait parameters.
    // Must be called before Start(); values are copied into this query object.
    void SetDiscoveryConfig(const videosc::dedup::DiscoveryConfig& config);

    // Start async query.
    //   paths     - input paths (UTF-8), any file or folder on a mounted volume.
    //   outputDir - output directory (UTF-8); disk_<n>.txt files are created here.
    // Returns false if a query is already running.
    bool Start(const std::vector<std::string>& paths, const std::string& outputDir);

    // Poll completion (thread-safe).
    bool IsDone() const;
    bool IsRunning() const;

    // Request cancellation. Worker stops after the current path query.
    void Cancel();

    // Get results. Safe to call after IsDone() == true.
    std::vector<DiskResult> GetResult();

private:
    // Worker thread entry point.
    void WorkerMain(std::vector<std::string> paths, std::string outputDir);

    // Query all files under one path via Everything SDK. Appends results to outFiles.
    // Returns true on success; false on failure (error filled).
    bool RunSdkQueryForPath(const std::string& pathUtf8,
                            std::vector<std::string>& outFiles,
                            std::string& error);

    // Load Everything64.dll and resolve function pointers.
    // Returns true on success; false on failure (error filled).
    bool EnsureSdkLoaded(std::wstring& error);

    // Ensure Everything service is running. If not, start Everything.exe and
    // wait for its IPC window to be ready (so SDK can query its index).
    bool EnsureEverythingRunning(std::wstring& error);

    // Detect Everything IPC window.
    static bool IsEverythingRunning();

    // Find Everything.exe full path (project layout / PATH / registry / etc.).
    std::wstring FindEverythingExe();

    // Helper: read a registry string value and append Everything.exe.
    static std::wstring TryRegExe(HKEY root, const wchar_t* subkey,
                                   const wchar_t* valueName);

    // --- Everything64.dll function pointer types ---
    typedef void  (WINAPI* Everything_SetSearchW_t)(const wchar_t*);
    typedef void  (WINAPI* Everything_SetMatchPath_t)(int);
    typedef void  (WINAPI* Everything_SetMax_t)(unsigned int);
    typedef void  (WINAPI* Everything_SetOffset_t)(unsigned int);
    typedef void  (WINAPI* Everything_SetRequestFlags_t)(unsigned int);
    typedef int   (WINAPI* Everything_QueryW_t)(int);
    typedef unsigned int (WINAPI* Everything_GetNumResults_t)(void);
    typedef unsigned int (WINAPI* Everything_GetNumFileResults_t)(void);
    typedef DWORD (WINAPI* Everything_GetTotResults_t)(void);
    typedef int   (WINAPI* Everything_IsFolderResult_t)(unsigned int);
    typedef int   (WINAPI* Everything_IsFileResult_t)(unsigned int);
    typedef unsigned int (WINAPI* Everything_GetResultFullPathNameW_t)(unsigned int, wchar_t*, unsigned int);
    typedef void  (WINAPI* Everything_Reset_t)(void);
    typedef unsigned int (WINAPI* Everything_GetLastError_t)(void);
    typedef int   (WINAPI* Everything_IsDBLoaded_t)(void);
    typedef void  (WINAPI* Everything_CleanUp_t)(void);

    // --- Loaded function pointers (valid while m_hDll != nullptr) ---
    HMODULE                          m_hDll = nullptr;
    Everything_SetSearchW_t          m_fnSetSearch = nullptr;
    Everything_SetMatchPath_t        m_fnSetMatchPath = nullptr;
    Everything_SetMax_t              m_fnSetMax = nullptr;
    Everything_SetOffset_t           m_fnSetOffset = nullptr;
    Everything_SetRequestFlags_t     m_fnSetRequestFlags = nullptr;
    Everything_QueryW_t              m_fnQuery = nullptr;
    Everything_GetNumResults_t       m_fnGetNumResults = nullptr;
    Everything_GetNumFileResults_t   m_fnGetNumFileResults = nullptr;
    Everything_GetTotResults_t       m_fnGetTotResults = nullptr;
    Everything_IsFolderResult_t      m_fnIsFolderResult = nullptr;
    Everything_IsFileResult_t        m_fnIsFileResult = nullptr;
    Everything_GetResultFullPathNameW_t m_fnGetResultFullPathName = nullptr;
    Everything_Reset_t               m_fnReset = nullptr;
    Everything_GetLastError_t        m_fnGetLastError = nullptr;
    Everything_IsDBLoaded_t          m_fnIsDBLoaded = nullptr;
    Everything_CleanUp_t             m_fnCleanUp = nullptr;

    std::wstring            m_dllPath;        // Everything64.dll path (configured or auto)
    std::wstring            m_everythingPath; // Everything.exe path (configured or auto)
    videosc::dedup::DiscoveryConfig m_discoveryConfig;
    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_done{false};
    std::atomic<bool>       m_cancel{false};
    mutable std::mutex      m_resultMutex;
    std::vector<DiskResult> m_results;
};

#endif // EVERYTHINGFILELISTQUERY_H
