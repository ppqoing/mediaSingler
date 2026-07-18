# 相似重复报告 `video_dhashes_required` 修复计划

> 日期：2026-07-16  
> 状态：已执行

## 1. 问题现象

生成 dHash 相似重复报告时任务直接失败，界面显示：

```text
报告生成失败：video_dhashes_required
```

该错误由 `DHashCandidateIndex::AddVideo()` 返回。触发条件是：

- `media_kind` 不是视频；或
- `has_video_dhashes == false`；或
- 六帧 `video_dhashes` 中任意一帧为 `0`。

项目既定规则为：视频六帧中任意一帧为 `0`，整组视频 dHash 均视为无效，不能参与相似去重。

## 2. 根因结论

### 2.1 MySQL 流式查询的过滤条件不完整

`MySqlReadRepository::StreamVisualContents()` 当前查询条件为：

```sql
media_algorithm_version = ?
AND (
    (media_kind = 1 AND image_dhash IS NOT NULL)
    OR
    (media_kind = 2 AND has_video_dhashes = 1)
)
```

该条件只能确认字段存在：

- 图片没有排除 `image_dhash = 0`。
- 视频没有检查六帧二进制 dHash 中是否存在值为 `0` 的帧。

因此数据库中“标志为已计算，但实际 dHash 无效”的历史记录仍会进入报告生成流程。

### 2.2 报告第一阶段先写工作集并强制建立索引

`DuplicateReportGenerator::GenerateSimilar()` 收到 MySQL 内容后直接执行：

1. 把内容写入本次报告的 RocksDB 临时工作集。
2. 图片调用 `candidateIndex.AddImage()`。
3. 视频调用 `candidateIndex.AddVideo()`。

候选索引采用严格规则校验 dHash。无效视频到达 `AddVideo()` 后返回
`video_dhashes_required`，生成器将其作为致命错误写入 `failure`，随后终止整个报告。

### 2.3 后续阶段已有过滤，但执行顺序过晚

报告第二阶段及候选复核阶段已经通过 `IsUsableVisual()` 跳过无效视觉内容，但第一阶段建立候选索引发生在这些过滤之前，因此后续防线无法生效。

## 3. 修复目标

1. 单条无效图片或视频 dHash 数据不得导致整个相似报告生成失败。
2. 视频任意一帧为 `0` 时继续视为无效，不放宽现有相似度规则。
3. 无效视觉数据不写入本次报告工作集，不建立候选索引，也不参与分组。
4. 静态视频保持现有行为：其六帧 dHash 有效但不参与相似视频分组。
5. 报告仍基于 MySQL 当前总结果生成，不改为读取本地临时结果。
6. 数据损坏、RocksDB 写入失败、MySQL 查询失败等真正基础设施错误仍应终止报告。

## 4. 推荐修改方案

### 4.1 增加统一的视觉 dHash 完整性判断

在重复报告服务内部增加单一判断入口，规则为：

```text
图片：
  media_kind == Image
  image_dhash 有值
  image_dhash != 0

视频：
  media_kind == Video
  has_video_dhashes == true
  六帧 video_dhashes 全部 != 0
```

该判断只检查 dHash 是否完整，不把 `static_visual` 混入完整性判断。静态视频应在后续“是否参与相似比较”阶段单独跳过。

### 4.2 在第一阶段建立索引前跳过无效内容

修改 `DuplicateReportGenerator::GenerateSimilar()` 的
`StreamVisualContents()` 回调：

1. 收到记录后先执行视觉 dHash 完整性判断。
2. 无效记录只增加“已检查/已跳过”进度，随后继续读取下一条。
3. 仅有效记录写入 `workPrefix/content/`。
4. 仅有效记录调用 `AddImage()` 或 `AddVideo()`。
5. `AddImage()`、`AddVideo()` 的严格校验继续保留，作为内部不变量防线。

这样不会通过吞掉索引错误掩盖其他程序缺陷，只在进入候选索引前明确处理可预期的历史无效数据。

### 4.3 收紧 MySQL 查询，减少无效数据传输

在不依赖 SQL 作为唯一安全边界的前提下，收紧
`StreamVisualContents()` 查询：

- 图片增加 `image_dhash <> 0`。
- 视频继续要求 `has_video_dhashes = 1` 和 `video_dhashes IS NOT NULL`。

视频六帧逐帧非零仍由 C++ 解码后的统一判断负责，避免把固定 48 字节布局判断重复写成复杂 SQL。

### 4.4 增加跳过统计与可诊断信息

扩展相似报告进度或生成结果，至少记录：

- 跳过的无效图片数量。
- 跳过的无效视频数量。
- 视频缺失标志数量。
- 视频包含零值帧数量。

报告成功后界面提示示例：

```text
dHash 相似报告已生成，共 20 组；跳过 3 条无效视觉记录。
```

无效内容属于不可参与报告的数据，不应继续显示成“报告生成失败”。如果执行日志需要记录明细，应使用独立操作名，例如：

```text
similar_report_skip_invalid_image_dhash
similar_report_skip_invalid_video_dhash
```

### 4.5 保持错误文本兼容

不删除 `image_dhash_required` 和 `video_dhashes_required`：

- 候选索引被其他调用方传入非法数据时仍返回原错误。
- 报告生成器经过前置过滤后正常情况下不应再触发这些错误。
- 若仍触发，说明前置判断与索引不变量发生漂移，应继续按程序错误终止并记录。

## 5. 预计修改文件

| 文件 | 计划修改 |
| --- | --- |
| `DedupCore/dedup/DuplicateReportService.cpp` | 增加统一完整性判断，在第一阶段写工作集和建索引前过滤无效内容 |
| `DedupCore/dedup/DuplicateReportService.h` | 如需要对 GUI 暴露跳过统计，扩展报告结果或进度结构 |
| `DedupCore/persistence/MySqlReadRepository.cpp` | 收紧图片零值和视频空二进制字段查询条件 |
| `VideoScGUI/VideoScApp.cpp` | 报告成功提示中展示无效视觉内容跳过数量 |
| `DedupTests/main.cpp` | 增加无效图片、缺失视频 dHash、视频零值帧和静态视频测试 |

不修改：

- 视频六帧 dHash 计算算法。
- “任意一帧为 `0` 则整组无效”的规则。
- 汉明距离阈值。
- SHA-512 精确重复报告流程。
- MySQL 表结构和 RocksDB 持久化格式。

## 6. 实施步骤

1. 提取图片和视频 dHash 完整性判断。
2. 在相似报告第一阶段索引前过滤无效内容。
3. 保留候选索引的严格参数校验。
4. 收紧 MySQL 视觉内容查询的基础条件。
5. 增加跳过计数及 GUI 成功提示。
6. 增加以下测试：
   - 图片 dHash 为 `0` 时跳过且报告继续。
   - 视频 `has_video_dhashes == false` 时跳过且报告继续。
   - 视频六帧任意一帧为 `0` 时跳过且报告继续。
   - 六帧有效的静态视频不报错但不进入相似组。
   - 有效图片和视频仍正常生成相似组。
7. 构建 Debug/Release x64。
8. 运行全部 `DedupTests`。
9. 使用当前 MySQL 数据重新生成 dHash 相似报告，确认不再出现
   `video_dhashes_required`。

## 7. 验收标准

1. 当前数据库存在无效视频 dHash 记录时，相似报告仍能完成并发布。
2. 六帧中任意一帧为 `0` 的视频不进入任何相似组。
3. 无效记录不会写入本次报告的候选工作集。
4. 报告界面显示跳过数量，而不是显示整个报告失败。
5. `DHashCandidateIndex::AddVideo()` 直接接收非法视频时仍返回
   `video_dhashes_required`。
6. SHA-512 精确重复报告行为不变。
7. Debug/Release x64 构建成功，全部测试通过。

## 8. 执行结果

1. 已增加统一的 `ClassifyVisualDHash()` 完整性分类：
   - 图片 dHash 缺失或为 `0` 判定无效。
   - 视频缺少完成标志判定无效。
   - 视频六帧中任意一帧为 `0` 判定整组无效。
2. 相似报告第一阶段已在写入 RocksDB 工作集及候选索引前跳过无效视觉内容。
3. `DHashCandidateIndex::AddImage()` 和 `AddVideo()` 的严格校验保持不变。
4. MySQL 视觉内容查询已排除图片零值，以及视频 dHash 二进制字段为空的记录。
5. 报告结果已增加无效图片、无效视频及总跳过数量；GUI 在报告成功后展示统计。
6. 执行日志中的成功消息会包含跳过统计。
7. 已增加缺失视频 dHash、视频零值帧、图片零值及候选索引拒绝零值帧的回归断言。
8. Debug x64 与 Release x64 均构建成功。
9. Debug、Release 下 `DedupTests` 均为 `36/36 passed`。
