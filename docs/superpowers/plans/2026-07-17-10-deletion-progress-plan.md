# 10－永久删除操作进度条修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 范围：永久删除文件及 MySQL 映射删除排队

## 1. 修改目标

永久删除过程中实时显示当前阶段、已处理文件、总文件、已处理字节、总体积、成功、跳过、失败和已释放大小。

## 2. 当前实现

- `DeletionExecutor::Execute()` 按组串行复核 SHA-512 并删除。
- 删除意图先写 RocksDB tombstone，文件删除后再排队 MySQL 映射删除。
- GUI 只显示“正在删除”和最终数量，没有实时进度快照。
- 全报告删除当前可能在执行阶段重新按策略生成选择。

## 3. 进度模型

新增 `DeletionProgressSnapshot`：

```text
task_id
state
stage
stage_index / stage_count
selected_files / selected_bytes
processed_files / processed_bytes
deleted_files / reclaimed_bytes
skipped_files / failed_files
current_group_id / current_path
latest_error
```

建议阶段：

1. `loading_selection`
2. `verifying_retained_files`
3. `verifying_delete_candidates`
4. `deleting_files`
5. `queueing_mapping_deletes`
6. `completed`

## 4. Core 修改

1. `DeletionExecutor::Execute()` 增加线程安全进度回调。
2. 在保留文件复核、每个待删文件复核、`DeleteFileW` 和映射排队后发布进度。
3. 删除总数和总体积读取持久化选择 metadata。
4. 删除执行器只消费已发布选择，不在执行时生成另一套策略结果。
5. 成功删除后累计实际 `reclaimed_bytes`；跳过和失败不计入释放量。
6. 每次文件处理结果继续写操作日志和失败日志。

## 5. GUI 修改

1. 增加 `DeletionViewState`，后台线程只更新快照。
2. 使用反色文字进度条显示 `processed_files / selected_files`。
3. 同时显示已处理体积、总选择体积、已释放体积和失败数。
4. 当前路径使用单行裁剪和 Tooltip，不自动换行撑高界面。
5. 删除确认窗口显示准确的选择文件数和总体积。
6. 本条不增加“取消永久删除”按钮。

## 6. 中断恢复

- 强杀后 `TaskRecoveryStore` 将删除任务标记为 Interrupted。
- 启动时先恢复 tombstone，再更新恢复结果。
- 已删除但映射未排队的文件只补映射删除。
- 文件仍存在时重新复核期望 SHA-512，内容变化则跳过并记录失败。

## 7. 预计修改文件

- `DedupCore/deletion/DeletionService.h/.cpp`
- `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/persistence/TaskRecoveryStore.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

## 8. 异常边界

- 进度回调异常不得绕过删除审计日志。
- 日志不可写时在危险删除开始前失败。
- GUI 不直接访问 tombstone 或删除内部队列。
- 删除任务状态带 task ID，旧快照不得覆盖新任务。

## 9. 测试与验收

1. 数百文件删除时 GUI 持续更新且不阻塞。
2. 最终满足：成功 + 跳过 + 失败 = 已处理。
3. 已释放大小只统计实际删除成功文件。
4. 权限不足、路径消失、SHA 改变和读取超时均显示并记录原因。
5. 删除不同阶段强杀后恢复正确。
6. 大组删除不会一次性复制整个报告到 GUI。

## 10. 不在本条范围内

- 不并行执行 `DeleteFileW`。
- 不新增删除取消按钮。
- 不改变“至少保留一个副本”的安全规则。
