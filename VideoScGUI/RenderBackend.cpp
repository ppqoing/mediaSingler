// RenderBackend.cpp
//
// Owns the D3D11 device/swapchain/render-target, the ImGui context + backends,
// and the Win32 window procedure. Extracted from main.cpp so that main.cpp is
// a thin entry point.

#include "RenderBackend.h"
#include <d3dcompiler.h>
#include <tchar.h>
#include <filesystem>
#include <exception>
#include <stdexcept>
#include "logging/ApplicationErrorLogger.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Single-instance pointer used by the static WndProc shim to dispatch to the
// active RenderBackend instance. Set in Init(), cleared in Shutdown().
static RenderBackend* g_instance = nullptr;

/**
 * @brief 取得与可执行文件同目录的 ImGui 布局文件绝对路径。
 * @return UTF-8 编码的 `VideoScGUI.layout.ini` 路径；系统路径查询失败时返回相对文件名。
 */
static std::string ResolveImGuiLayoutPathUtf8() {
    wchar_t executablePath[32768]{};
    constexpr DWORD pathCapacity = static_cast<DWORD>(sizeof(executablePath) / sizeof(executablePath[0]));
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, pathCapacity);
    if (pathLength == 0 || pathLength >= pathCapacity) return "VideoScGUI.layout.ini";

    const std::wstring layoutPath =
        (std::filesystem::path(executablePath).parent_path() / L"VideoScGUI.layout.ini").wstring();
    const int utf8Length = WideCharToMultiByte(CP_UTF8,
                                                WC_ERR_INVALID_CHARS,
                                                layoutPath.data(),
                                                static_cast<int>(layoutPath.size()),
                                                nullptr,
                                                0,
                                                nullptr,
                                                nullptr);
    if (utf8Length <= 0) return "VideoScGUI.layout.ini";
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            layoutPath.data(),
                            static_cast<int>(layoutPath.size()),
                            result.data(),
                            utf8Length,
                            nullptr,
                            nullptr) != utf8Length) {
        return "VideoScGUI.layout.ini";
    }
    return result;
}

// Declared by imgui_impl_win32.h; forward-declared here so HandleMessage can
// use it without pulling the header into RenderBackend.h.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------
RenderBackend::RenderBackend() = default;

RenderBackend::~RenderBackend() {
    Shutdown();
}

// -----------------------------------------------------------------------------
// Init: create the D3D11 device + swap chain, then initialize ImGui.
// Returns false if device creation failed (partial state is cleaned up).
// -----------------------------------------------------------------------------
bool RenderBackend::Init(HWND hwnd) {
    m_mainWindow = hwnd;
    m_closeRequested = false;
    m_allowClose = false;
    // ---- D3D11 device + swap chain (moved from CreateDeviceD3D) ----
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
        &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
            &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext);
    }
    if (FAILED(res)) {
        // Release any partially created resources.
        if (m_pSwapChain)        { m_pSwapChain->Release();        m_pSwapChain = nullptr; }
        if (m_pd3dDeviceContext) { m_pd3dDeviceContext->Release(); m_pd3dDeviceContext = nullptr; }
        if (m_pd3dDevice)        { m_pd3dDevice->Release();        m_pd3dDevice = nullptr; }
        return false;
    }

    CreateRenderTarget();

    // ---- ImGui ----
    InitImGui(hwnd);

    g_instance = this;
    m_initialized = true;
    return true;
}

// -----------------------------------------------------------------------------
// NewFrame: begin a new ImGui frame.
// -----------------------------------------------------------------------------
void RenderBackend::NewFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

/**
 * @brief 绘制主视口和所有拖出的辅助视口，并提交对应交换链。
 * @param clearColor 主视口渲染目标使用的 RGBA 清屏颜色。
 * @throws std::runtime_error 主交换链提交失败时抛出。
 */
void RenderBackend::RenderAndPresent(const float clearColor[4]) {
    ImGui::Render();
    m_pd3dDeviceContext->OMSetRenderTargets(1, &m_mainRTV, nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRTV, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Multi-Viewport 由标准 Win32/DX11 后端创建和绘制辅助原生窗口。
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    const HRESULT presented = m_pSwapChain->Present(1, 0);
    if (FAILED(presented)) throw std::runtime_error("DXGI Present 失败");
    const ULONGLONG now = GetTickCount64();
    if (m_lastLayoutSaveTick == 0 || now - m_lastLayoutSaveTick >= 5000) {
        SaveLayoutNow();
        m_lastLayoutSaveTick = now;
    }
}

// -----------------------------------------------------------------------------
// Shutdown: shut down ImGui backends, destroy ImGui context, release D3D11
// resources. Safe to call multiple times.
// -----------------------------------------------------------------------------
void RenderBackend::Shutdown() {
    if (g_instance == this) g_instance = nullptr;

    if (m_initialized) {
        SaveLayoutNow();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_initialized = false;
    }

    // CleanupDeviceD3D
    CleanupRenderTarget();
    if (m_pSwapChain)        { m_pSwapChain->Release();        m_pSwapChain = nullptr; }
    if (m_pd3dDeviceContext) { m_pd3dDeviceContext->Release(); m_pd3dDeviceContext = nullptr; }
    if (m_pd3dDevice)        { m_pd3dDevice->Release();        m_pd3dDevice = nullptr; }
    m_mainWindow = nullptr;
}

void RenderBackend::SaveLayoutNow() noexcept {
    try {
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return;
        const char* path = ImGui::GetIO().IniFilename;
        if (path != nullptr && path[0] != '\0') ImGui::SaveIniSettingsToDisk(path);
    } catch (...) {
        // 布局保存失败不能阻止业务线程、RocksDB 和 D3D 资源按序关闭。
    }
}

void RenderBackend::ConfirmClose() noexcept {
    m_allowClose = true;
    if (m_mainWindow != nullptr && IsWindow(m_mainWindow)) {
        DestroyWindow(m_mainWindow);
    }
}

// -----------------------------------------------------------------------------
// CreateRenderTarget: create the render target view from the swap chain's
// back buffer.
// -----------------------------------------------------------------------------
void RenderBackend::CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    const HRESULT loaded = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(loaded) || pBackBuffer == nullptr) throw std::runtime_error("读取 DXGI 后台缓冲失败");
    const HRESULT created = m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_mainRTV);
    pBackBuffer->Release();
    if (FAILED(created)) throw std::runtime_error("创建 D3D11 渲染目标失败");
}

// -----------------------------------------------------------------------------
// CleanupRenderTarget: release the render target view.
// -----------------------------------------------------------------------------
void RenderBackend::CleanupRenderTarget() {
    if (m_mainRTV) {
        m_mainRTV->Release();
        m_mainRTV = nullptr;
    }
}

/**
 * @brief 创建 ImGui 上下文并启用 Docking 与独立原生辅助视口。
 * @param hwnd 主 Win32 窗口句柄。
 */
void RenderBackend::InitImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDecoration = false;
    io.ConfigViewportsNoDefaultParent = true;
    io.ConfigViewportsNoTaskBarIcon = false;
    m_imguiIniPathUtf8 = ResolveImGuiLayoutPathUtf8();
    io.IniFilename = m_imguiIniPathUtf8.c_str();

    LoadFonts();

    ImGui::StyleColorsDark();
    ApplyStyle();
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        // 辅助 HWND 是矩形渲染目标，使用不透明直角背景避免圆角区域露出清屏色。
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice, m_pd3dDeviceContext);
}

// -----------------------------------------------------------------------------
// LoadFonts: load the Microsoft YaHei Chinese font, falling back to the
// default font if the file is not present.
// -----------------------------------------------------------------------------
void RenderBackend::LoadFonts() {
    // Load Chinese font - Microsoft YaHei (微软雅黑)
    // Default Windows font at C:\Windows\Fonts\msyh.ttc
    ImGuiIO& io = ImGui::GetIO();
    const wchar_t* fontPath = L"C:\\Windows\\Fonts\\msyh.ttc";
    if (GetFileAttributesW(fontPath) != INVALID_FILE_ATTRIBUTES) {
        // Convert to UTF-8 for ImGui
        char fontPathUtf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fontPath, -1, fontPathUtf8, sizeof(fontPathUtf8), nullptr, nullptr);
        // Load Chinese glyphs (Common + Latin)
        const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesChineseFull();
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(fontPathUtf8, 18.0f, &cfg, glyphRanges);
    } else {
        // Fallback: default font
        io.Fonts->AddFontDefault();
    }
}

// -----------------------------------------------------------------------------
// ApplyStyle: soft modern dark theme (Catppuccin Mocha inspired)
//
// Palette: muted blue-purple base, soft lavender accent, pastel status colors.
// Larger rounding + thinner borders + generous padding for a calmer feel.
// -----------------------------------------------------------------------------
void RenderBackend::ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry — softer rounding, thinner borders, relaxed spacing
    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 10.0f;
    s.GrabRounding      = 8.0f;
    s.TabRounding       = 6.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.PopupBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.WindowPadding     = ImVec2(16, 14);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.IndentSpacing     = 18.0f;
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 10.0f;

    // Soft dark palette (Catppuccin Mocha-ish)
    auto col = [](float r, float g, float b, float a = 1.0f) {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    };
    const ImVec4 Base     = col(30, 30, 46);       // #1e1e2e base
    const ImVec4 Mantle   = col(24, 24, 37);       // #181825 mantle
    const ImVec4 Crust    = col(17, 17, 27);       // #11111b crust
    const ImVec4 Surface0 = col(49, 50, 68);       // #313244
    const ImVec4 Surface1 = col(69, 71, 90);       // #45475a
    const ImVec4 Surface2 = col(88, 91, 112);      // #585b70
    const ImVec4 Overlay0 = col(108, 112, 134);    // #6c7086
    const ImVec4 Overlay1 = col(127, 132, 156);    // #7f84 9c
    const ImVec4 Text     = col(205, 214, 244);    // #cdd6f4 text
    const ImVec4 Subtext  = col(166, 173, 200);    // #a6adc8 subtext0
    const ImVec4 Lavender = col(180, 190, 254);    // #b4befe lavender (accent)
    const ImVec4 Blue     = col(137, 180, 250);    // #89b4fa blue
    const ImVec4 Green    = col(166, 227, 161);    // #a6e3a1 green (success)
    const ImVec4 Red      = col(243, 139, 168);    // #f38ba8 red (error)
    const ImVec4 Yellow   = col(249, 226, 175);    // #f9e2af yellow (warning)
    const ImVec4 Peach    = col(250, 179, 135);    // #fab387 peach

    // Windows / backgrounds
    s.Colors[ImGuiCol_WindowBg]            = ImVec4(Base.x, Base.y, Base.z, 0.96f);
    s.Colors[ImGuiCol_ChildBg]             = ImVec4(Mantle.x, Mantle.y, Mantle.z, 1.00f);
    s.Colors[ImGuiCol_PopupBg]             = ImVec4(Surface0.x, Surface0.y, Surface0.z, 0.98f);
    s.Colors[ImGuiCol_Border]              = ImVec4(Surface2.x, Surface2.y, Surface2.z, 0.40f);
    s.Colors[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_DockingEmptyBg]      = Crust;
    s.Colors[ImGuiCol_DockingPreview]      = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.30f);

    // Text
    s.Colors[ImGuiCol_Text]               = Text;
    s.Colors[ImGuiCol_TextDisabled]       = Overlay0;
    s.Colors[ImGuiCol_TextSelectedBg]     = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.35f);

    // Frames / inputs
    s.Colors[ImGuiCol_FrameBg]            = Surface0;
    s.Colors[ImGuiCol_FrameBgHovered]     = Surface1;
    s.Colors[ImGuiCol_FrameBgActive]      = Surface2;

    // Titles / menus
    s.Colors[ImGuiCol_TitleBg]            = Mantle;
    s.Colors[ImGuiCol_TitleBgActive]      = Mantle;
    s.Colors[ImGuiCol_TitleBgCollapsed]   = Crust;
    s.Colors[ImGuiCol_MenuBarBg]          = Mantle;

    // Scrollbars
    s.Colors[ImGuiCol_ScrollbarBg]        = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab]      = Surface1;
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = Surface2;
    s.Colors[ImGuiCol_ScrollbarGrabActive]   = Overlay0;

    // Checkboxes / sliders / grabs
    s.Colors[ImGuiCol_CheckMark]          = Lavender;
    s.Colors[ImGuiCol_SliderGrab]         = Lavender;
    s.Colors[ImGuiCol_SliderGrabActive]   = Blue;
    s.Colors[ImGuiCol_Button]             = Surface0;
    s.Colors[ImGuiCol_ButtonHovered]      = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.35f);
    s.Colors[ImGuiCol_ButtonActive]       = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.55f);

    // Headers (collapsing headers, selectable rows)
    s.Colors[ImGuiCol_Header]             = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.18f);
    s.Colors[ImGuiCol_HeaderHovered]      = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.28f);
    s.Colors[ImGuiCol_HeaderActive]       = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.40f);

    // Separators
    s.Colors[ImGuiCol_Separator]          = Surface1;
    s.Colors[ImGuiCol_SeparatorHovered]   = Surface2;
    s.Colors[ImGuiCol_SeparatorActive]    = Overlay0;

    // Resize grips
    s.Colors[ImGuiCol_ResizeGrip]         = ImVec4(Surface1.x, Surface1.y, Surface1.z, 0.5f);
    s.Colors[ImGuiCol_ResizeGripHovered]  = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.6f);
    s.Colors[ImGuiCol_ResizeGripActive]   = Lavender;

    // Tabs
    s.Colors[ImGuiCol_Tab]                = Mantle;
    s.Colors[ImGuiCol_TabHovered]         = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.35f);
    s.Colors[ImGuiCol_TabActive]          = Surface0;
    s.Colors[ImGuiCol_TabUnfocused]       = Crust;
    s.Colors[ImGuiCol_TabUnfocusedActive] = Mantle;
    s.Colors[ImGuiCol_PlotLines]          = Lavender;
    s.Colors[ImGuiCol_PlotHistogram]      = Blue;
    s.Colors[ImGuiCol_TableHeaderBg]      = Mantle;
    s.Colors[ImGuiCol_TableBorderStrong]  = Surface1;
    s.Colors[ImGuiCol_TableBorderLight]   = Surface0;
    s.Colors[ImGuiCol_TableRowBg]         = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_TableRowBgAlt]      = ImVec4(Surface0.x, Surface0.y, Surface0.z, 0.35f);

    // Misc
    s.Colors[ImGuiCol_DragDropTarget]     = Yellow;
    s.Colors[ImGuiCol_NavHighlight]       = Lavender;
    s.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(Lavender.x, Lavender.y, Lavender.z, 0.3f);
    s.Colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.5f);
    s.Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
}

// -----------------------------------------------------------------------------
// HandleMessage: process a window message. Returns true if consumed.
// Handles WM_SIZE (resize swap chain + recreate render target) and
// WM_GETMINMAXINFO (enforce a minimum window size).
// -----------------------------------------------------------------------------
bool RenderBackend::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_CLOSE:
        if (hWnd == m_mainWindow && !m_allowClose) {
            m_closeRequested = true;
            return true;
        }
        return false;
    case WM_SIZE:
        if (m_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            const HRESULT resized = m_pSwapChain->ResizeBuffers(0,
                                                                (UINT)LOWORD(lParam),
                                                                (UINT)HIWORD(lParam),
                                                                DXGI_FORMAT_UNKNOWN,
                                                                0);
            if (FAILED(resized)) throw std::runtime_error("调整 DXGI 交换链大小失败");
            CreateRenderTarget();
        }
        return true;
    case WM_GETMINMAXINFO: {
        // Enforce a minimum window size so docked panels remain usable.
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 1024;
        mmi->ptMinTrackSize.y = 640;
        return true;
    }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Static WndProc shim: dispatches to the active RenderBackend instance.
// -----------------------------------------------------------------------------
LRESULT CALLBACK RenderBackend::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    try {
        if (g_instance && g_instance->HandleMessage(hWnd, msg, wParam, lParam))
            return 0;  // consumed

        switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    } catch (const std::exception& exception) {
        videosc::dedup::ApplicationErrorLogger::Write(
            {"fatal", "callback_exception", "VideoScGUI", "win32_wndproc",
             exception.what(), "std::exception", {}, 0});
        std::terminate();
    } catch (...) {
        videosc::dedup::ApplicationErrorLogger::Write(
            {"fatal", "callback_exception", "VideoScGUI", "win32_wndproc",
             "Win32 窗口回调发生未知异常", "unknown_exception", {}, 0});
        std::terminate();
    }
}
