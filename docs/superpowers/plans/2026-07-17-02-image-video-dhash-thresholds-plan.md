# 02－图片与视频独立汉明距离配置修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 图片默认值：`4`，范围 `0–15`  
> 视频默认值：`5`，范围 `0–15`

## 1. 修改目标

配置页分别提供图片与视频相似阈值：

```text
图片：HammingDistance(image_dhash_left, image_dhash_right) <= image_max_hamming_distance
视频：(六帧距离总和 / 6.0) <= video_max_average_hamming_distance
```

音频没有 dHash，继续只参与 SHA-512 精确去重。

## 2. 当前实现

- `DHashSimilarityConfig` 只有 `image_max_hamming_distance` 和报告校验线程数。
- 图片候选索引使用动态 `n+1` 分段。
- 视频候选索引使用固定六段两两组合，最终规则固定为平均值 `< 5`。
- `SimilarReportMetadata` 只保存图片阈值，GUI 无法按报告快照解释视频规则。

## 3. 配置修改

扩展 `DHashSimilarityConfig`：

```cpp
std::uint32_t image_max_hamming_distance = 4;
std::uint32_t video_max_average_hamming_distance = 5;
std::uint32_t validation_worker_threads = 4;
```

实施要求：

1. 提升配置 schema version。
2. 旧配置缺少视频字段时使用默认值 5。
3. `JsonConfigStore`、`ConfigValidator` 和 `ScanOptionsCodec` 同步支持新字段。
4. 配置页在“dHash 相似报告”区域分别显示两个整数输入。
5. 报告启动时冻结两个阈值；运行中修改只影响下一次报告。

## 4. 图片候选与最终判定

图片继续使用：

```text
segment_count = image_max_hamming_distance + 1
```

当真实距离 `<= n` 时，至少有一个同序号分段完全相等。分桶只召回候选，最终仍执行完整 64 位 `popcount`。

## 5. 视频候选无漏召回方案

把 6 个 64 位视频 dHash 按采样帧顺序视为 384 位签名：

```text
maximum_total_distance = video_max_average_hamming_distance * 6
segment_count = maximum_total_distance + 1
```

若平均距离 `<= n`，则总距离 `<= 6n`。把 384 位完整切为 `6n+1` 个连续非空段，至少有一段完全相等，因此候选索引不会漏掉合格视频对。

视频桶键必须包含：

- 索引版本。
- 配置阈值。
- 分段序号和位宽。
- 分段值。
- 现有 2 秒时长桶。

最终比较：

1. 检查双方都有 6 帧且每帧非 0。
2. 保留静态画面视频排除规则。
3. 保留时长差不超过 2 秒规则。
4. 计算 6 个对应帧的完整汉明距离。
5. 使用 `total / 6.0 <= configured_maximum` 判定。

## 6. 报告元数据

相似报告 metadata 至少新增：

```text
video_max_average_hamming_distance
video_bucket_index_version
video_grouping_rule = video-average-complete-link-disjoint-v1
```

同时继续保存图片阈值、算法版本、线程池大小和生成时间。旧报告缺少新字段时提示重新生成，不使用当前配置猜测旧报告结果。

## 7. 预计修改文件

- `DedupCore/config/AppConfig.h/.cpp`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/orchestration/ScanOptionsCodec.cpp`
- `DedupCore/dedup/DHashSimilarity.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 8. 测试与验收

1. 新旧配置保存、加载和迁移正确。
2. 图片阈值变化不影响视频；视频阈值变化不影响图片。
3. 对视频 `n=0..15` 构造总距离 `0..6n` 的随机样本，全部被候选索引召回。
4. 平均值小于、等于、大于阈值时分别接受、接受、拒绝。
5. 当前固定 `< 5` 文案全部改为报告快照中的 `<= 配置值`。
6. 阈值不一致时 GUI 显示报告实际值并提示重新生成。

## 9. 不在本条范围内

- 不改变 6 个采样时间点。
- 不配置视频时长差阈值。
- 不增加音频感知哈希。
- 不修改 SHA-512 精确报告。
