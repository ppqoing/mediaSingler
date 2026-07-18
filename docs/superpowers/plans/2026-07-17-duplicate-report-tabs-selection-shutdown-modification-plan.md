# 重复报告标签页、选择策略与安全退出修改计划

> 日期：2026-07-17  
> 状态：已执行并完成自动化验证  
> 关联根因：[重复报告标签页、选择操作与退出残留根因文档](2026-07-17-duplicate-report-tabs-selection-shutdown-root-cause.md)  
> 本文记录已确认方案及其执行结果

## 1. 已确认需求

1. “重复报告”窗口拆成两个独立顶层标签：
   - `报告操作`；
   - `重复组结果`。
2. “按条件选择当前组”只处理用户当前选中的一个重复组。
3. “按条件选择所有组”处理当前已发布报告的全部组，不依赖当前滚动位置或 GUI 缓存。
4. “删除选中文件”只消费 RocksDB 中当前报告 generation 的持久选择。
5. 保留策略改成分组显示的非互斥复选框，多项同时选中时按固定优先级逐级比较。
6. 点击主窗口关闭时进入安全退出流程；若正在永久删除，则停止领取新文件，完成当前文件的安全记录后退出。

## 2. 固定实施口径

### 2.1 标签页层级

“重复报告”仍是现有可停靠、可拖出的功能窗口，窗口内部改成：

```text
重复报告
├─ 报告操作
│  ├─ 生成 SHA-512 报告
│  ├─ 生成 dHash 报告
│  ├─ 删除报告
│  └─ 报告任务进度与状态
└─ 重复组结果
   ├─ SHA-512 精确重复
   └─ dHash 内容相似
```

- 顶层标签真正拥有各自内容，不能再把公共内容绘制在 `EndTabBar()` 之后。
- `重复组结果` 内保留 SHA-512/dHash 子标签，用于切换当前报告类型。
- 排序、保留策略、条件选择、选择汇总、永久删除和虚拟化结果列表全部归属 `重复组结果`。
- 报告生成、报告清理、取消生成、任务进度和报告任务消息全部归属 `报告操作`。
- 当前独立的重复组详情功能窗口保持不变，继续由结果列表点击打开。

### 2.2 多选保留策略固定优先级

复选框按以下分组和顺序显示，顺序同时是比较优先级：

1. 路径规则：`保留扫描路径优先级最高`；
2. 质量规则：`保留最高质量`；
3. 时间规则：`保留最新`、`保留最旧`；
4. 体积规则：`保留最大`、`保留最小`。

固定比较顺序为：

```text
路径优先级 > 最高质量 > 最新 > 最旧 > 最大 > 最小 > path_id 稳定决胜
```

- 所有复选框互不排斥，包括“最新/最旧”和“最大/最小”。
- 多项勾选时，只有前一个策略比较相等，才继续执行后一个策略。
- 同时勾选相反策略不会产生不确定结果；优先级靠前者先决胜，后者只处理前者的平局。
- 默认只勾选“保留最高质量”，保持现有默认行为。
- 不允许一个策略都不选；最后一个已选策略取消时拒绝操作并显示原因。
- 本轮不新增配置文件字段，策略选择只属于当前 GUI 会话，避免无需求依据地修改配置 schema。

### 2.3 关闭行为

- 首次点击主窗口关闭按钮：不立即销毁窗口，进入 `正在安全退出` 状态。
- 安全退出期间停止接受新任务，现有编辑和危险按钮全部禁用。
- 窗口继续显示当前关闭阶段和仍在运行的任务。
- 永久删除停止领取新的删除项；正在处理的单个文件必须完成文件结果、删除意图和日志落盘。
- 所有后台任务、同步服务和 RocksDB 安全关闭后，才真正销毁 Win32 窗口并退出进程。
- 不提供直接 `TerminateProcess`、线程 `detach`、删除 RocksDB `LOCK` 或跳过删除意图落盘的快速退出分支。

## 3. 目标状态模型

### 3.1 报告页面状态

在 `VideoScApp` 中增加明确的页面枚举，而不是通过绘制位置隐式表达：

```cpp
enum class DuplicateReportPage {
    Operations,
    Results,
};
```

继续使用 `DuplicateReportKind` 表示结果子标签类型，但页面枚举和报告类型互不复用。

建议拆分私有渲染函数：

- `RenderDuplicateReportOperationsTab()`；
- `RenderDuplicateReportResultsTab()`；
- `RenderReportKindTabs()`；
- `RenderReportSelectionControls()`；
- `RenderReportVirtualizedList()`。

拆分只用于明确内容所有权，不创建额外公共 UI 框架。

### 3.2 保留策略状态

核心层增加值类型 `RetentionPolicySet`，包含六个布尔选择和固定顺序访问方法。GUI 不再保存 `m_keepPolicyIndex`，改为保存一个 `RetentionPolicySet`。

核心比较器改为：

```text
for policy in 固定优先级:
    如果未勾选则跳过
    如果 left/right 在本策略有差异则立即返回
最终按 path_id 稳定决胜
```

`DeletionPlanner::Select()` 接受策略集合，并返回带原因的规划结果，而不是只能用 `optional` 表达全部失败。

建议结果结构：

- `plan`：成功时的删除计划；
- `status`：成功、成员不足、目标磁盘无成员、没有可删成员、策略无效；
- `message`：面向日志和 GUI 的具体原因；
- `matched_target_members`：指定磁盘匹配数量。

### 3.3 当前组身份

增加 `m_selectedReportGroupOrdinal`，与以下字段共同定义当前组：

- 报告类型；
- 报告 generation；
- group ordinal；
- group id。

“按条件选择当前组”使用 ordinal 从 `DuplicateReportStore` 重新读取完整组，再读取当前持久选择，不能遍历 `m_reportGroupCache`。

换报告类型、换 generation、删除报告或清空运行时后，必须同时清空当前组身份和详情窗口。

### 3.4 关闭状态

增加关闭阶段枚举：

```text
Running
CloseRequested
CancellingTasks
FinishingCurrentDeletion
StoppingSync
ClosingRocksDb
ReadyToDestroyWindow
```

`VideoScApp` 提供：

- `RequestShutdown()`：只发取消/停止请求，不阻塞 GUI 线程；
- `AdvanceShutdown()`：每帧推进关闭状态并收取已完成线程；
- `IsShutdownComplete()`：只有所有资源安全收口后返回 true；
- `RenderShutdownOverlay()`：展示阶段和未结束任务。

## 4. 详细修改方案

## 4.1 拆分报告操作与结果标签页

涉及文件：

- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`

修改步骤：

1. 在 `RenderDuplicateReportWindow()` 内创建顶层 `BeginTabBar("duplicate_report_page_tabs")`。
2. 把报告生成、报告清理、生成取消、进度条和报告任务消息移入 `报告操作` 标签。
3. 把报告类型子标签、规则元数据、排序、选择操作、永久删除和虚拟列表移入 `重复组结果` 标签。
4. 保持两个标签各自的滚动区域，结果列表继续使用 `ImGuiListClipper` 动态加载。
5. 报告任务运行时允许进入结果标签查看已发布的旧报告，但会按具体操作禁用可能冲突的写操作。
6. 布局保存继续由 ImGui ini 处理；新增标签选择状态不单独写配置文件。

验收：

- 点击“报告操作”时不绘制几百组结果列表。
- 点击“重复组结果”时不混入报告生成进度和清理确认区。
- SHA-512/dHash 切换只影响结果子标签，不切换顶层页面。

## 4.2 修复三个按钮的可用条件

涉及文件：

- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupCore/dedup/ReportSelectionStore.h`
- `DedupCore/dedup/ReportSelectionStore.cpp`

移除包裹三个按钮的公共 `BeginDisabled(m_reportSummaries.empty() || busy)`，分别计算状态：

| 操作 | 可用条件 | 主要禁用原因 |
|---|---|---|
| 按条件选择当前组 | 当前组身份完整、报告 generation 有效、策略和安全阈值有效、没有报告生成/清理/选择/删除冲突 | 尚未点击一个组、报告已切换、任务冲突 |
| 按条件选择所有组 | generation 有效、摘要非空、策略和安全阈值有效、没有报告生成/清理/选择/删除冲突 | 没有已发布报告、没有重复组、任务冲突 |
| 删除选中文件 | 持久选择数量大于 0、选择来源 generation 等于当前 generation、没有扫描/报告/选择/删除冲突 | 没有持久选择、选择已过期、扫描或删除运行中 |

补充规则：

- 条件选择可与扫描并存，因为只访问已经发布的报告 generation 和独立选择命名空间；永久删除仍禁止与扫描并存。
- 每个禁用按钮提供 tooltip，明确显示唯一主因，不能只显示灰色按钮。
- `StartDeletion()` 保留二次前置检查，GUI 可用条件不能代替执行层安全校验。

为了使删除真正不依赖 GUI 摘要缓存，给 `ReportSelectionStore` 增加流式遍历已选择组的接口。删除线程按选择 generation 枚举 group id，再从 `DuplicateReportStore` 加载对应组；不再捕获 `m_reportSummaries` 作为删除事实来源。

## 4.3 “当前组”改为真正的当前选中组

涉及文件：

- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`

修改步骤：

1. 用户点击重复组标题时保存 group ordinal、group id、报告类型和 generation。
2. 按钮名称改为“按条件选择当前组”。
3. 点击后按 ordinal 从 RocksDB 重新读取组，防止详情副本或可见缓存过期。
4. 生成安全选择并通过 `ReportSelectionStore::SetGroup()` 原子保存。
5. 持久化成功后，才更新详情副本和当前可见缓存。
6. 删除 `ApplyDeletionSelection(false)` 遍历 `m_reportGroupCache` 的旧分支。

结果：滚动位置、虚拟列表裁剪、缓存淘汰和标签切换不会改变“当前组”的含义。

## 4.4 修复安全保留成员判定和状态回滚

涉及文件：

- `DedupCore/deletion/DeletionService.h`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.cpp`

修改步骤：

1. `DeletionPlanner` 使用 `RetentionPolicySet` 和带状态结果，区分不同失败原因。
2. “指定磁盘”输入在比较前执行首尾空白清理和不区分大小写的规范化匹配。
3. 条件选择中，某组没有目标磁盘成员时标记为“目标磁盘无匹配并跳过”，不再显示“无法确定安全保留成员”。
4. 指定磁盘只限制条件批量选择的待删候选，不影响用户手动点击成员时的保留成员判定。
5. 手动切换成员时先在局部副本上完成：
   - 至少保留一个成员检查；
   - 保留成员规划；
   - dHash 安全距离复核；
   - RocksDB `SetGroup()`。
6. 只有全部成功后才提交 GUI 缓存；任何失败都保持原内存状态和原持久状态。
7. GUI 分别显示：
   - 组内成员不足；
   - 指定磁盘无匹配；
   - 没有可删除候选；
   - dHash 距离未严格小于安全上限；
   - RocksDB 保存失败。

## 4.5 保留策略改成非互斥复选框

涉及文件：

- `DedupCore/deletion/DeletionService.h`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

修改步骤：

1. 增加 `RetentionPolicySet` 和固定优先级比较函数。
2. 将 `DeletionPlanner::Select(group, KeepPolicy, ...)` 改为策略集合版本。
3. 可保留单策略兼容重载，仅供现有测试或内部调用过渡；最终 GUI 不再调用旧重载。
4. 删除 `m_keepPolicyIndex` 和 `KeepPolicyFromIndex()`。
5. GUI 按路径、质量、时间、体积四组绘制六个 Checkbox，并显示优先级编号。
6. 默认只启用最高质量。
7. 尝试取消最后一个策略时恢复勾选并提示“至少保留一个策略”。
8. 当前组选择、全报告选择和手动成员切换统一使用同一份策略快照。
9. 后台全报告选择在线程启动时复制策略快照，运行期间修改 GUI 复选框不改变正在执行的任务。

## 4.6 增加安全退出状态机

涉及文件：

- `VideoScGUI/RenderBackend.h`
- `VideoScGUI/RenderBackend.cpp`
- `VideoScGUI/main.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupCore/deletion/DeletionService.h`
- `DedupCore/deletion/DeletionService.cpp`
- `DedupCore/persistence/MySqlSyncService.h`
- `DedupCore/persistence/MySqlSyncService.cpp`
- 必要时调整 `DedupCore/persistence/MySqlBackup.*`

Win32 与主循环：

1. `RenderBackend::WndProc` 拦截主窗口 `WM_CLOSE`，只设置关闭请求，不立即调用默认销毁。
2. 主循环检测关闭请求后调用 `VideoScApp::RequestShutdown()`。
3. 主循环继续渲染关闭遮罩并调用 `AdvanceShutdown()`。
4. `IsShutdownComplete()` 为 true 后保存布局，再显式销毁主窗口。
5. `WM_DESTROY` 仍只负责 `PostQuitMessage(0)`，不在窗口回调中执行阻塞等待。

任务取消与收口：

1. 扫描：调用现有 `Cancel()`，等待 `is_running()` 变为 false 后再 `Wait()`。
2. 报告生成：设置 `m_reportCancelRequested`，线程结束后立即 join。
3. 全报告选择：设置 `m_selectionCancelRequested`，丢弃未发布 staging，保留上一份 active 选择。
4. 永久删除：新增 `m_deletionStopRequested`；执行器在每个文件开始前检查，正在处理的文件完成意图、文件结果、日志和选择更新后停止。
5. 报告清理：不在 RocksDB 当前批次中途退出，完成当前安全批次后停止后续批次。
6. 图片/视频预览：给现有 DLL `shouldCancel/cancelContext` 赋值，关闭时中断 FFmpeg 读取并收取 future。
7. 文件检索：调用现有 `EverythingFileListQuery::Cancel()`。
8. 数据库初始化：在连接、检查、备份和建表阶段之间检查关闭请求；mysqldump 被取消时终止子进程并删除未完成备份文件。
9. MySQL 同步：把 `Stop()` 拆成“请求停止”和“等待结束”，避免 GUI 线程在发出关闭请求的同一帧阻塞。
10. RocksDB：只有全部工作线程停止后才 `Close()`，不删除 `LOCK` 文件。

关闭遮罩至少显示：

- 当前关闭阶段；
- 扫描、报告、选择、删除、预览、数据库、同步是否仍活动；
- 若永久删除正在收口，显示当前文件和“完成当前文件后退出”。

## 5. 数据一致性约束

1. `ReportSelectionStore` 仍是删除选择的唯一持久事实来源。
2. GUI 缓存、详情副本和虚拟列表只渲染持久状态，不能先提交再尝试保存。
3. 当前组选取使用报告 generation + ordinal，不只依赖可能重复或过期的 group id。
4. 全报告选择继续使用 staging generation，取消或关闭时不发布半成品。
5. 删除停止请求只影响尚未开始的文件；当前文件必须完成一致性边界。
6. 关闭 RocksDB 前，所有可能访问 RocksDB 的线程和 future 必须结束。
7. 真正销毁窗口后不得再存在 `VideoScGUI.exe` 的业务收口阶段。

## 6. 实施顺序

### 阶段 1：核心保留策略与诊断结果

1. 增加 `RetentionPolicySet`。
2. 把单策略比较改成固定优先级逐级比较。
3. 让 `DeletionPlanner` 返回详细状态。
4. 增加策略组合、目标磁盘无匹配和稳定决胜测试。

### 阶段 2：持久选择枚举与当前组数据流

1. 给 `ReportSelectionStore` 增加流式选中组枚举。
2. 增加当前 group ordinal 状态。
3. 当前组选择改为重新读取并原子提交。
4. 删除流程改为枚举持久选择，不再依赖 GUI 摘要。

### 阶段 3：报告标签页与按钮可用性

1. 拆出操作标签和结果标签。
2. 迁移控件到对应标签。
3. 替换保留策略 Combo。
4. 分别计算三个按钮的可用条件和禁用提示。

### 阶段 4：安全退出协议

1. 增加 `WM_CLOSE` 请求路径和关闭状态机。
2. 接入扫描、报告、选择、删除和文件检索取消。
3. 接入预览 DLL 取消回调。
4. 拆分 MySQL 同步停止请求与等待。
5. 补充数据库初始化和备份取消检查。
6. 确保布局、日志、选择、删除意图和 RocksDB 顺序收口。

### 阶段 5：构建与验收

1. Debug x64 全解决方案构建。
2. Release x64 全解决方案 Rebuild。
3. Debug/Release `DedupTests` 全部通过。
4. 执行 GUI 人工验收矩阵并记录结果。

## 7. 自动化测试计划

在 `DedupTests/main.cpp` 增加：

1. 默认仅最高质量策略与旧行为一致。
2. 路径优先级高于质量。
3. 质量相同时使用最新决胜。
4. 同时选择最新/最旧时结果稳定且最新优先。
5. 同时选择最大/最小时结果稳定且最大优先。
6. 所有策略比较相同时按 path_id 稳定决胜。
7. 空策略集合被拒绝。
8. 指定磁盘无成员返回明确状态，不返回模糊安全错误。
9. 指定磁盘大小写和首尾空白规范化。
10. 手动选择规划失败不会改变原组和 RocksDB 选择。
11. 持久选择组流式枚举只返回 active selection generation。
12. 全报告选择取消后仍读取上一份完整 active 选择。
13. 删除停止请求不会开始下一个文件，当前文件结果完整。
14. 预览取消回调能结束图像和视频预览任务。
15. MySQL 同步请求停止后能够被等待并安全关闭。

## 8. GUI 人工验收矩阵

### 8.1 标签页

- 报告操作和重复组结果内容完全分离。
- 结果标签中的 SHA-512/dHash 子标签切换正确。
- 切换顶层标签不会丢失持久选择或当前报告 generation。
- 大报告仅在结果标签激活时执行虚拟列表渲染。

### 8.2 按钮与当前组

- 未点击组时仅“按条件选择当前组”禁用，并显示明确 tooltip。
- 点击一个组后只改变该组，不改变同屏其他组。
- 滚动到其他位置后，当前组身份不受缓存淘汰影响。
- 全报告选择覆盖离屏组。
- 有持久选择时删除按钮可用；没有选择时明确禁用。
- 扫描运行时可建立选择，但永久删除保持禁用。

### 8.3 保留策略与安全成员

- 六个策略均可独立或组合勾选。
- 最后一个策略不能取消。
- 界面显示固定优先级。
- 指定磁盘不存在于当前组时显示“目标磁盘无匹配”，不再显示模糊安全错误。
- 失败后重新加载，界面与 RocksDB 选择完全一致。

### 8.4 关闭进程

分别在以下状态点击关闭：

- 空闲；
- 扫描文件；
- 生成 SHA-512 报告；
- 生成 dHash 报告；
- 全报告选择；
- 永久删除；
- 生成图片预览；
- 生成视频六帧预览；
- 初始化数据库；
- MySQL 同步；
- 删除重复报告。

每种状态必须满足：

1. 窗口保持显示“正在安全退出”，不会先消失。
2. 不再领取新任务。
3. 当前安全边界完成后关闭 RocksDB。
4. 窗口真正消失后，任务管理器中不存在 `VideoScGUI.exe`。
5. 重启后没有半发布报告、半发布选择或损坏删除意图。

## 9. 风险与控制

### 风险 1：多策略改变现有保留结果

控制：默认只启用最高质量；单选最高质量必须与现有结果完全一致，组合策略通过固定顺序测试。

### 风险 2：选择按钮与扫描并发写 RocksDB

控制：只访问固定报告 generation 和独立 selection namespace；报告替换仍被选择任务阻断；RocksDB 关闭前等待双方结束。

### 风险 3：关闭时永久删除留下中间状态

控制：只在文件边界检查停止请求；当前文件必须完成 tombstone、文件操作、同步排队和日志，再结束删除线程。

### 风险 4：GUI 关闭状态机卡住

控制：每个活动任务必须发布名称和阶段；所有外部 IO 使用已有超时或新增可取消句柄；不使用无限期、无状态提示的主线程 join。

### 风险 5：删除不再依赖报告摘要后漏组

控制：以 active selection generation 的组索引为枚举来源，并校验 snapshot 的组选中数与实际枚举结果一致。

## 10. 最终验收标准

1. “重复报告”窗口存在独立的“报告操作”和“重复组结果”标签。
2. 结果列表只在结果标签内绘制，并继续使用滚动位置动态加载。
3. 当前组选择只修改明确选中的一个组。
4. 全报告选择覆盖全部持久报告组，不依赖 GUI 缓存。
5. 三个按钮使用各自正确的可用条件，并显示禁用原因。
6. 删除按钮以当前 generation 的持久选择为依据。
7. 六个保留策略是非互斥复选框，并按文档固定优先级稳定决胜。
8. 指定磁盘无匹配时给出明确原因，失败不改变内存和持久选择。
9. 点击关闭后窗口保持显示安全退出进度。
10. 永久删除完成当前文件后停止，不领取新文件。
11. 窗口真正消失后 `VideoScGUI.exe` 已退出。
12. RocksDB 不删除 `LOCK`，所有后台线程在关闭数据库前结束。
13. Debug/Release x64 构建通过，自动化测试全部通过。

## 11. 计划制定阶段边界

- 计划确认前不修改任何业务源码。
- 不改变 MySQL 表结构。
- 不改变现有报告分组算法和 dHash 距离规则。
- 不改变持久选择的安全距离复核规则。
- 不新增强制终止主进程或工作线程的入口。
- 用户确认并明确下达“执行”命令后实施。

## 12. 执行结果

### 12.1 已完成

1. 重复报告窗口已拆分为“报告操作”和“重复组结果”两个顶层标签；SHA-512/dHash 保留为结果页子标签。
2. 当前组、全部报告和永久删除三个按钮已使用独立可用条件，并提供禁用原因提示。
3. 当前组使用 generation、ordinal、group id 从 RocksDB 重新加载后原子保存；全报告选择继续使用 staging generation。
4. 永久删除改为枚举 active selection generation 的持久组选中索引，不再依赖 GUI 摘要缓存。
5. 六项保留策略已改为非互斥复选框，并按固定优先级稳定决胜；指定磁盘匹配已规范化。
6. 手动选择只从未选中的副本中确定安全保留成员，保存失败不会提前修改 GUI 状态。
7. 主窗口 `WM_CLOSE` 已改为安全退出请求；窗口保持显示收口阶段，资源完成关闭后才销毁主窗口。
8. 永久删除只在文件边界响应停止，当前文件完成复核、删除意图、日志和映射消息后才停止。
9. 扫描、报告、选择、文件检索、图片/视频预览和 MySQL 同步均已接入关闭请求。
10. 数据库初始化在连接、查表、备份和建表之间检查关闭请求；mysqldump 可取消并删除未完成备份。

### 12.2 自动化验证

- Debug x64 全解决方案构建：通过。
- Debug `DedupTests`：40/40 通过。
- Release x64 全解决方案 Rebuild：通过。
- Release `DedupTests`：40/40 通过。
- 主窗口关闭烟雾测试：按 `VideoScGUI` 窗口类发送 `WM_CLOSE`，进程以退出码 0 结束。
- Release 构建仍输出既有的 `pwsh.exe` 未找到提示，但 MSBuild 返回 0，所有 Release 目标均已生成。

### 12.3 未自动化的人工场景

涉及真实大报告、长时间扫描、真实 MySQL/mysqldump 和正在永久删除文件的交互场景仍需按第 8 节在实际数据环境中验收；核心停止边界、持久选择枚举和策略组合已由单元测试覆盖。
