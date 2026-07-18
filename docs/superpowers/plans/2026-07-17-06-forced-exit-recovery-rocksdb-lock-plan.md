# 06－强制退出恢复与 RocksDB 锁安全修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 核心原则：不删除 RocksDB `LOCK` 文件，不发布半成品，不重复危险删除

## 1. 修改目标

处理中途强杀后，下次启动能够：

- 继续提供可恢复扫描。
- 清理未发布报告和选择半成品。
- 恢复删除映射事务。
- 优先处理历史待同步数据。
- 安全取得 RocksDB 独占使用权。
- 明确显示上次任务被中断及恢复结果。

## 2. 当前实现

- `ScanCheckpointStore` 已保存扫描阶段和冻结配置。
- 报告使用 generation 与 active 指针，正常取消由 RAII 清理工作键。
- `DeletionExecutor` 已持久化 tombstone 并提供 `RecoverPendingDeletes()`。
- `RocksStore::Close()` 会销毁 Column Family handle 和数据库实例。
- 强杀时析构函数不会运行，报告 work 键和任务状态可能残留。
- 当前没有按 RocksDB 目录隔离的单实例保护和统一操作租约。

## 3. 持久化任务记录

新增 `TaskRecoveryStore`，在 `Checkpoints` Column Family 保存：

```text
runtime-task/<task-type>/<task-id>
```

记录字段：

```text
record_version
task_type
task_id
state
stage
scan_id / generation_id / deletion_batch_id
configuration_snapshot
started_utc_ms
heartbeat_utc_ms
completed_utc_ms
last_error
```

状态使用 `Running / Committing / Completed / Cancelled / Failed / Interrupted`。任务开始、阶段切换、提交前和结束时同步写入关键状态。

## 4. 单实例和 RocksDB 锁

1. 根据规范化 RocksDB 目录生成稳定哈希和 Windows named mutex。
2. 同一数据库目录只允许一个实例持有 mutex。
3. 取得 mutex 后才调用 `RocksStore::Open()`。
4. 打开失败时有限次数重试，用于等待强杀后的系统句柄释放。
5. 已取得 mutex 但仍报告锁占用时，记录详细错误并停止启动。
6. 不删除 `LOCK` 文件，不调用自动 `RepairDB` 或破坏性重建绕过互斥。

## 5. RocksStore 生命周期闸门

1. 每个公开数据库操作获取操作租约。
2. `BeginShutdown()` 后拒绝新操作。
3. `Close()` 等待活动租约归零，再销毁 handle 和数据库。
4. `ForEachPrefix` 在整个迭代期间持有租约，所有异常路径销毁 iterator。
5. 禁止持有 RocksDB 租约等待线程池 `join`，关闭顺序必须由外层协调。

## 6. 启动恢复顺序

```text
取得 named mutex
-> 正常打开 RocksDB
-> 标记遗留 Running/Committing 为 Interrupted
-> 恢复 deletion tombstone
-> 发布历史 staged/pending MySQL 操作
-> 清理未发布报告 generation/work/candidate index
-> 清理选择 staging generation
-> 加载可恢复扫描
-> 启动同步服务
```

## 7. 各任务恢复策略

- 扫描：保留 checkpoint 与本地已完成能力，由用户选择恢复；重新规划时排除完整结果。
- 报告：不续跑半个报告；保留上一份 active generation，分批幂等清理半成品。
- 报告删除：前缀删除支持重复执行，重启继续清理。
- 选择全部：只发布完整 selection generation；强杀时丢弃 staging。
- 永久删除：依据 tombstone、文件存在性和期望 SHA-512 补齐映射删除，禁止盲目再次删除不同内容。
- 布局：恢复最近一次周期保存文件。

## 8. 正常关闭顺序

```text
停止接收 GUI 命令
-> 请求扫描/报告/选择/删除安全停止
-> 停止三个线程池并 join
-> 停止 MySQL 同步服务
-> 销毁所有 RocksDB 使用者
-> RocksStore::Close()
-> 释放 named mutex
```

## 9. 预计修改文件

- 新增 `DedupCore/persistence/TaskRecoveryStore.h/.cpp`
- `DedupCore/persistence/RocksStore.h/.cpp`
- `DedupCore/orchestration/ScanCoordinator.cpp`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `VideoScGUI/main.cpp`
- `DedupTests/main.cpp`

## 10. 日志与界面

- 文本日志记录上次任务 ID、最后阶段、最后心跳、检测时间和恢复动作。
- GUI 显示“上次扫描可恢复”“报告半成品已清理”“删除映射已恢复”等独立结果。
- 锁占用显示数据库目录和错误，不建议用户手工删除文件。

## 11. 测试与验收

1. 扫描各阶段强杀后可恢复。
2. 报告枚举、校验、分组和发布前强杀后，上一份 active 报告不变。
3. 选择全部中强杀后仍保留上一份完整选择。
4. 删除前、文件已删但映射未排队、映射已排队三个位置强杀后恢复正确。
5. 第二实例打开同一目录被拒绝。
6. 强杀后无需删除 `LOCK` 文件即可重新打开。
7. 数据库确实被其他进程占用时不会绕过锁。
8. 正常关闭时活动数据库操作归零后才关闭。

## 12. 不在本条范围内

- 不自动修复实际损坏的 RocksDB。
- 不承诺恢复半个 dHash 报告的计算进度。
- 不通过删除 `LOCK` 文件解决锁错误。
