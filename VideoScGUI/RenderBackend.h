#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>

// Forward declarations for ImGui backend impls (avoid pulling imgui headers
// into this header — they are only needed in the .cpp).
struct ImGuiIO;

// RenderBackend owns the D3D11 device, swap chain, render target view, and
// the ImGui context + backends. It also hosts the Win32 window procedure.
//
// Lifecycle:
//   1. Init(windowHandle) — creates D3D11 device/swapchain, ImGui context,
//      loads fonts, applies style, inits ImGui backends.
//   2. NewFrame()         — call at the start of each frame (calls the three
//      ImGui _NewFrame functions).
//   3. RenderAndPresent() — renders and presents the main viewport plus any
//      detached platform viewports.
//   4. Shutdown()         — shuts down ImGui backends, destroys ImGui
//      context, releases all D3D11 resources. Also called by destructor.
//
// Static WndProc dispatches to the instance via a stored pointer.
class RenderBackend {
public:
    RenderBackend();
    ~RenderBackend();

    // Non-copyable.
    RenderBackend(const RenderBackend&) = delete;
    RenderBackend& operator=(const RenderBackend&) = delete;

    // Initialize D3D11 + ImGui for the given window. Returns false on failure
    // (D3D11 device creation failed). On failure, partial state is cleaned up.
    bool Init(HWND hwnd);

    // Per-frame: begin a new ImGui frame. Calls ImGui_ImplDX11_NewFrame,
    // ImGui_ImplWin32_NewFrame, ImGui::NewFrame.
    void NewFrame();

    /**
     * @brief 绘制主视口及所有拖出的辅助视口，并提交各自交换链。
     * @param clearColor 主视口渲染目标使用的 RGBA 清屏颜色。
     * @throws std::runtime_error 主交换链提交失败时抛出。
     */
    void RenderAndPresent(const float clearColor[4]);

    // Shutdown ImGui + release all D3D11 resources. Safe to call multiple times.
    void Shutdown();

    /** @brief 正常关闭前立即保存当前主窗口、停靠与独立视口布局。 */
    void SaveLayoutNow() noexcept;

    /** @brief 用户是否已经点击主窗口关闭按钮。 */
    bool CloseRequested() const noexcept { return m_closeRequested; }

    /** @brief 安全退出完成后真正销毁主窗口。 */
    void ConfirmClose() noexcept;

    // Access the D3D11 device (used by VideoScApp::LoadImageToTexture to
    // create textures). Returns nullptr if not initialized.
    ID3D11Device* GetDevice() const { return m_pd3dDevice; }

    // Static Win32 window procedure. Pass to WNDCLASSEXW::lpfnWndProc.
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Handle a window message. Returns true if the message was consumed.
    // Called by the static WndProc shim.
    bool HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void InitImGui(HWND hwnd);
    void LoadFonts();
    void ApplyStyle();

    // D3D11 resources (owned by this instance)
    ID3D11Device*           m_pd3dDevice        = nullptr;
    ID3D11DeviceContext*    m_pd3dDeviceContext = nullptr;
    IDXGISwapChain*         m_pSwapChain        = nullptr;
    ID3D11RenderTargetView* m_mainRTV           = nullptr;
    HWND m_mainWindow = nullptr;

    /** @brief ImGui 布局文件的 UTF-8 绝对路径；其存储期必须覆盖整个 ImGui Context。 */
    std::string m_imguiIniPathUtf8;
    ULONGLONG m_lastLayoutSaveTick = 0;
    bool m_initialized = false;
    bool m_closeRequested = false;
    bool m_allowClose = false;
};
