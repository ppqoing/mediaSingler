# 05－重复报告生成进度条修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 范围：SHA-512 精确报告与 dHash 相似报告

## 1. 修改目标

两类重复报告都显示当前阶段、阶段序号、已处理量、总量、百分比、活动线程和取消状态。

## 2. 当前实现

- dHash 相似报告已有 7 阶段 `DuplicateReportProgress`。
- GUI 已对 dHash 阶段绘制反色文字进度条。
- SHA-512 精确报告只显示阶段字符串、路径数和组数，没有同等进度条。
- 部分 MySQL 流式阶段无法预先获得可靠总量。

## 3. 统一进度模型

整理 `DuplicateReportProgress`：

```text
generation_id
report_kind
state
stage
stage_index / stage_count
stage_processed / stage_total / stage_total_known
overall_processed / overall_total / overall_total_known
queued_tasks / active_threads / maximum_threads
processed_paths / processed_contents / emitted_groups
latest_error
```

进度快照必须按值复制，不暴露数据库迭代器、队列或线程对象。

## 4. 精确报告阶段

建议拆分为：

1. `counting_exact_report_input`
2. `streaming_exact_duplicate_members`
3. `writing_exact_groups`
4. `publishing_exact_report`

能够通过轻量 SQL 获得总量时显示确定进度；否则显示不确定进度和已处理数量，不为制造百分比执行高成本全量加载。

## 5. dHash 报告阶段

保留当前 7 阶段，并增加：

- 图片长宽比拒绝数量。
- 视频比较数量。
- 报告线程池排队、活动和完成数量。
- 当前冻结的图片/视频阈值。

## 6. GUI 修改

1. 两类报告复用同一 `DrawReportProgress()`。
2. 底层继续使用 `DrawContrastProgressBar()`，文字颜色为进度底色反色。
3. 确定进度显示 `processed / total / percent`。
4. 不确定进度显示动画和已处理数量。
5. 取消后冻结最后一次有效进度，显示“等待线程池任务安全退出”。
6. 完成、失败、取消和阶段切换立即发布；普通进度按固定间隔节流。

## 7. 预计修改文件

- `DedupCore/dedup/DuplicateReportService.h`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `DedupCore/dedup/ExactDuplicateReader.h/.cpp`
- `DedupCore/persistence/MySqlReadRepository.h/.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`

## 8. 异常边界

- 进度回调异常不得中断报告核心流程，记录应用错误后丢弃该次回调。
- `processed` 单调递增且不得超过已知 `total`。
- 旧任务快照通过 generation ID 被 GUI 丢弃，不能覆盖新任务。
- 报告失败时强制发布最后阶段和错误摘要。

## 9. 测试与验收

1. 精确报告启动后立即显示进度条。
2. dHash 报告全部阶段名称和序号正确。
3. 确定总量阶段最终 `processed == total`。
4. 不确定阶段不会显示错误的 100%。
5. 取消、失败和成功的最终状态均稳定可见。
6. 高频进度不会阻塞 ImGui 帧循环。

## 10. 不在本条范围内

- 不把所有阶段强行折算为不准确的单一总百分比。
- 不改变报告分组算法。
- 删除进度由第 10 条文档负责。
