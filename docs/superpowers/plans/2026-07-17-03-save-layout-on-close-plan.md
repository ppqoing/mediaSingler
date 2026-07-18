# 03－关闭窗口自动保存 GUI 布局修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 布局文件：与配置文件同目录的 `VideoScGUI.layout.ini`

## 1. 修改目标

正常关闭应用时保存主窗口 Dock、现有功能窗口、拖出 viewport 的位置与尺寸；强杀时恢复最近一次周期保存结果。

## 2. 当前实现

- `RenderBackend` 已设置 `io.IniFilename`。
- ImGui 运行中会按自身节奏保存，`DestroyContext()` 也可能保存最终状态。
- 设置页已有“重置初始布局”并立即写盘。
- Win32 `WM_DESTROY` 直接发送 `WM_QUIT`，没有独立的关闭请求和显式保存阶段。

## 3. 修改方案

1. 在 `RenderBackend` 增加幂等 `SaveLayoutNow()`。
2. `WM_CLOSE` 只设置关闭请求，不立即销毁窗口或 ImGui Context。
3. 主循环收到关闭请求后执行：

   ```text
   停止接收新操作
   -> 显式保存布局
   -> 关闭 VideoScApp 后台任务
   -> 再次保存布局
   -> RenderBackend::Shutdown()
   -> 销毁 Win32 窗口
   ```

4. 当 `io.WantSaveIniSettings` 为真或达到固定保存周期时节流调用 `SaveLayoutNow()`。
5. 保存范围包含 ImGui Docking 和 Multi-Viewport 原生 ini 数据，不自定义第二套布局格式。
6. 保存后检查文件存在性和更新时间；失败写入执行日志，并在设置页显示最后错误。
7. “重置初始布局”继续复用同一保存入口。
8. 强杀无法运行关闭回调，因此只承诺恢复最近一次周期保存结果。

## 4. 状态设计

增加 `LayoutPersistenceState`：

```text
layout_path
last_save_utc_ms
last_save_succeeded
save_pending
latest_error
```

GUI 只读取该状态显示，不从文件系统每帧查询。

## 5. 预计修改文件

- `VideoScGUI/RenderBackend.h`
- `VideoScGUI/RenderBackend.cpp`
- `VideoScGUI/main.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`

## 6. 异常边界

- 布局保存失败不得阻止应用释放线程池和数据库。
- ImGui Context 不存在或 ini 路径为空时返回明确失败状态。
- 正常关闭流程必须幂等，避免异常路径重复 Shutdown 崩溃。
- 保存失败写易读文本日志，不抛异常穿过 Win32 WndProc。

## 7. 测试与验收

1. 调整 Dock 比例后正常关闭，重启恢复。
2. 拖出功能窗口、移动和缩放后正常关闭，重启恢复。
3. 最小化、最大化和关闭不同 viewport 后布局一致。
4. 布局目录只读时设置页和日志显示具体失败原因。
5. 强杀后重启恢复最近一次周期保存，而不是空布局。
6. 重置布局后立即关闭，重启仍为初始布局。

## 8. 不在本条范围内

- 不保存报告选择、扫描状态或业务配置到布局文件。
- 不保证强杀前最后一帧布局必然写入。
- 不改变现有默认 DockBuilder 布局。
