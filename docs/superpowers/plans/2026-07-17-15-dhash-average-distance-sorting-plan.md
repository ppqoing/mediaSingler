# 15—重复组主列表按平均汉明距离排序修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 范围：dHash 相似重复组主列表；不改变重复分组结果和 SHA-512 精确报告

## 1. 修改目标

在 dHash 相似重复组主列表增加两个排序按钮：

- `平均汉明距离 ↑`：组平均距离较小的排在前面。
- `平均汉明距离 ↓`：组平均距离较大的排在前面。

排序只改变主列表的显示顺序，不改变组 ID、组内成员、选择状态、保留策略或删除计划。

## 2. 当前实现

- `ReportSortMode` 只支持生成顺序、可释放大小正序/倒序和组内数量正序/倒序。
- `ApplyReportSort()` 只读取 `ReportGroupSummary`，排序时不会加载完整组，这是大报告保持流畅的正确边界。
- `ReportGroupSummary` 当前没有平均汉明距离字段。
- 组详情已经通过 `SimilarityEvidence::average_hamming_distance` 计算并显示平均值，但该值只在打开详情后计算，不能直接用于主列表全局排序。

## 3. 平均距离定义

对 dHash 相似组 `G`，使用报告生成时保存的规范化 `SimilarityEvidence` 集合 `E(G)`：

```text
group_average_hamming_distance =
    sum(evidence.average_hamming_distance for evidence in E(G)) / |E(G)|
```

规则：

1. 图片证据的 `average_hamming_distance` 等于单个 64 位图片 dHash 的汉明距离。
2. 视频证据的 `average_hamming_distance` 等于 6 个对应采样帧汉明距离的算术平均值。
3. 平均值以不同视觉签名之间的规范化证据为单位，不按同一签名下的路径数量重复加权，避免同内容多路径扭曲排序。
4. 只有一个视觉签名且存在多个相同签名成员时，组平均距离定义为 `0.0`。
5. 存在多个不同视觉签名但证据为空、证据数量不完整，或平均值为 `NaN/Inf` 时，视为报告摘要损坏；不得把该组静默当作距离 `0`。
6. 计算使用 `double`，界面显示保留两位小数；排序比较使用未格式化的原始值。

该定义与当前组详情窗口对 `group.evidence` 求平均的语义保持一致，并避免重新读取媒体文件或重新计算 dHash。

## 4. 报告摘要与持久化

扩展 `ReportGroupSummary`：

```cpp
bool has_average_hamming_distance = false;
double average_hamming_distance = 0.0;
```

修改规则：

1. dHash 报告生成并保存组时一次性计算平均距离，和成员数、可释放大小一起写入 `summary/<ordinal>`。
2. SHA-512 精确报告设置 `has_average_hamming_distance = false`，不伪造 dHash 距离。
3. 提升 `kSummaryCodecVersion`，反序列化时校验布尔标记、数值范围和有限性。
4. 旧 dHash 报告摘要不包含该字段时，不在 GUI 线程加载全部组临时补算；禁用平均距离排序并提示重新生成 dHash 报告。
5. 报告正文和摘要必须在同一个 generation 中提交；摘要写入失败时不得发布半成品报告。

## 5. GUI 与排序逻辑

扩展 `ReportSortMode`：

```cpp
DHashAverageAscending,
DHashAverageDescending,
```

实现规则：

1. 仅在当前打开 dHash 相似报告时显示或启用两个新按钮。
2. 打开 SHA-512 精确报告时，不显示 dHash 排序按钮；如果当前模式来自相似报告，则自动回退到生成顺序。
3. 正序、倒序第一排序键分别为 `average_hamming_distance` 升序、降序。
4. 平均距离相同时使用 `group_id` 升序作为稳定第二排序键，保证重复加载结果一致。
5. 排序后沿用当前逻辑清理组缓存、重建虚拟列表行起点并复位滚动位置，不修改持久化选择状态。
6. 主列表组标题增加只读文本 `平均距离：x.xx`，使用单行显示，不自动换行，便于人工核对排序结果。

## 6. 性能与线程边界

- 平均值只在报告生成或摘要迁移阶段计算一次，GUI 排序只处理轻量摘要。
- 禁止点击排序按钮后遍历完整 `DuplicateGroup`、访问媒体文件或重新计算两两距离。
- `m_reportSummaries` 继续一次加载、内存排序；虚拟列表继续按滚动位置动态加载完整组。
- 排序不提交到 RocksDB，不改变报告 generation；关闭并重新打开报告时可恢复默认生成顺序。

## 7. 预计修改文件

- `DedupCore/dedup/DuplicateReportService.h`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 8. 异常与日志

- 摘要缺失平均距离字段时显示“当前 dHash 报告不支持平均距离排序，请重新生成报告”。
- 摘要解码失败、证据不完整或平均值非法时记录报告类型、generation、ordinal、group ID、证据数和具体原因。
- 单个摘要损坏时停止加载本次报告，避免只显示部分组造成误判。

## 9. 测试与验收

1. 构造平均距离为 `0.0`、`2.5`、`4.0` 的三个图片组，正序和倒序结果正确。
2. 构造六帧平均距离为小数的视频组，排序使用真实 `double` 值，而不是格式化后的两位小数字符串。
3. 平均距离相同时，结果始终按 `group_id` 升序稳定排列。
4. 同一视觉签名增加数百条路径不会改变组平均距离。
5. 相似报告显示两个排序按钮和平均距离文本；SHA-512 报告不显示或不启用这些按钮。
6. 排序前后的组成员、选择状态、可释放大小和组 ID 完全一致。
7. 大报告点击排序时不加载完整组，不出现明显界面卡顿。
8. 旧摘要和非法平均值被明确拒绝，不会按 `0` 混入排序。
9. Debug/Release x64 构建通过，相关自动化测试通过。

## 10. 不在本条范围内

- 不改变 complete-link 严格分组规则。
- 不按组内最大距离、中位数或与保留文件的距离排序。
- 不改变组内成员顺序。
- 不给 SHA-512 精确报告构造伪 dHash 距离。
- 不改变删除选择安全阈值；该功能由第 16 条文档负责。
