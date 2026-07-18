# ImGui 拖出窗口原生最小化、最大化与关闭按钮修改计划

> 日期：2026-07-16  
> 状态：已完成修改；Debug/Release 构建及测试通过

## 1. 问题现象

启用 ImGui Multi-Viewport 后，功能窗口能够从主窗口拖出并成为独立 Win32 窗口，也能继续进行磁吸停靠；但拖出的窗口只有 ImGui 自绘标题栏，没有 Windows 原生的最小化、最大化和关闭按钮。

## 2. 根因

### 2.1 辅助视口仍使用无原生装饰模式

`VideoScGUI/RenderBackend.cpp` 当前启用了：

- `ImGuiConfigFlags_ViewportsEnable`
- `ConfigViewportsNoDefaultParent = true`

但没有修改 `ConfigViewportsNoDecoration`。项目所用 Dear ImGui `1.92.9 WIP` 的默认值为：

```cpp
ConfigViewportsNoDecoration = true;
```

因此 ImGui 会给辅助视口添加 `ImGuiViewportFlags_NoDecoration`。标准 Win32 后端据此使用：

```cpp
WS_POPUP
```

创建辅助窗口。`WS_POPUP` 不包含 `WS_CAPTION`、`WS_SYSMENU`、`WS_MINIMIZEBOX` 和 `WS_MAXIMIZEBOX`，所以不会出现系统标题栏及三个系统按钮。

当 `ConfigViewportsNoDecoration` 设为 `false` 后，现有 Win32 后端会自动改用：

```cpp
WS_OVERLAPPEDWINDOW
```

该样式已经包含标题栏、系统菜单、最小化、最大化、关闭和边框缩放能力，无需修改 `third_party/imgui`。

### 2.2 多数功能窗口没有可写的关闭状态

Win32 后端收到辅助窗口的 `WM_CLOSE` 后不会立即销毁窗口，而是设置：

```cpp
viewport->PlatformRequestClose = true;
```

ImGui 只会在对应 `ImGui::Begin()` 收到非空 `bool* p_open` 时，把该值设为 `false`。

当前只有以下窗口传入了关闭状态：

- 扫描路径
- 设置
- 重复组详情

扫描任务、重复报告和四个诊断窗口均向 `ImGui::Begin()` 传入 `nullptr`。如果只开启 Windows 原生装饰，这些窗口虽然会出现关闭按钮，但点击后无法可靠隐藏，也没有统一的重新打开入口。

## 3. 修改目标

1. 拖出的现有功能窗口显示 Windows 原生标题栏。
2. 原生标题栏提供可用的最小化、最大化/还原和关闭按钮。
3. 最小化窗口时暂停该辅助视口的绘制，恢复后继续正常显示。
4. 最大化窗口时使用当前显示器工作区，并支持再次还原。
5. 点击关闭后隐藏对应功能窗口，不退出整个应用。
6. 已关闭窗口可从主窗口“视图”菜单重新打开。
7. 保留窗口拖出、相互磁吸、停靠回主 DockSpace 和布局持久化能力。
8. 不修改 `third_party/imgui`，不重构 `BeginChild()` 内部容器。

## 4. 计划修改

### 4.1 启用 Windows 原生窗口装饰

修改 `VideoScGUI/RenderBackend.cpp` 的 `InitImGui()`：

1. 在启用 Multi-Viewport 后显式设置：

   ```cpp
   io.ConfigViewportsNoDecoration = false;
   ```

2. 保留：

   ```cpp
   io.ConfigViewportsNoDefaultParent = true;
   ```

   确保拖出的窗口仍然是独立顶级窗口，不重新隶属于主窗口。

3. 保持 `ConfigViewportsNoTaskBarIcon = false`，让独立窗口正常参与任务栏和 Alt+Tab 管理。
4. 继续使用现有 Per-Monitor V2 DPI 配置。当前 Win32 后端已经使用 `AdjustWindowRectExForDpi()` 计算带系统边框的尺寸，不另写窗口尺寸换算。

### 4.2 为功能窗口增加独立显示状态

在 `VideoScApp` 中增加或复用窗口显示状态：

- `m_showScanWindow`
- `m_showScanPathsWindow`
- `m_showDuplicateReportWindow`
- `m_showReportDetailWindow`
- `m_showSettingsWindow`
- `m_showScreenshotWindow`
- `m_showResultsWindow`
- `m_showDiskInfoWindow`
- `m_showFileSearchWindow`

其中扫描路径、设置和重复组详情复用现有字段，其余窗口新增独立字段。

所有可拖出功能窗口的 `ImGui::Begin()` 均传入对应 `bool* p_open`，使 Win32 原生关闭按钮能够通过 ImGui 标准流程隐藏窗口。

主 DockSpace 宿主窗口、模态确认框、错误弹窗和 Tooltip 不属于普通功能窗口，继续保持现有关闭规则。

### 4.3 调整窗口渲染入口

1. `VideoScApp::Render()` 仅在对应显示状态为 `true` 时提交功能窗口。
2. 各窗口渲染方法检查 `ImGui::Begin()` 的返回值：
   - 不可见或折叠时只调用配对的 `ImGui::End()`。
   - 不继续提交耗时 UI 内容和纹理预览。
3. 重复组详情关闭时继续执行现有选中组和临时缩略图清理逻辑。
4. 关闭扫描任务窗口只隐藏界面，不取消正在执行的扫描任务；重新打开后继续显示当前阶段和进度。
5. 关闭重复报告窗口只隐藏界面，不删除已生成报告、选择状态或后台任务结果。

### 4.4 完善“视图”菜单

主窗口“视图”菜单增加全部普通功能窗口的显示开关：

- 扫描任务
- 扫描路径
- 重复报告
- 设置

诊断工具调整为子菜单：

- 视频截图
- 单文件诊断
- 磁盘信息
- 文件检索
- 全部显示
- 全部关闭

重复组详情仍通过点击重复组打开；关闭后可以再次点击对应组重新打开，不在“视图”菜单中保留无选中组的空入口。

### 4.5 默认布局与重置

“重置初始布局”同步恢复窗口显示状态：

- 显示：扫描任务、扫描路径、重复报告、设置。
- 隐藏：重复组详情和四个诊断工具窗口。

继续清除并重建现有 DockNode，不新建布局文件。辅助窗口正常位置和尺寸继续写入 `VideoScGUI.layout.ini`。

最小化和最大化属于当前 Windows 会话状态，不要求写入 ImGui 布局；恢复正常大小后的窗口位置和尺寸继续持久化。

## 5. 标题栏设计说明

启用原生装饰后，辅助窗口顶部会出现 Windows 系统标题栏，用于最小化、最大化、关闭、系统拖动和系统边框缩放。

ImGui 内部标题栏或 Docking 标签栏仍保留，用于窗口磁吸、标签合并和拆分。这是保持现有 Docking 行为的最小安全方案。若强制隐藏 ImGui 标题栏，窗口在重新合并到主视口或单独浮动时可能失去稳定的拖动/停靠入口，因此本次不做自定义无边框标题栏重构。

## 6. 验证计划

1. 构建 Debug/Release x64。
2. 对扫描任务、扫描路径、重复报告、设置和四个诊断窗口分别执行拖出。
3. 检查拖出窗口存在 Windows 原生：
   - 最小化按钮
   - 最大化/还原按钮
   - 关闭按钮
4. 最小化后从任务栏恢复，确认内容、纹理和扫描进度正常。
5. 在不同显示器上最大化，确认窗口限制在对应工作区，不覆盖系统任务栏。
6. 点击关闭，确认仅隐藏对应功能窗口，不退出主程序、不取消后台扫描。
7. 从“视图”菜单重新打开窗口，确认原业务状态仍存在。
8. 把多个窗口磁吸为同一浮动 DockNode 后点击原生关闭，确认该浮动容器中的功能窗口按 ImGui 标准关闭请求一致隐藏。
9. 将窗口重新停靠回主 DockSpace，确认内部停靠、标签页和布局保存仍正常。
10. 点击“重置初始布局”，确认窗口显示状态与默认 Dock 布局同时恢复。
11. 运行 `DedupTests`，确认业务计算、报告和异常边界无回归。

## 7. 执行结果

1. 已设置 `ConfigViewportsNoDecoration = false`，拖出的辅助视口由标准 Win32 后端使用 `WS_OVERLAPPEDWINDOW` 创建，具备原生最小化、最大化/还原和关闭按钮。
2. 继续保持 `ConfigViewportsNoDefaultParent = true`，辅助窗口仍是独立顶级窗口。
3. 显式保持 `ConfigViewportsNoTaskBarIcon = false`，独立窗口可参与任务栏和 Alt+Tab 管理。
4. 扫描任务、扫描路径、重复报告、设置和四个诊断窗口均已绑定独立 `p_open` 状态：
   - 点击原生关闭按钮只隐藏对应功能窗口。
   - 隐藏扫描任务窗口不会取消后台扫描。
   - 重复组详情继续使用既有关闭和临时缩略图清理逻辑。
5. “视图”菜单已增加核心功能窗口开关；“诊断工具”子菜单支持分别显示、全部显示和全部关闭。
6. “重置初始布局”会恢复四个核心窗口并关闭诊断窗口与重复组详情。
7. 未修改 `third_party/imgui`，未拆分任何 `BeginChild()` 容器。
8. Debug/Release x64 均构建成功，两个配置的 `DedupTests` 均为 33/33 通过。
9. Release 链接仍报告项目既有的 `DedupCore.pdb` 类型记录警告，未影响 `VideoScGUI.exe` 生成。
