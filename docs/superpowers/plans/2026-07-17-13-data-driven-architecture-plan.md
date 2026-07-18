# 13－项目数据驱动架构渐进修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 实施方式：渐进迁移，不进行无关模块大重写

## 1. 修改目标

本次涉及的报告、选择、删除、恢复、布局和线程池进度统一使用数据驱动单向状态流：

```text
用户操作 -> Command -> Core Service -> 持久状态/快照 -> ApplicationState -> ImGui 渲染
```

## 2. 当前问题

- GUI 按钮回调会同时修改原子量、缓存、选择 map、消息和窗口状态。
- 部分业务含义依赖“当前是否可见”或缓存是否命中。
- 后台线程直接更新多个 GUI 成员，容易出现旧任务覆盖新任务。
- 报告、选择、删除进度使用不同的状态表达。

## 3. 实施边界

迁移范围：

- 重复报告生成与加载。
- 报告选择。
- 永久删除。
- 强杀恢复。
- 布局保存。
- 三个线程池和磁盘读取进度。

暂不重写：

- DiskInfo DLL 公共 API。
- FFmpeg 媒体算法接口。
- Everything 查询封装。
- 无关诊断工具窗口。

## 4. 单向状态流

1. GUI 按钮只提交 Command。
2. Command Handler 校验当前任务状态和配置冻结边界。
3. Core Service 执行业务并写 RocksDB 或生成快照。
4. 后台任务发布带 task ID/generation ID 的不可变 Snapshot。
5. GUI 主线程消费快照并更新 `ApplicationState`。
6. ImGui 只根据 `ApplicationState` 渲染，不直接读取后台队列或 RocksDB。

## 5. 状态切片

新增或整理：

```text
ReportGenerationViewState
ReportViewState
ReportSelectionViewState
DeletionViewState
RecoveryViewState
LayoutPersistenceState
ThreadPoolViewState
```

公共字段：

```text
task_id / generation_id
state
stage
processed / total / total_known
started_utc_ms / updated_utc_ms
latest_error
```

## 6. Command 边界

```text
StartReportGenerationCommand
CancelReportGenerationCommand
LoadReportCommand
ToggleReportMemberSelectionCommand
ApplyReportSelectionCommand
StartDeletionCommand
SaveLayoutCommand
RecoverInterruptedTasksCommand
```

命令不得持有 ImGui 指针或缓存引用。执行结果通过状态快照返回。

## 7. 事实来源

- 报告组和 metadata：`DuplicateReportStore`。
- 选择和总体积：`ReportSelectionStore`。
- 删除意图：tombstone 与 `TaskRecoveryStore`。
- 扫描恢复：`ScanCheckpointStore`。
- 线程进度：线程池 Snapshot。
- 布局保存状态：`LayoutPersistenceState`。

GUI 缓存、缩略图和纹理属于可丢弃派生数据，不得成为业务事实来源。

## 8. 过期状态防护

- 所有异步状态带任务 ID。
- GUI 只接受当前活动任务或当前报告 generation 的快照。
- 旧线程完成时若 ID 不匹配，只回收资源，不覆盖当前界面。
- 配置编辑态与任务冻结态分离，页面显示当前任务实际使用值。

## 9. 预计修改文件

- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/deletion/DeletionService.h/.cpp`
- `DedupCore/persistence/TaskRecoveryStore.h/.cpp`
- `DedupCore/orchestration/ProgressSnapshot.h`
- `DedupCore/concurrency/TaskThreadPool.h/.cpp`
- `DedupTests/main.cpp`

## 10. 分阶段迁移

1. 先建立状态结构和快照，不改变算法。
2. 报告进度迁移。
3. 选择持久化并移除可见缓存事实来源。
4. 删除与恢复迁移。
5. 布局和线程池状态迁移。
6. 移除重复旧字段和直接跨线程 GUI 写入。

## 11. 测试与验收

1. 清空 GUI 缓存后能从 Store 和 Snapshot 恢复完整状态。
2. UI 是否可见不影响选择和删除结果。
3. 旧任务快照不能覆盖新任务。
4. Core 自动化测试不依赖 ImGui。
5. 同一状态在主列表、详情和确认窗口显示一致。
6. 后台高频更新不阻塞 GUI 主线程。

## 12. 不在本条范围内

- 不创建无业务价值的多层接口和基类。
- 不把所有项目对象放入一个全局可变 Store。
- 不要求一次提交完成全部架构迁移。
