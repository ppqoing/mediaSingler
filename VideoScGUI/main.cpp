// VideoScGUI : ImGui-based GUI for the VideoSc DLL.
// Backend: Win32 + DirectX 11.

#include <windows.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>

#include "RenderBackend.h"
#include "VideoScApp.h"
#include "config/AppConfig.h"
#include "diagnostics/CrashHandler.h"
#include "logging/ApplicationErrorLogger.h"

namespace {

/** @brief 保证 COM 只在初始化成功时反初始化。 */
class ScopedCom final {
public:
    ScopedCom() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(result_)) CoUninitialize(); }
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

/** @brief 管理主窗口和窗口类，覆盖正常返回与 C++ 异常路径。 */
class ScopedWindow final {
public:
    ScopedWindow(HINSTANCE instance, const wchar_t* class_name) noexcept
        : instance_(instance), class_name_(class_name) {}

    ~ScopedWindow() {
        if (window_ != nullptr && IsWindow(window_)) DestroyWindow(window_);
        if (registered_) UnregisterClassW(class_name_, instance_);
    }

    void SetRegistered() noexcept { registered_ = true; }
    void SetWindow(HWND window) noexcept { window_ = window; }
    HWND window() const noexcept { return window_; }

private:
    HINSTANCE instance_ = nullptr;
    const wchar_t* class_name_ = nullptr;
    HWND window_ = nullptr;
    bool registered_ = false;
};

/**
 * @brief 记录主线程可恢复异常并向用户显示错误 ID。
 * @param message 不含敏感信息的错误说明。
 * @param exception_type 异常类别。
 * @return 固定的非零进程退出码。
 */
int ReportMainThreadFailure(const std::string& message, const std::string& exception_type) noexcept {
    videosc::dedup::ApplicationErrorRecord record;
    record.severity = "fatal";
    record.category = "main_thread_exception";
    record.module = "VideoScGUI";
    record.operation = "application_lifecycle";
    record.message = message;
    record.exception_type = exception_type;
    const std::string error_id = videosc::dedup::ApplicationErrorLogger::Write(record);
    const std::wstring text = L"程序遇到无法继续的错误，将安全退出。\n\n异常 ID：" +
                              std::wstring(error_id.begin(), error_id.end()) +
                              L"\n诊断目录：" + CrashHandler::CrashDirectory().wstring();
    MessageBoxW(nullptr, text.c_str(), L"VideoScGUI 异常", MB_OK | MB_ICONERROR);
    return 2;
}

/**
 * @brief 创建 Win32/D3D/ImGui 生命周期并运行主消息循环。
 * @param instance 当前模块实例。
 * @param show_command Win32 窗口显示参数。
 * @return 标准进程退出码。
 * @throws std::exception 初始化、渲染或业务主线程发生的 C++ 异常。
 */
int RunApplication(HINSTANCE instance, int show_command) {
    ScopedCom com;
    if (FAILED(com.result()) && com.result() != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("COM 初始化失败");
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr wchar_t kWindowClass[] = L"VideoScGUI";
    ScopedWindow scoped_window(instance, kWindowClass);
    WNDCLASSEXW window_class = {sizeof(window_class),
                               CS_CLASSDC,
                               RenderBackend::WndProc,
                               0L,
                               0L,
                               instance,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               kWindowClass,
                               nullptr};
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (RegisterClassExW(&window_class) == 0) throw std::runtime_error("注册主窗口类失败");
    scoped_window.SetRegistered();

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    const int window_width = (std::max)(1024, work_width > 0 ? work_width * 9 / 10 : 1280);
    const int window_height = (std::max)(640, work_height > 0 ? work_height * 9 / 10 : 800);
    const int position_x = work_area.left + (work_width - window_width) / 2;
    const int position_y = work_area.top + (work_height - window_height) / 2;
    const HWND window = CreateWindowExW(0,
                                        kWindowClass,
                                        L"VideoSc - 媒体去重工具",
                                        WS_OVERLAPPEDWINDOW,
                                        position_x < 0 ? 100 : position_x,
                                        position_y < 0 ? 100 : position_y,
                                        window_width,
                                        window_height,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        nullptr);
    if (window == nullptr) throw std::runtime_error("创建主窗口失败");
    scoped_window.SetWindow(window);
    ShowWindow(window, show_command == 0 ? SW_SHOWDEFAULT : show_command);
    UpdateWindow(window);

    RenderBackend backend;
    if (!backend.Init(window)) throw std::runtime_error("Direct3D 11 或 ImGui 初始化失败");
    {
        // 应用对象必须先于渲染后端销毁，确保纹理释放时 D3D 设备仍有效。
        VideoScApp app;
        app.SetD3DDevice(backend.GetDevice());
        constexpr float clear_color[4] = {0.067f, 0.067f, 0.106f, 1.00f};
        bool done = false;
        while (!done) {
            MSG message{};
            while (PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessage(&message);
                if (message.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (backend.CloseRequested() && !app.IsShutdownRequested()) {
                app.RequestShutdown();
            }
            if (app.IsShutdownRequested()) {
                app.AdvanceShutdown();
                if (app.IsShutdownComplete()) {
                    backend.SaveLayoutNow();
                    backend.ConfirmClose();
                    continue;
                }
            }
            backend.NewFrame();
            app.Render();
            app.ProcessDeferredActions();
            backend.RenderAndPresent(clear_color);
        }
        backend.SaveLayoutNow();
    }
    backend.Shutdown();
    return 0;
}

}  // namespace

/**
 * @brief 安装诊断系统并进入 GUI 生命周期。
 * @param instance 当前模块实例。
 * @param previous_instance 未使用的兼容参数。
 * @param command_line 未使用的命令行。
 * @param show_command Win32 窗口显示参数。
 * @return 标准进程退出码。
 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    try {
        const videosc::dedup::AppConfig defaults =
            videosc::dedup::AppConfig::CreateDefault(videosc::dedup::GetApplicationDirectory());
        CrashHandler::Install(defaults.logging.directory);
        videosc::dedup::ApplicationErrorLogger::Configure(defaults.logging);
        CrashHandler::LaunchExternalReporter();
        return RunApplication(instance, show_command);
    } catch (const std::bad_alloc&) {
        return ReportMainThreadFailure("内存不足", "std::bad_alloc");
    } catch (const std::exception& exception) {
        return ReportMainThreadFailure(exception.what(), "std::exception");
    } catch (...) {
        return ReportMainThreadFailure("未知 C++ 异常", "unknown_exception");
    }
}
