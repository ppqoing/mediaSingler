# GUI 布局本地持久化与重置功能计划

> 日期：2026-07-15  
> 状态：已按确认范围执行完成

## 1. 目标

1. 用户调整窗口停靠位置、大小、标签页顺序后，布局自动保存到本地文件。
2. 每次启动 `VideoScGUI` 时自动加载上次布局。
3. 在“设置”界面增加“重置初始布局”按钮。
4. 点击重置后立即恢复项目当前定义的默认 DockBuilder 布局，并覆盖本地布局文件。
5. 布局文件与业务 `config.json` 分离，不修改配置 schema。

## 2. 当前实现与根因

当前 `RenderBackend::InitImGui` 执行：

```cpp
io.IniFilename = nullptr;
```

这会关闭 ImGui 原生 `.ini` 自动加载和自动保存，因此关闭程序后所有窗口位置、大小及 Dock 节点都会丢失。

`VideoScApp::RenderDockSpace` 已经包含默认布局构建逻辑：

- 左侧 30% 为设置、扫描路径和扫描任务。
- 右侧为重复报告。
- 只有 `MainDockSpace` 节点不存在时才调用 DockBuilder 创建默认布局。

因此无需自定义布局序列化格式，只需启用 ImGui 原生持久化，并为重置操作复用现有默认布局构建分支。

## 3. 推荐文件位置

推荐布局文件：

```text
<VideoScGUI.exe 所在目录>\VideoScGUI.layout.ini
```

理由：

- 当前业务配置已经固定保存在可执行文件目录的 `config.json`。
- 使用绝对路径，不受程序启动工作目录影响。
- 文件名与默认 `imgui.ini` 区分，避免和其他 ImGui 工具混淆。
- ImGui 的 Windows 文件实现使用 UTF-8 转 UTF-16，可支持中文安装路径。

若安装目录不可写，ImGui 无法保存布局；当前项目的 `config.json` 同样要求安装目录可写，因此本方案不额外引入 AppData 回退路径。

## 4. 保存与加载生命周期

### 启动

1. `RenderBackend::InitImGui` 创建 ImGui Context。
2. 通过 `GetModuleFileNameW` 取得可执行文件目录。
3. 生成布局文件的绝对 UTF-8 路径，并保存在 `RenderBackend` 成员字符串中，保证 `io.IniFilename` 指针在 ImGui 生命周期内始终有效。
4. 设置 `io.IniFilename` 后，ImGui 在第一次 `NewFrame` 自动加载布局文件。
5. `RenderDockSpace` 检测到已加载的 `MainDockSpace` 时保留用户布局；布局文件不存在时走现有默认布局构建分支。

### 运行中

ImGui 在窗口或 Dock 节点发生变化后，按 `IniSavingRate` 自动写入 `VideoScGUI.layout.ini`，不阻塞每帧渲染。

### 退出

`ImGui::DestroyContext` 会在 Context 已加载设置且 `io.IniFilename` 有效时保存最终布局。布局路径字符串必须晚于 ImGui Context 销毁。

## 5. 重置初始布局

设置界面新增：

```text
界面布局
布局文件  E:\...\VideoScGUI.layout.ini
[重置初始布局]
```

按钮只设置 `m_resetDockLayoutRequested`，下一帧在 `RenderDockSpace` 开始阶段执行：

1. 调用 `ImGui::ClearIniSettings()` 清空窗口、表格和 Dock 设置。
2. 删除当前 `MainDockSpace` 节点。
3. 复用现有 DockBuilder 代码创建初始布局。
4. 恢复初始可见窗口：设置、扫描路径开启，诊断工具关闭。
5. 关闭动态“重复组详情”窗口并清理对应 UI 状态。
6. 调用 `ImGui::SaveIniSettingsToDisk` 立即覆盖布局文件，避免用户立刻退出时重置结果未保存。
7. 设置界面显示“界面布局已恢复为初始状态”和实际布局文件路径。

重置仅影响界面布局，不修改：

- `config.json`。
- 扫描路径和线程配置。
- RocksDB 数据和扫描检查点。
- MySQL 配置。
- 重复报告、删除选择和媒体算法。

## 6. 代码修改范围

### `VideoScGUI/RenderBackend.h`

- 增加持有布局文件 UTF-8 绝对路径的成员字符串。
- 不新增公共业务接口。

### `VideoScGUI/RenderBackend.cpp`

- 新增取得可执行文件目录并生成布局文件路径的私有辅助逻辑。
- 将 `io.IniFilename = nullptr` 改为指向持久成员字符串。
- 初始化失败时保持现有 D3D11 和 ImGui 清理顺序。

### `VideoScGUI/VideoScApp.h`

- 增加 `m_resetDockLayoutRequested` 请求状态。
- 如需避免在 `RenderDockSpace` 内重复代码，增加一个只负责构建默认 Dock 节点的私有方法。

### `VideoScGUI/VideoScApp.cpp`

- 在设置窗口显示当前布局文件路径。
- 增加“重置初始布局”按钮和结果提示。
- 在 `RenderDockSpace` 中消费重置请求，清理旧布局并复用默认 DockBuilder 布局。
- 重置完成后立即保存 `.ini`。

### 不修改范围

- `DedupCore`。
- `config.json` 结构和 `JsonConfigStore`。
- `VideoSc.dll` 与 FFmpeg 接口。
- Win32 主窗口创建参数，除非确认需要连同原生窗口位置和尺寸一起保存。

## 7. 异常处理

- 布局文件不存在：正常创建默认布局，首次产生布局变化后自动保存。
- 布局文件内容不可识别：ImGui 忽略无效条目；`MainDockSpace` 不存在时恢复默认布局。
- 布局文件无法写入：程序继续运行并显示默认或当前布局，不影响业务配置和扫描功能。
- 重置时布局文件写入失败：界面仍保持已经重建的初始布局；ImGui 保存 API 不返回错误码，用户可根据设置页显示的绝对路径检查目录写权限。

## 8. 验收标准

1. 首次启动没有布局文件时显示项目默认布局。
2. 拖动窗口、调整 Dock 比例并正常退出后，生成 `VideoScGUI.layout.ini`。
3. 第二次启动自动恢复窗口停靠位置、尺寸和标签页顺序。
4. 布局加载不依赖当前工作目录。
5. 设置窗口能够查看布局文件绝对路径。
6. 点击“重置初始布局”后，无需重启即可恢复默认左右分栏。
7. 重启程序后仍保持重置后的初始布局。
8. 重置布局不会修改 `config.json`、扫描状态或报告数据。
9. `Debug|x64` 解决方案构建成功，现有 `DedupTests` 全部通过。

## 9. 已确认口径

1. 布局文件与 `config.json` 保存到同一个目录，即 `VideoScGUI.exe` 所在目录。
2. 只保存 ImGui 窗口和 Dock 布局；Win32 主窗口的位置、宽高以及最大化状态保持现有启动规则。

## 10. 执行结果

已完成：

1. `RenderBackend` 在 ImGui Context 生命周期内持有 `VideoScGUI.layout.ini` 的 UTF-8 绝对路径。
2. `io.IniFilename` 已从禁用状态改为指向布局文件，ImGui 启动时自动加载、运行中自动保存、销毁 Context 时保存最终布局。
3. 设置界面新增布局文件绝对路径显示和“重置初始布局”按钮。
4. 重置请求在下一帧 `RenderDockSpace` 中清空 ImGui 设置、删除旧 Dock 节点并复用原有 DockBuilder 默认布局。
5. 重置时恢复设置、扫描路径窗口，关闭诊断工具和动态重复组详情窗口，并立即写回布局文件。
6. 未修改 `config.json` schema、Win32 主窗口状态、扫描数据、报告数据或媒体算法。

验证结果：

- `Debug|x64` 解决方案构建成功。
- `VideoScGUI.exe` 生成成功。
- `DedupTests.exe` 结果为 `29/29 passed`。
- 未自动启动桌面 GUI；首次运行时将在可执行文件目录创建或更新 `VideoScGUI.layout.ini`。
