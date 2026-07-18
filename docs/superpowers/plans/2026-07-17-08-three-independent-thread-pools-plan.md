# 08－三个独立线程池架构修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 线程池：文件读取、媒体计算、dHash 报告

## 1. 修改目标

把本项目主要并行工作迁移到三个互不共享的线程池：

| 线程池 | 最大线程数来源 | 负责内容 |
| --- | --- | --- |
| 文件读取线程池 | `storage.max_concurrent_file_reads` | 文件读取与流式 SHA-512 |
| 媒体计算线程池 | `compute.worker_threads` | 图片 dHash、视频 6 帧、缩略图、FFmpeg 分析 |
| dHash 报告线程池 | `dhash_similarity.validation_worker_threads` | 候选真实距离与只读组兼容性计算 |

三个池不共享队列、线程额度、取消状态和错误状态。

## 2. 当前实现

- `DiskHashScheduler` 为每个磁盘通道创建固定工作线程。
- 媒体阶段临时创建 `worker_threads` 个 `std::thread`。
- dHash 报告内部创建候选校验线程数组和专用队列。
- 各处重复实现取消、异常捕获、join 和队列同步。

## 3. 公共线程池类型

新增：

- `DedupCore/concurrency/TaskThreadPool.h`
- `DedupCore/concurrency/TaskThreadPool.cpp`

核心接口：

```text
Start(maximum_threads, queue_capacity)
Submit(task)
CloseSubmissions()
DrainAndJoin()
CancelPendingAndJoin()
Snapshot()
```

快照字段：

```text
maximum_threads
active_threads
queued_tasks
completed_tasks
failed_tasks
cancelled_tasks
accepting
```

## 4. 公共实现约束

1. 队列必须有界，满时形成可取消背压。
2. 线程池捕获 `std::bad_alloc`、`std::exception` 和未知异常。
3. 保存首个任务错误，并通过失败回调通知业务协调器。
4. 禁止 detach；所有退出路径都必须 join。
5. 析构函数执行幂等取消和 join。
6. 线程池只负责调度，不实现业务重试、RocksDB 事务、CPU/磁盘自适应或 GUI 状态。
7. 任务闭包不得持有生命周期短于线程池的裸对象引用。
8. 新增类型和方法按项目规则补充完整中文职责、参数、返回值、异常和副作用注释。

## 5. 三个实例的生命周期

### 5.1 文件读取线程池

- 在一次扫描进入 SHA-512 阶段前创建。
- 扫描结束、取消或失败时停止。
- 每盘队列由 `DiskHashScheduler` 管理，中央分发器向池提交实际文件任务。
- 每盘动态并发许可由智能磁盘控制器管理，不通过创建新线程扩容。

### 5.2 媒体计算线程池

- 在媒体特征阶段创建。
- 任务结束后清空并 join；阶段切换重新评估 CPU 自适应并发。
- `worker_threads` 是池硬上限；自动计算模式从许可 1 开始逐步放宽。

### 5.3 dHash 报告线程池

- 在相似报告启动时创建。
- 只执行纯候选比较和只读兼容性检查。
- 报告发布或 work 键清理前必须停止并 join。

## 6. 不迁移的长期线程

以下长期控制循环继续使用专用线程：

- GUI 主循环。
- 扫描协调器。
- MySQL 同步服务。
- CPU 采样控制器。
- 磁盘占用采样器。
- 少量根目录发现控制线程。

原因是它们会长期等待事件，不应占用任务线程池工作槽。

## 7. RocksDB 关闭协作

正常关闭顺序：

```text
停止向三个池提交任务
-> Cancel 或 Drain
-> join 三个池
-> 停止同步服务
-> 关闭 RocksDB
```

禁止在线程池仍可能调用 Store 时执行 `RocksStore::Close()`。

## 8. 预计修改文件

- 新增 `DedupCore/concurrency/TaskThreadPool.h/.cpp`
- `DedupCore/DedupCore.vcxproj`
- `DedupCore/scheduling/DiskHashScheduler.h/.cpp`
- `DedupCore/orchestration/ScanCoordinator.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `DedupCore/orchestration/ProgressSnapshot.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 9. 测试与验收

1. 队列容量、背压、Drain、Cancel 和析构行为正确。
2. 任务异常不会终止进程，首错误能传回业务层。
3. 一个池取消或失败不改变另两个池状态。
4. 三个池的活动线程均不超过各自配置。
5. 关闭 RocksDB 前所有池活动线程与排队任务归零。
6. 高负载下不出现死锁、遗漏 join 或无限内存增长。
7. Debug/Release x64 构建和自动化测试通过。

## 10. 不在本条范围内

- 不把所有单个后台线程强制迁入线程池。
- 不让三个池共享全局线程额度。
- 不改变永久删除的串行安全语义。
