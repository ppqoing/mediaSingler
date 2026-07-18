# 09－按物理盘智能增加文件读取并发修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 默认磁盘读取占用目标：`90%`

## 1. 修改目标

每个物理盘从 1 个读取并发开始；有任务积压且读取占用未达到目标时，每秒增加 1，直到达到目标或线程池/全局硬上限。

## 2. 当前实现

- `NativeFileDiscovery::PrepareRoot()` 已能识别 `PhysicalDriveN`、`PhysicalSet:n,m` 和 HDD/SSD。
- `DiskHashScheduler` 为每个存储目标建立独立队列和固定数量工作线程。
- 所有磁盘共享 `max_concurrent_file_reads` 闸门。
- 当前只有 CPU 自适应计算，没有物理盘读取占用采样。

## 3. 配置修改

扩展 `StorageConfig`：

```cpp
bool adaptive_read_threads = true;
std::uint32_t disk_read_target_percent = 90;
```

继续使用：

```text
max_concurrent_file_reads   // 文件读取线程池硬上限
hdd_read_threads_per_disk   // 关闭智能模式或采样失败时回退值
ssd_read_threads_per_disk   // 关闭智能模式或采样失败时回退值
```

建议目标范围 `10–100`。采样间隔固定 1 秒、增长步长固定 1、阶段内只增不减。

## 4. 磁盘采样器

新增 `DiskUtilizationSampler`：

1. 从存储目标键解析物理盘编号。
2. 使用 Windows 物理磁盘性能计数计算采样区间读取忙碌比例。
3. 第一次采样只建立基线。
4. `PhysicalSet` 采样所有成员，并取最高占用。
5. 处理计数回绕、设备离线、权限不足和采样错误。
6. 采样失败时只记录一次降级原因，并切换 HDD/SSD 固定策略。

## 5. 中央分发与动态许可

每个存储目标维护：

```text
allowed_read_concurrency
active_read_count
queued_file_count
read_utilization_percent
sampling_available
fallback_reason
```

控制循环：

```text
初始 allowed = 1
每秒采样：
  if queue_backlogged
     and utilization < target
     and allowed < 可用硬上限:
       allowed += 1
```

`allowed` 是该盘提交到文件读取线程池的许可，不创建新的 OS 线程。

## 6. 公平性与 HDD 优化

1. 中央分发器按存储目标轮询提交，禁止 SSD 长期占满线程池导致 HDD 饥饿。
2. HDD 队列继续使用物理位置电梯选择。
3. 全部磁盘活动读取总数不超过 `max_concurrent_file_reads`。
4. 没有积压时不增加许可。
5. 达到或超过目标后停止增加，不在当前阶段减少。
6. 新扫描阶段重新从 1 开始评估。

## 7. GUI 与进度

每盘显示：

- 读取占用百分比。
- 当前允许并发 / 硬上限。
- 活动读取数。
- 排队文件数。
- 吞吐量。
- 采样可用性和回退原因。

配置页增加：

- “智能分配文件读取线程”复选框。
- “磁盘读取占用目标”整数输入。
- 固定 HDD/SSD 数值作为关闭智能模式或失败回退配置的说明。

## 8. 预计修改文件

- 新增 `DedupCore/scheduling/DiskUtilizationSampler.h/.cpp`
- `DedupCore/config/AppConfig.h`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/orchestration/ScanOptionsCodec.cpp`
- `DedupCore/scheduling/DiskHashScheduler.h/.cpp`
- `DedupCore/orchestration/ProgressSnapshot.h`
- `DedupCore/orchestration/ScanCoordinator.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 9. 取消与异常

- 取消扫描时唤醒分发器、采样器、队列背压和线程池等待。
- 采样线程异常只禁用智能模式，不直接中止可继续的扫描。
- 文件读取池失败时停止全部磁盘提交并保存扫描断点。
- 不在持有磁盘队列锁时等待线程池提交空间。

## 10. 测试与验收

1. 单 HDD、单 SSD、多 HDD、多 SSD 和混合盘分别测试。
2. 占用低于目标时每秒只增加 1。
3. 达到 90% 或硬上限后停止增加。
4. `PhysicalSet` 任一成员达到目标即停止增长。
5. 全局池上限小于各盘许可总和时仍公平推进。
6. 采样失败时使用固定 HDD/SSD 线程并显示原因。
7. 取消后所有控制线程和工作任务及时退出。

## 11. 不在本条范围内

- 不按写入占用扩容。
- 不在当前阶段自动降低并发。
- 不为每个物理盘创建独立 OS 线程池。
