# 16—选择与删除的 dHash 安全距离上限修改计划

> 日期：2026-07-17  
> 状态：已执行，等待最终验收
> 范围：dHash 相似报告的选择入口和删除前安全复核；SHA-512 精确报告不受影响

## 1. 修改目标

在重复报告操作区增加两个可清空输入框：

- `图片选择距离上限（严格小于）`
- `视频选择平均距离上限（严格小于）`

选择图片或视频待删除文件时，先由当前保留策略确定组内保留文件，再比较每个候选文件与保留文件的 dHash 距离。只有距离严格小于对应有效上限的候选文件才能进入待删除状态，降低相似但不应删除的文件被批量选中的风险。

## 2. 已确认判定规则

### 2.1 图片

```text
HammingDistance(candidate.image_dhash, retained.image_dhash)
    < effective_image_selection_limit
```

### 2.2 视频

```text
(distance(frame0) + ... + distance(frame5)) / 6.0
    < effective_video_selection_limit
```

视频任意一帧 dHash 为 `0` 时结果无效，不得选中。

### 2.3 通用规则

1. 比较基准是保留策略本次选出的 `retained_path_id`，不是组根、首成员或平均 dHash。
2. 比较运算符固定为严格小于 `<`，距离等于输入值时不得选中。
3. 图片和视频使用两个独立上限，互不覆盖。
4. SHA-512 精确报告继续按精确内容重复规则选择，不应用 dHash 安全上限。
5. 同一视觉签名的有效成员距离按 `0` 处理，但仍需通过路径、报告 generation 和保留成员校验。

## 3. 配置模型与空值语义

新增独立的选择安全配置，避免和“报告生成阈值”混为同一个业务字段：

```cpp
struct ReportSelectionConfig {
    std::optional<std::uint32_t> image_dhash_distance_exclusive_limit;
    std::optional<double> video_dhash_average_distance_exclusive_limit;
};
```

JSON 配置字段：

```text
report_selection.image_dhash_distance_exclusive_limit
report_selection.video_dhash_average_distance_exclusive_limit
```

规则：

1. 输入框为空时保存为 `null`，不把当前报告阈值复制成固定配置。
2. 空值时从当前 dHash 报告 metadata 读取生成时冻结的阈值：

   ```text
   effective_image_selection_limit = report.image_max_hamming_distance
   effective_video_selection_limit = report.video_max_average_hamming_distance
   ```

3. 因为比较使用严格 `<`，例如报告图片阈值为 `4` 且输入为空时，只会选中距离 `0、1、2、3` 的候选文件，距离 `4` 不会选中。
4. 图片输入为整数；视频输入允许小数，以匹配六帧平均距离。两个值均不得为负数、`NaN` 或无穷值。
5. 图片有效范围为 `0–64`，视频有效范围为 `0.0–64.0`；超出范围时拒绝保存和执行选择，不静默截断。
6. 旧配置缺少字段时迁移为空值，自动沿用报告生成阈值。
7. 旧 dHash 报告缺少图片或视频生成阈值 metadata 时禁用对应选择，不回退到当前配置页面的生成阈值，并提示重新生成报告。

## 4. GUI 修改

在重复报告的保留策略与“按条件选择”按钮之间增加两个输入框。

界面规则：

1. 输入框标签明确显示“严格小于”，避免用户把边界理解为 `<=`。
2. 输入为空时在同一行显示实际回退值，例如：

   ```text
   未填写，沿用报告生成阈值：图片距离 < 4
   未填写，沿用报告生成阈值：视频六帧平均距离 < 5
   ```

3. 输入非法时显示红色原因，并禁用“按条件选择当前可见组”和“按条件选择全部报告”。
4. 编辑完成后通过现有配置存储流程写入 `config.json`；重启应用后恢复填写值或空值状态。
5. 打开 SHA-512 精确报告时隐藏或禁用两个输入框，并标明精确报告不使用 dHash 安全距离。
6. 选择完成后显示因距离边界未选中的文件数量，便于人工确认保护规则确实生效。

## 5. 选择流程修改

将距离规则封装在删除选择服务层，不由 ImGui 按钮回调自行判断：

```text
选择命令
  -> 根据 KeepPolicy 确定 retained_path_id
  -> 取得报告 generation 的冻结规则
  -> 解析图片/视频有效选择上限
  -> 逐个计算候选文件与保留文件的真实距离
  -> 仅提交 distance < effective_limit 的候选成员
  -> 原子发布选择快照与汇总
```

具体要求：

1. 扩展 `DeletionPlanner::Select()` 的输入，显式接收报告类型、报告规则快照和选择安全规则，禁止内部读取 GUI 状态。
2. 复用 `DHashRules::HammingDistance()` 和统一的视频六帧平均距离函数，不复制 `popcount` 或平均值实现。
3. “按条件选择当前可见组”和“按条件选择全部报告”执行完全相同的安全判定；区别只在数据范围。
4. 单成员手动选中也必须经过同一规则，避免绕过批量按钮后仍可误删。
5. 找不到保留成员、媒体类型不一致、dHash 缺失、视频帧为零或距离计算失败时，该候选保持未选中并记录原因。
6. 至少保留一个成员的现有不变量继续生效；安全过滤只会减少删除候选，不会改变保留成员。
7. 选择命令开始时冻结有效上限；命令运行期间修改输入只影响下一次选择命令。

## 6. 持久化选择与删除前复核

为实现跨重启可审计和防止选择状态绕过，在选择记录或组摘要中保存：

```text
source_report_generation
retained_path_id
candidate_path_id
media_kind
measured_hamming_distance
distance_rule
exclusive_limit
used_report_default
```

规则：

1. 当前可见组选择和全报告选择继续写入 selection staging generation，全部完成后原子切换 active 指针。
2. 选择汇总只累计通过安全上限的文件和字节；被过滤文件不计入“已选择总体积”。
3. 永久删除开始前对每条选择记录复核报告 generation、保留成员、媒体类型、距离和严格上限。
4. 复核不通过的文件记为“安全规则跳过”，不得调用 `DeleteFileW`，并从本次预计释放体积中扣除。
5. 配置值在选择完成后发生变化时，不修改已发布选择；删除复核使用该选择记录中冻结的上限。用户重新执行选择命令后才生成采用新值的选择 generation。
6. 选择记录缺少安全快照的旧 dHash 选择不得直接永久删除；要求重新执行按条件选择。SHA-512 旧选择不受此限制。

## 7. 日志要求

安全过滤和删除复核写入易读文本执行日志，至少包含：

```text
操作、报告类型、report generation、group ID、candidate path、retained path、
媒体类型、实测距离、严格上限、上限来源（输入/报告默认）、结果、原因
```

批量选择完成时汇总：扫描文件数、选中文件数、距离边界过滤数、无效 dHash 数和失败数。日志不得使用 JSON 格式。

## 8. 预计修改文件

- `DedupCore/config/AppConfig.h`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/dedup/DHashSimilarity.h`
- `DedupCore/dedup/DHashSimilarity.cpp`
- `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/deletion/DeletionService.h`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

## 9. 测试与验收

1. 图片上限填 `4` 时，距离 `3` 可选中，距离 `4` 和 `5` 不可选中。
2. 视频上限填 `4.5` 时，六帧平均距离 `4.49` 可选中，`4.5` 不可选中。
3. 图片输入为空时使用报告图片生成阈值；视频输入为空时使用报告视频生成阈值。
4. 当前配置页生成阈值与旧报告 metadata 不一致时，空值始终沿用报告生成时阈值。
5. 图片和视频上限分别生效，修改一项不会影响另一媒体类型。
6. 任意视频帧为 `0`、dHash 缺失或保留成员不存在时不选中并记录详细原因。
7. 当前可见组、全部报告和单成员手动选择都无法绕过安全上限。
8. 重启后输入值、空值状态、选择规则快照和已选总体积保持一致。
9. 修改输入但不重新选择时，旧选择仍按冻结规则复核；重新选择后使用新规则。
10. 删除前篡改或损坏选择记录时，目标文件被安全跳过而不是删除。
11. SHA-512 精确报告的选择结果与改动前一致。
12. Debug/Release x64 构建通过，相关自动化测试通过。

## 10. 不在本条范围内

- 不修改 dHash 报告的分组阈值和 complete-link 规则。
- 不根据组平均距离直接决定删除文件。
- 不让安全选择输入反向修改报告生成配置。
- 不对 SHA-512 精确报告增加 dHash 限制。
- 不自动删除被距离规则过滤的文件。
