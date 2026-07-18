# ImGui 子窗口跨原生窗口拖出与磁吸停靠修改计划

> 日期：2026-07-16  
> 状态：已完成修改；Debug/Release 构建及测试通过

## 1. 现状结论

1. 项目已经使用 Dear ImGui `1.92.9 WIP` Docking 分支，并启用了 `ImGuiConfigFlags_DockingEnable`。
2. 主界面已经创建 `MainDockSpace`，现有“设置”“扫描任务”“扫描路径”“重复报告”等 `ImGui::Begin()` 功能窗口支持在主窗口内部停靠。
3. 项目内置的 Win32 与 DirectX 11 ImGui 后端已经包含 Multi-Viewport 支持，不需要升级 ImGui 或引入新依赖。
4. 当前没有启用 `ImGuiConfigFlags_ViewportsEnable`，渲染循环也没有更新和绘制平台窗口，因此功能窗口不能脱离主 Win32 窗口成为独立原生窗口。
5. 当前布局已经保存到可执行文件同目录的 `imgui_layout.ini`，可继续保存独立窗口的位置、尺寸及停靠关系。

## 2. 修改目标

1. 允许用户拖动 ImGui 功能窗口标题栏，将其从主窗口拖出，创建独立的顶级 Win32 窗口。
2. 拖出的窗口不设置主窗口为 Win32 父窗口，可独立移动、缩放、最小化和跨显示器放置。
3. 独立窗口仍支持 ImGui Docking 磁吸目标：
   - 可重新停靠回主窗口的 `MainDockSpace`。
   - 可与其他拖出的功能窗口合并为标签页。
   - 可在其他浮动窗口的上、下、左、右进行分区停靠。
4. 保存独立窗口的位置、尺寸、显示器和停靠关系，下次启动时恢复。
5. 配置界面的“重置初始布局”继续有效，清除独立窗口布局并恢复默认主窗口停靠结构。

## 3. 计划修改

### 3.1 启用 Multi-Viewport

修改 `VideoScGUI/RenderBackend.cpp` 的 ImGui 初始化：

1. 增加 `ImGuiConfigFlags_ViewportsEnable`。
2. 明确设置 `ConfigViewportsNoDefaultParent = true`，确保辅助窗口是独立顶级 Win32 窗口。
3. 保留 ImGui 自绘标题栏与现有主题，避免原生标题栏和 ImGui 标题栏重复。
4. 保持自动合并开启，使窗口拖回主窗口区域时能够重新磁吸停靠。

### 3.2 补充多窗口渲染循环

修改 `RenderBackend::RenderAndPresent()`：

1. 完成主视口 `ImGui::Render()` 和 DirectX 11 绘制后，调用 `ImGui::UpdatePlatformWindows()`。
2. 调用 `ImGui::RenderPlatformWindowsDefault()`，由现有 Win32/DX11 后端为各辅助窗口创建交换链、渲染并提交。
3. 仅在 `ViewportsEnable` 已启用时执行多视口流程，保留明确的功能开关边界。
4. 主窗口继续使用当前交换链和垂直同步逻辑，不改动业务帧循环。

### 3.3 功能窗口停靠行为

1. 保持现有 `ImGui::Begin()` 功能窗口为可停靠窗口，不增加 `ImGuiWindowFlags_NoDocking`。
2. 主 DockSpace 宿主窗口继续禁止被其他窗口停靠或拖动。
3. 动态标题的“重复组详情”窗口使用稳定的隐藏 ID，避免标题内容变化导致布局记录失效。
4. 模态异常弹窗继续依附其打开时所在视口，不允许作为普通功能窗口参与停靠。

### 3.4 布局保存与重置

1. 继续使用现有 `imgui_layout.ini`，不新增第二份布局配置文件。
2. 确认辅助视口的位置、尺寸、DockNode 和显示器信息能够自动写入布局。
3. 执行“重置初始布局”时：
   - 清除旧布局和辅助视口位置。
   - 销毁不再使用的浮动 DockNode。
   - 把默认功能窗口重新放回主 DockSpace。
4. 窗口位于已移除显示器时，启动后自动限制到当前可用显示器工作区，避免窗口不可见。

## 4. 已确认范围

1. 本次仅支持现有由 `ImGui::Begin()` 创建的功能窗口拖出，包括扫描任务、扫描路径、重复报告、重复组详情、设置及诊断工具窗口。
2. `ImGui::BeginChild()` 创建的父窗口内部区域不属于本次范围，继续隶属于原功能窗口。
3. 不拆分“重复报告”中的 `all_report_groups` 滚动容器，不改变重复报告界面的职责和内部布局。
4. 不新增业务窗口、不调整现有窗口显示入口，只扩展现有窗口的原生多视口与停靠能力。

## 5. 验证计划

1. 构建 Debug/Release x64。
2. 分别将设置、扫描任务、重复报告和重复组详情拖出主窗口，确认生成独立 Win32 窗口。
3. 验证辅助窗口可跨显示器移动、缩放、最小化，并且关闭主程序时全部正常退出。
4. 验证两个浮动功能窗口之间可标签合并和四方向分区停靠。
5. 验证浮动窗口可重新磁吸停靠到主 DockSpace。
6. 重启程序，确认独立窗口位置、尺寸和停靠关系恢复。
7. 点击“重置初始布局”，确认浮动窗口回到默认主窗口布局。
8. 验证扫描、缩略图预览、重复报告和异常弹窗在辅助视口中正常渲染，无 DirectX 设备或交换链异常。

## 6. 执行结果

1. 已启用 `ImGuiConfigFlags_ViewportsEnable`，现有 `ImGui::Begin()` 功能窗口可拖出主窗口成为独立 Win32 顶级窗口。
2. 已显式启用无默认 Win32 父窗口模式，拖出的窗口不再隶属于主窗口。
3. 已在每帧主视口绘制后调用 `UpdatePlatformWindows()` 与 `RenderPlatformWindowsDefault()`，辅助窗口由现有 Win32/DX11 后端创建、绘制和提交。
4. 保留现有 Docking 与自动合并行为，浮动功能窗口可相互磁吸停靠，也可重新停靠回主 DockSpace。
5. 继续使用现有 `VideoScGUI.layout.ini` 保存窗口位置、尺寸及停靠关系；“重置初始布局”逻辑保持不变。
6. 未拆分或修改 `all_report_groups` 等 `BeginChild()` 内部容器。
7. Debug/Release x64 均构建成功，两个配置的 `DedupTests` 均为 33/33 通过。
