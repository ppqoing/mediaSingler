# 重复报告标签页、选择操作与退出残留根因文档

> 日期：2026-07-17  
> 状态：根因与实施口径已确认  
> 范围：重复报告结果页、条件选择、保留策略、安全保留成员、主窗口关闭  
> 本文只记录当前源码事实与根因，不包含修改计划，不执行业务代码修改

## 1. 结论摘要

1. 当前的“SHA-512 精确重复”和“dHash 内容相似”标签只负责切换一个共享的报告类型字段，报告操作区、选择区和结果列表全部绘制在标签栏外，因此并不是相互独立的结果标签页。
2. “按条件选择当前可见组”“按条件选择全部报告”“删除选中文件”被同一个 `m_reportSummaries.empty() || busy` 条件整体禁用；删除按钮错误地依赖当前摘要列表，而不是持久化选择数量。
3. “按条件选择当前可见组”并不处理当前选中的一个组，而是遍历上一帧仍在缓存中的可见组；按钮名称、用户预期和数据作用域不一致。
4. “当前组无法确定安全保留成员”主要由“指定磁盘”没有精确匹配组内任何成员引起。当前代码把“目标磁盘无匹配”“组数据不足”等不同原因合并为同一条提示，并且失败前已经改动了内存中的选择状态。
5. 保留策略当前由一个 `int` 索引映射到一个 `KeepPolicy` 枚举，GUI 使用单选下拉框，核心规划器也只接受一个策略；所以现有模型从数据结构上就不支持多选组合。
6. 点击主窗口关闭按钮后，Win32 窗口会先销毁并退出消息循环，随后 `VideoScApp` 析构函数同步等待后台线程、预览任务、扫描任务和同步线程。窗口已消失但这些等待尚未完成时，任务管理器中仍会看到 `VideoScGUI.exe`，这就是“有时不退出”的直接原因。

## 2. 问题一：重复组结果没有真正位于独立标签页

### 2.1 当前实现

- `VideoScApp::RenderDuplicateReportWindow()` 创建一个名为“重复报告”的 ImGui 功能窗口。
- 窗口顶部创建 `report_kind_tabs`，两个 `BeginTabItem()` 内只修改：
  - `m_visibleReportKind`；
  - `m_selectionCoversEntireReport`；
  - `m_loadReportRequested`。
- 两个 `EndTabItem()` 和 `EndTabBar()` 结束后，才统一绘制报告生成按钮、删除报告按钮、排序、保留策略、条件选择、删除按钮以及全部重复组列表。

代码证据：

- `VideoScGUI/VideoScApp.cpp:3680-3707`：标签项只切换共享状态。
- `VideoScGUI/VideoScApp.cpp:3709-4122`：全部操作区和结果列表位于标签栏外。
- `VideoScGUI/VideoScApp.h:398-407`：精确报告与相似报告共用同一套可见 generation、摘要、排序、缓存和选择快照字段。

### 2.2 根因

当前标签实现采用“共享页面 + 类型切换器”，而不是“标签拥有自己的页面内容”。因此：

- 两类报告没有独立的结果容器；
- 切换标签会清理并重新加载同一套共享模型；
- 结果区不能作为独立标签单独布局、保持滚动位置或独立显示。

这不是 ImGui 标签组件失效，而是页面内容的所有权放在了标签栏外。

## 3. 问题二：条件选择与删除按钮无法点击

### 3.1 共同禁用条件过宽

三个按钮被同一个禁用块包裹：

```cpp
ImGui::BeginDisabled(m_reportSummaries.empty() || busy);
```

其中 `busy` 同时包含：

- 报告生成；
- 报告删除；
- 永久删除；
- 全报告选择；
- 扫描任务。

代码证据：

- `VideoScGUI/VideoScApp.cpp:3709-3711`：`busy` 的组成。
- `VideoScGUI/VideoScApp.cpp:3942-3950`：三个按钮共用禁用条件。

### 3.2 三个按钮实际需要的前置条件不同

- 选择当前组：需要当前选中的组、有效报告 generation、运行时数据库可用，并且没有冲突任务。
- 选择全部组：需要报告摘要和有效 generation，并且没有冲突任务。
- 删除选中文件：应以 `m_reportSelectionSnapshot.selected_file_count > 0` 和删除流程是否空闲为主要条件，不应被 `m_reportSummaries.empty()` 直接禁用。

当前把三种操作绑定到同一条件，造成以下可复现场景：

1. 报告切换后 `LoadReport()` 会先执行 `ClearLoadedReport()`，摘要重新加载前按钮全部禁用。
2. 报告摘要加载失败、旧 dHash 报告缺少元数据或尚未加载时，三个按钮全部禁用。
3. RocksDB 中已经有持久化选择，但当前摘要列表为空时，“删除选中文件”仍无法点击。
4. 扫描运行时，即使用户只想处理已经发布且冻结的历史报告，三个按钮也会一起禁用。

### 3.3 “当前组”作用域实现错误

`ApplyDeletionSelection(false)` 并没有读取 `m_selectedReportGroup`，而是遍历 `m_reportGroupCache`，并通过 `last_used_frame` 选择最近一帧可见的缓存组。

代码证据：

- `VideoScGUI/VideoScApp.cpp:2234-2279`：非全报告分支遍历缓存组。
- `VideoScGUI/VideoScApp.cpp:2080-2087`：缓存按渲染帧淘汰。
- `VideoScGUI/VideoScApp.cpp:4072-4077`：真正的当前选中组保存在 `m_selectedReportGroup`。

因此当前按钮实际语义是“选择上一帧可见区域中的若干组”，既不是“当前选中组”，也不是稳定定义的“当前页面”。滚动、缓存淘汰和按钮所在位置都会影响实际作用范围。

## 4. 问题三：显示“当前组无法确定安全保留成员”

### 4.1 提示触发点

手动点击组内成员时，`ToggleReportMemberDeletion()` 会先修改 `group.selected_for_deletion`，然后调用：

```cpp
DeletionPlanner::Select(group, KeepPolicyFromIndex(m_keepPolicyIndex), targetStorage)
```

规划器返回空值后显示“当前组无法确定安全保留成员”。

代码证据：

- `VideoScGUI/VideoScApp.cpp:3612-3624`：先修改内存选择。
- `VideoScGUI/VideoScApp.cpp:3626-3635`：再按策略规划，失败后直接返回。

### 4.2 规划器返回空值的实际条件

`DeletionPlanner::Select()` 只有以下几类空结果：

1. 组内成员少于 2 个。
2. 应删除成员为空。
3. 应删除成员数量不安全地覆盖整组。

代码证据：

- `DedupCore/deletion/DeletionService.cpp:215`：成员不足。
- `DedupCore/deletion/DeletionService.cpp:217-239`：指定磁盘过滤与保留成员计算。
- `DedupCore/deletion/DeletionService.cpp:240-242`：空删除集或整组删除被拒绝。

正常重复报告组至少有两个成员，所以现场最可能的稳定触发条件是：

- “指定磁盘”输入非空；
- 输入值与组内所有 `member.storage_target_key` 都不完全相等；
- 规划器随后只允许删除指定磁盘中的成员，最终得到空删除集并返回失败。

当前匹配是 `std::wstring` 完全相等，没有规范化别名、路径、盘符或大小写，也没有在界面展示当前组实际的 `storage_target_key` 候选，因此用户很容易输入一个看似正确但不匹配的值。

### 4.3 附带状态一致性问题

规划失败发生在 `group.selected_for_deletion` 已被增删之后，失败分支没有回滚原值，也没有写入 `ReportSelectionStore`。因此可能出现：

- 当前缓存模型显示状态已经变化；
- RocksDB 持久选择没有变化；
- 重新滚动、重新加载或重启后界面状态又恢复。

这会让“无法确定安全保留成员”看起来像随机失败，实际是内存状态与持久状态短暂分叉。

## 5. 问题四：保留策略不能多选

### 5.1 当前数据结构天然互斥

- GUI 只有 `int m_keepPolicyIndex`。
- `KeepPolicyFromIndex()` 只返回一个枚举值。
- 页面用 `ImGui::Combo()` 单选。
- `DeletionPlanner::Select()` 参数也是单个 `KeepPolicy`。

代码证据：

- `VideoScGUI/VideoScApp.h:461-464`：单个策略索引。
- `VideoScGUI/VideoScApp.cpp:354-365`：索引到唯一策略的映射。
- `VideoScGUI/VideoScApp.cpp:3916-3925`：单选下拉框。
- `DedupCore/deletion/DeletionService.h:21-29,58-60`：核心接口只接受一个策略。
- `DedupCore/deletion/DeletionService.cpp:168-187`：比较器一次只执行一个策略。

### 5.2 尚未定义的组合语义

现有策略中存在天然冲突的组合：

- 保留最新 与 保留最旧；
- 保留最小 与 保留最大。

所以仅把 Combo 换成多个 Checkbox 还不足以形成确定算法。必须先确定多选策略是：

1. 按固定优先级作为逐级排序条件；或
2. 所有勾选条件同时满足；或
3. 任一条件满足即可成为保留候选，再通过另一个规则决胜。

若没有明确组合语义，同一组可能存在多个互相矛盾的“应保留成员”，永久删除不能安全执行。

## 6. 问题五：窗口关闭后进程有时仍未退出

### 6.1 窗口销毁与业务退出没有协调阶段

当前 Win32 回调只在 `WM_DESTROY` 中调用 `PostQuitMessage(0)`；没有拦截 `WM_CLOSE`，也没有在窗口消失前通知 `VideoScApp` 进入关闭状态。

主循环收到 `WM_QUIT` 后立即停止渲染，随后才销毁 `VideoScApp`。

代码证据：

- `VideoScGUI/RenderBackend.cpp:435-445`：关闭消息直接走默认销毁，`WM_DESTROY` 只投递退出消息。
- `VideoScGUI/main.cpp:140-155`：收到 `WM_QUIT` 后离开主循环，再触发 `VideoScApp` 析构。

### 6.2 析构函数存在多处无期限同步等待

`VideoScApp::~VideoScApp()` 会依次：

1. 请求取消报告生成和全报告选择；
2. `join()` 报告生成线程；
3. `join()` 报告清理线程；
4. `join()` 全报告选择线程；
5. `join()` 永久删除线程；
6. `ShutdownRuntime()` 取消并等待扫描、停止并等待 MySQL 同步、关闭 RocksDB；
7. `CleanupReportPreviewDirectory()` 等待所有图片和视频预览 `std::future`；
8. `join()` 数据库初始化线程。

代码证据：

- `VideoScGUI/VideoScApp.cpp:2672-2689`：析构等待顺序。
- `VideoScGUI/VideoScApp.cpp:1279-1294`：扫描、同步与 RocksDB 关闭等待。
- `VideoScGUI/VideoScApp.cpp:644-652`：预览任务逐项 `wait()`。

其中仅报告生成、全报告选择和扫描具备明确取消请求。以下任务没有统一取消协议：

- 报告清理；
- 永久删除；
- 数据库初始化；
- 图片/视频预览异步任务；
- 正在进行的 MySQL 调用或外部进程等待。

因此当这些任务处于磁盘 IO、FFmpeg、MySQL、mysqldump 或 RocksDB 批量操作中时，窗口先消失，进程继续等待任务自然完成。“有时”取决于关闭瞬间是否存在这些活动任务。

### 6.3 外部崩溃报告器不是主进程残留根因

`VideoScCrashReporter.exe` 会附加并监视主进程，但它在收到主进程 `EXIT_PROCESS_DEBUG_EVENT` 后结束。它可能作为另一个进程出现在任务管理器中，但不会替代上述 `VideoScGUI.exe` 析构等待链路。

代码证据：

- `VideoScGUI/diagnostics/CrashHandler.cpp:253-281`：启动外部报告器。
- `VideoScCrashReporter/main.cpp:128-161`：收到目标进程退出事件后结束。

## 7. 根因之间的关联

这五项并非完全独立：

- 结果列表与操作区共用一个页面和一套状态，使标签切换、摘要清空、按钮禁用和当前组作用域互相影响。
- 保留策略只有单值模型，使条件选择和手动选择都依赖同一个单策略规划器。
- 手动选择先改缓存再规划，放大了安全保留失败后的界面/数据库状态差异。
- 关闭流程没有独立状态机，页面消失后无法继续展示“正在取消”“正在完成当前删除”或具体阻塞任务。

因此后续修改不能只更换控件外观；需要先拆分页面状态、操作可用性和关闭生命周期，但仍应保持 RocksDB 选择记录为唯一持久事实来源。

## 8. 需要用户确认的实施口径

以下两点无法仅从当前源码唯一确定，生成修改计划前需要确认：

1. “独立标签页”是指：
   - 在“重复报告”窗口内拆成“报告操作”和“重复组结果”两个标签，结果标签内再切换 SHA-512/dHash；还是
   - 把“SHA-512 重复结果”和“dHash 重复结果”分别做成两个独立顶层标签页。
2. 多个保留策略同时勾选时，冲突策略如何组合。建议采用“按界面顺序逐级比较”的稳定优先级模型，并禁止同时勾选语义相反的两项；如果相反项也必须允许同时勾选，则还需要定义谁优先。
3. 关闭过程中若正在永久删除，建议停止领取新的删除项，完成当前文件的安全落盘和意图记录后退出；不建议直接终止进程。需要确认是否采用该安全退出规则。

## 9. 本轮未执行内容

- 未修改 GUI、选择逻辑、删除逻辑、线程或 RocksDB 代码。
- 未改变任何配置字段或持久化格式。
- 未运行会生成、选择或删除真实媒体文件的操作。
- 等待用户确认根因正确后，再生成独立修改计划。
