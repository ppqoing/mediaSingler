# 04－图片长宽比前置过滤修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 默认容差：`10%`

## 1. 修改目标

图片候选对先比较长宽比。相对差异超过配置容差时，不再执行 dHash 汉明距离比较，也不能进入同一严格组。

## 2. 判定公式

```text
ratio1 = width1 / height1
ratio2 = width2 / height2
relative_difference = abs(ratio1 - ratio2) / max(ratio1, ratio2) * 100
accept when relative_difference <= tolerance_percent
```

实现使用 `long double` 或等价的溢出安全交叉乘法，禁止整数除法截断。

## 3. 当前实现

- `ShaFileData` 已保存 `width` 和 `height`。
- `CompareVisuals()` 对图片直接执行 `ImagesAreSimilar()`。
- 图片视觉签名当前主要由完整 dHash 构成，相同 dHash 会先压缩。
- 宽高为 0 的图片没有专门的报告失败分类。

## 4. 配置修改

在 `DHashSimilarityConfig` 增加：

```cpp
std::uint32_t image_aspect_ratio_tolerance_percent = 10;
```

建议有效范围 `0–100`，写入：

- `config.json`。
- 配置校验。
- 报告冻结规则。
- 相似报告 metadata。
- 配置页面。

## 5. 比较流程

```text
候选 dHash 对
-> 校验图片宽高非 0
-> 计算长宽比相对差异
-> 超过容差：记录 aspect_ratio_rejected，结束
-> 容差内：计算完整 64 位汉明距离
-> 按图片阈值判定
```

长宽比过滤不加入候选分桶，避免粗桶边界造成漏召回。

## 6. 相同 dHash 压缩修正

为避免相同 dHash、明显不同比例的图片被提前合并，图片 `VisualSignatureKey` 改为：

```text
I/<完整dhash>/<约分后的width>:<约分后的height>
```

- 比例完全相同的内容继续压缩。
- 比例略有差异但在容差内的内容保持两个签名，通过真实比较后仍可同组。
- 比例超差的相同 dHash 内容被长宽比规则拒绝，不再导致“相同签名最终比较失败”。

## 7. 无效尺寸处理

- `width == 0` 或 `height == 0` 时不参与图片相似报告。
- 计入跳过统计并写失败日志：SHA-512、路径、宽高、媒体算法版本和来源数据库。
- 不因单个无效内容终止整个报告，除非失败日志无法写入或数据存储损坏。

## 8. 预计修改文件

- `DedupCore/config/AppConfig.h`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 9. 测试与验收

1. 容差 10%：小于、等于、大于边界分别接受、接受、拒绝。
2. 横图和竖图按直接长宽比比较，不自动旋转归一化。
3. 宽高为 0 不除零，并产生详细失败日志。
4. 相同 dHash、相同比例正确压缩。
5. 相同 dHash、不同但容差内比例仍能进入同一严格组。
6. 相同 dHash、超差比例不会错误合组。
7. 长宽比拒绝时实际汉明计算计数不增加。

## 10. 不在本条范围内

- 不比较图片面积或分辨率差。
- 不进行旋转、裁剪或内容区域检测。
- 不修改视频的时长和 dHash 规则。
