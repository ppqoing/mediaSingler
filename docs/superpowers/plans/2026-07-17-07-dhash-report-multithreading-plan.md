# 07－dHash 报告多线程计算修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 线程来源：独立 dHash 报告线程池

## 1. 修改目标

dHash 报告中彼此独立的距离计算并行执行，提高大量候选对的处理速度，同时保持报告内容与线程调度顺序无关。

## 2. 当前实现

- 候选真实距离校验已经临时创建多个 `std::thread`。
- 工作线程通过有界 `CandidatePairQueue` 获取任务。
- 严格归组保持稳定顺序单线程。
- 当前线程数组和异常汇总逻辑只服务报告内部，无法复用公共线程池能力。

## 3. 并行边界

交给 dHash 报告线程池：

- 图片宽高和长宽比检查。
- 图片完整 64 位汉明距离。
- 视频 6 帧距离与平均值。
- 候选相似证据的纯结果构建。
- 同一当前签名对多个候选组的只读兼容性检查。

保持单线程：

- MySQL 流式读取和稳定代表选择。
- 候选任务稳定编号。
- `group-of` 唯一归属提交。
- 多候选组最终择优。
- 正式报告序号、稳定组 ID 和 active generation 发布。

## 4. 任务模型

输入任务：

```text
candidate_pair_id
left_representative_sha512
right_representative_sha512
media_kind
frozen_rule_snapshot
```

输出结果：

```text
accepted / rejected / failed
reject_reason
group_kind
frame_distances[6]
average_distance
aspect_ratio_difference
```

工作线程不得直接修改 GUI 或分组归属。通过的结果写幂等规范化边或返回结果队列，归组线程只消费完整结果。

## 5. 线程配置

继续使用：

```cpp
dhash_similarity.validation_worker_threads
```

- 默认 4，范围 1–256。
- 报告启动时冻结。
- 只控制 dHash 报告线程池，不影响文件读取和媒体计算池。

## 6. 取消和异常

1. 取消后停止提交新任务。
2. 清空尚未开始任务，允许在途纯计算完成或检查取消标志退出。
3. 首个工作异常保存完整上下文并关闭提交。
4. 全部工作线程 `join` 后才能清理报告 work 键和 RocksDB 使用者。
5. 任一失败都不发布当前 generation。

## 7. 进度

报告快照增加或继续维护：

```text
candidate_pairs_total
queued_candidate_pairs
validated_candidate_pairs
accepted_similarity_pairs
aspect_ratio_rejected_pairs
rejected_bucket_collisions
active_validation_threads
configured_validation_threads
```

GUI 显示完成数、总数、百分比和活动线程。

## 8. 预计修改文件

- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `DedupCore/concurrency/TaskThreadPool.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

## 9. 测试与验收

1. 同一输入分别使用 1、2、4、8 线程，候选边、严格组和关系完全一致。
2. 热门桶大量碰撞时队列有界，内存不随候选总量无限增长。
3. RocksDB 读取或写入失败时所有工作线程安全退出。
4. 报告取消后活动线程最终归零，不发布半成品。
5. 工作线程异常日志包含候选对、媒体类型、阈值和阶段。
6. 多线程性能明显优于单线程的真实大候选集，且 GUI 保持响应。

## 10. 不在本条范围内

- 公共线程池实现细节由第 08 条文档负责。
- 不并行修改严格组归属。
- 不与扫描线程池共享线程或取消状态。
