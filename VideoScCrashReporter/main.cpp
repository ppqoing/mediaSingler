#include <Windows.h>
#include <DbgHelp.h>

#include <filesystem>
#include <string>

#pragma comment(lib, "Dbghelp.lib")

namespace {

/**
 * @brief 读取命令行中的十进制进程 ID。
 * @param value 十进制字符串。
 * @return 合法 DWORD 进程 ID；失败返回 0。
 */
DWORD ParseProcessId(const wchar_t* value) noexcept {
    if (value == nullptr || *value == L'\0') return 0;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 10);
    return end != value && *end == L'\0' ? static_cast<DWORD>(parsed) : 0;
}

/**
 * @brief 为外部捕获的二次异常写入小型转储和元数据。
 * @param process 被监视进程句柄。
 * @param process_id 被监视进程 ID。
 * @param thread_id 异常线程 ID。
 * @param exception 调试事件复制出的异常记录。
 * @param directory 输出目录。
 */
void WriteExternalDump(HANDLE process,
                       const DWORD process_id,
                       const DWORD thread_id,
                       const EXCEPTION_RECORD& exception,
                       const std::filesystem::path& directory) noexcept {
    try {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        SYSTEMTIME time{};
        GetSystemTime(&time);
        wchar_t filename[512]{};
        swprintf_s(filename,
                   L"VideoScGUI-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu-external",
                   time.wYear,
                   time.wMonth,
                   time.wDay,
                   time.wHour,
                   time.wMinute,
                   time.wSecond,
                   process_id,
                   thread_id);
        const std::filesystem::path prefix = directory / filename;
        const HANDLE dump = CreateFileW((prefix.wstring() + L".dmp").c_str(),
                                        GENERIC_WRITE,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_ALL;
            const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
            if (thread != nullptr) GetThreadContext(thread, &context);
            EXCEPTION_RECORD record = exception;
            EXCEPTION_POINTERS pointers{&record, &context};
            MINIDUMP_EXCEPTION_INFORMATION information{thread_id, &pointers, FALSE};
            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            MiniDumpWriteDump(process,
                              process_id,
                              dump,
                              type,
                              thread == nullptr ? nullptr : &information,
                              nullptr,
                              nullptr);
            if (thread != nullptr) CloseHandle(thread);
            CloseHandle(dump);
        }

        const HANDLE text = CreateFileW((prefix.wstring() + L".txt").c_str(),
                                        GENERIC_WRITE,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (text != INVALID_HANDLE_VALUE) {
            wchar_t content[1024]{};
            swprintf_s(content,
                       L"VideoScGUI 外部崩溃报告\r\n异常码: 0x%08lX\r\n异常地址: %p\r\n进程: %lu\r\n线程: %lu\r\n",
                       exception.ExceptionCode,
                       exception.ExceptionAddress,
                       process_id,
                       thread_id);
            const WORD bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(text, &bom, sizeof(bom), &written, nullptr);
            WriteFile(text,
                      content,
                      static_cast<DWORD>(wcslen(content) * sizeof(wchar_t)),
                      &written,
                      nullptr);
            CloseHandle(text);
        }
    } catch (...) {
        // 报告进程失败时不能干扰被监视进程的异常分发。
    }
}

}  // namespace

/**
 * @brief 附加目标进程并在二次异常到达调试器时保存诊断文件。
 * @param argc 参数数量。
 * @param argv 参数数组，格式为 --pid N --directory PATH。
 * @return 0 表示监视正常结束，其他值表示启动参数或附加失败。
 */
int RunReporter(const int argc, wchar_t* argv[]) {
    if (argc != 5 || wcscmp(argv[1], L"--pid") != 0 || wcscmp(argv[3], L"--directory") != 0) return 1;
    const DWORD process_id = ParseProcessId(argv[2]);
    if (process_id == 0 || !DebugActiveProcess(process_id)) return 2;
    DebugSetProcessKillOnExit(FALSE);

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
                                 FALSE,
                                 process_id);
    bool running = true;
    while (running) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, INFINITE)) break;
        DWORD continue_status = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (process == nullptr) process = event.u.CreateProcessInfo.hProcess;
                else CloseHandle(event.u.CreateProcessInfo.hProcess);
                if (event.u.CreateProcessInfo.hFile != nullptr) CloseHandle(event.u.CreateProcessInfo.hFile);
                if (event.u.CreateProcessInfo.hThread != nullptr) CloseHandle(event.u.CreateProcessInfo.hThread);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile != nullptr) CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT:
                continue_status = DBG_EXCEPTION_NOT_HANDLED;
                if (!event.u.Exception.dwFirstChance && process != nullptr) {
                    WriteExternalDump(process,
                                      process_id,
                                      event.dwThreadId,
                                      event.u.Exception.ExceptionRecord,
                                      std::filesystem::path(argv[4]));
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                running = false;
                break;
            default:
                break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continue_status);
    }
    if (process != nullptr) CloseHandle(process);
    return 0;
}

/**
 * @brief 报告进程入口，保证普通 C++ 异常不会越过进程边界。
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @return 报告器退出码。
 */
int wmain(const int argc, wchar_t* argv[]) {
#ifdef _DEBUG
    if (argc == 2 && wcscmp(argv[1], L"--crash-probe") == 0) {
        // 仅 Debug 构建提供故障注入，用独立子进程验证 fail-fast 转储链路。
        Sleep(3000);
        RaiseFailFastException(nullptr, nullptr, 0);
        return 3;
    }
#endif
    try {
        return RunReporter(argc, argv);
    } catch (...) {
        return 3;
    }
}
