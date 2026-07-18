# 永久删除可信判定重构设计

日期：2026-07-18
状态：已获用户认可，待实现计划

## 1. 背景与问题

当前对相似（Similar）报告执行"永久删除选中项"时，删除线程在 `VideoScGUI/VideoScApp.cpp:3179` 做整报告级阻断：

```cpp
trustedThreeStageReport =
    videosc::dedup::IsSimilarReportEligibleForPermanentDeletion(metadata);
if (!trustedThreeStageReport) {
    throw std::runtime_error(
        "当前相似报告跳过过视觉内容或候选校验，不具备完整删除证据；请修复源文件并重新生成完整报告后再删除。");
}
```

`IsSimilarReportEligibleForPermanentDeletion`（`DedupCore/dedup/DuplicateReportService.cpp:1074`）目前要求同时满足：

- `report_schema_version == 4`
- `image_uses_three_stage_verification`
- `image_features_incomplete == 0`
- `!partial_scope_published`
- `deferred_hot_signatures == 0`

其中 `partial_scope_published` 在报告生成时（`DuplicateReportService.cpp:2988`）按以下条件写入：跳过无效视觉内容数非零、结构三筛 I/O 失败数非零、结构三筛计算失败数非零。

**问题**：被跳过的文件（特征不完整、无法解码、结构校验失败）根本不会进入分组流程，因而不在任何重复组内；已入组的成员全部通过了完整的三筛/视频校验，其成对证据是完整的。但现行闸门只要作用域内任何一个文件被跳过，就禁止删除整份报告里的所有组——可靠组被无关的坏文件误伤。用户立场（已确认）：不可靠的文件本就不该进入重复判断；对已知可靠的重复组应允许删除。

**已核实的事实**（设计约束）：

- 热门签名延迟（`deferred_hot_signatures != 0`）会导致候选构建失败（`PdqCandidateBuilder.cpp:265`，"hot image signatures require manual review"），报告任务整体失败、不发布；已发布 v4 报告恒有 `deferred_hot_signatures == 0`，元数据反序列化也强制此约束（`DuplicateReportService.cpp:640`）。因此该字段无需纳入新判定。
- 成员级安全机制已存在：`BuildPersistedDeletionPlan`（`VideoScApp.cpp:563`）按持久选择冻结的距离上限逐成员复评；对无实测距离的图片成员，`trustedThreeStageReport == false` 时逐成员拒绝（`untrusted_three_stage_report`）。目前整报告 `throw` 使该逐成员路径成为死代码。
- 删除执行器在删除前逐个复核完整 SHA-512，且删除计划强制保留一个副本（`BuildPersistedDeletionPlan` 末尾：`selected_for_deletion.size() >= plan.members.size()` 时拒绝）。
- 完全重复（Exact）报告始终可信，不受本次改动影响（`VideoScApp.cpp:3169`）。

## 2. 目标

1. 删除可信判定只看报告本身的证据质量（schema v4 + 三筛校验），不再因作用域内无关文件被跳过而整报告禁止删除。
2. 被跳过的内容对用户可见、可定位、可修复：持久化跳过记录，GUI 面板展示，提供修复入口。
3. 遗留（非 v4/非三筛）报告保留保护，但提供显式风险确认后的强制删除通道，全程审计留痕。

## 3. 非目标

- 不修改候选生成、三筛校验、分组算法。
- 不修改 V4 元数据编解码格式，不提升报告 schema 版本；不新增 MySQL 表或字段。
- 不做按组污染标记（方案 B，已否决）。
- 不提供全局/持久化的绕过配置开关；绕过仅按次确认。
- 不改变 Exact 报告的删除路径。

## 4. 设计

### 4.1 删除可信判定重构（核心）

- `IsSimilarReportEligibleForPermanentDeletion` 改为：

  ```cpp
  return metadata.report_schema_version == 4 && metadata.image_uses_three_stage_verification;
  ```

  同步更新 `DuplicateReportService.h:179-183` 的注释：判定依据为报告证据质量（schema 与三筛规则），不再考虑作用域完整性字段。
- 删除 `VideoScApp.cpp:3179-3182` 的整报告 `throw`；保留元数据加载失败时的 `删除前报告规则校验失败` 异常（相似报告必须存在可校验的元数据）。
- `image_features_incomplete`、`partial_scope_published` 不再影响删除资格，仅作为报告信息展示与修复引导数据源（见 4.2）。
- 效果：v4 三筛报告即使存在跳过也允许删除；既有安全网全部保留——逐成员冻结上限复评、删除前完整 SHA-512 复核、强制保留副本。无实测距离图片成员的 `untrusted_three_stage_report` 逐成员拒绝路径在当前流程下不可达（不可信报告要么整报告拒绝，要么经 4.3 绕过开关按可信处理），保留作为防御性逻辑。

### 4.2 跳过内容持久化与修复引导

**记录模型**（DedupCore，`DuplicateReportService.h` 新增）：

```cpp
enum class SkippedVisualContentReason : std::uint8_t {
    InvalidImage = 0,          // 图片三级特征未完成
    MissingVideoDHash = 1,     // 视频六帧 dHash 缺失
    ZeroVideoFrame = 2,        // 视频六帧 dHash 含零值
    UnsupportedMedia = 3,      // 不支持的媒体
    StructuralIoFailure = 4,       // 结构三筛读取/解码失败
    StructuralTimeout = 5,         // 结构三筛读取超时
    StructuralComputeFailure = 6,  // 结构三筛比较计算失败
};

struct SkippedVisualContentRecord {
    std::string primary_sha512;    // 十六进制
    std::string secondary_sha512;  // 仅结构三筛失败时有值（候选对另一侧）
    MediaKind media_kind = MediaKind::Other;
    SkippedVisualContentReason reason = SkippedVisualContentReason::InvalidImage;
};
```

**写入**：报告生成期间在现有诊断发出点收集记录（`DuplicateReportService.cpp:1717-1740` 的视觉内容跳过、`:2358/:2385/:2425` 附近的结构三筛失败），随 `SaveSimilarMetadata` 同阶段批量写入 RocksDB：

- 列族：`RocksColumnFamily::ExactIndex`（与报告数据一致）。
- 键：`GenerationPrefix(Similar, generation_id) + "skipped/" + Hex64(seq)`。
- 值：带版本字节的二进制序列化（与现有元数据序列化风格一致）。
- 分块写入（每块约 4096 条，对齐现有 `DeletePrefix`/候选批写模式）。
- 清理：报告换代时 `Publish` 删除旧代前缀（`DuplicateReportService.cpp:1108-1113`），跳过记录随之自动清理，无需额外逻辑；未发布的中断代由 `ReportGenerationCleanup` 清理（需把 skipped 前缀纳入其清理范围——其 `generation_prefix_` 已覆盖）。

**读取**:`DuplicateReportStore` 新增：

- `LoadSkippedContents(generation_id, offset, limit, out)`：分页加载。
- `CountSkippedByReason(generation_id, counts)`：按原因聚合计数（面板头部用）。

**GUI**(`VideoScApp.cpp` 报告信息区，`:5071-5122` 附近）:

- 新增"跳过内容"折叠区：按原因显示计数；虚拟列表逐条显示原因、SHA-512、解析出的当前活动路径（经现有路径索引按 SHA 解析，与"关联有效文件路径"同一数据源；解析失败显示"已不存在"）。样例路径为报告生成时快照，之后修复/移动文件需重新生成报告才会反映（面板提供重新生成入口）。
- 修复入口：提供"重新扫描并补齐特征"按钮，触发既有图片特征回填/扫描流程（`backfilling_image_features` 机制），完成后提示用户重新生成报告。

### 4.3 遗留报告风险绕过开关

- 适用：仅当相似报告不可信（非 v4 或非三筛，即旧报告）。v4 报告不再需要绕过。
- UI：不可信报告上"永久删除"按钮默认禁用，旁边显示具体原因（旧报告规则版本）；提供"了解风险后强制删除"勾选，勾选后弹二次确认对话框，确认后才允许启动删除。
- 语义：绕过等同于在 `BuildPersistedDeletionPlan` 调用处把 `trustedThreeStageReport` 置为 true（报告级证据信任）；逐成员冻结上限复评、删除前完整 SHA-512 复核、强制保留副本仍然生效。
- 审计：绕过删除启动时写执行日志事件（`ExecutionLogger.WriteEvent`,operation=`override_untrusted_report`，含 task_id、报告 generation、选中组数）。
- 不做持久化配置；每次删除任务单独确认。

### 4.4 文案调整

- 移除整报告阻断错误文案（随 4.1 的 throw 一并删除）。
- 部分作用域橙色警告（`VideoScApp.cpp:5103-5106`）保留，去掉"不可用于永久删除"，改为：`该报告自动跳过了无法完成特征或结构校验的资源；结果仅覆盖成功计算的视觉内容。`

### 4.5 影响文件

- `DedupCore/dedup/DuplicateReportService.h`：判定函数注释、跳过记录模型与存储接口声明。
- `DedupCore/dedup/DuplicateReportService.cpp`：判定函数实现、跳过记录收集/序列化/批量写入/分页读取/按原因计数。
- `VideoScGUI/VideoScApp.cpp`：移除整报告 throw、跳过内容面板与修复入口、绕过勾选与确认、审计事件、文案。
- `DedupTests/main.cpp`：更新旧语义断言，新增测试（见 4.6）。

### 4.6 测试

更新 `DedupTests/main.cpp` 现有断言（`:2178/:2181` 附近）为新判定语义，并新增：

1. v4 部分作用域报告（`partial_scope_published=true`、`image_features_incomplete>0`）判定为可信、允许删除。
2. 遗留报告（`image_uses_three_stage_verification=false` 或 `report_schema_version!=4`）判定为不可信；默认拒绝删除，绕过后按可信处理。
3. 跳过记录：保存→分页加载→按原因计数一致；发布新一代后旧代记录被清理。
4. 保留现有元数据严格校验测试（`:2208` 非法规则拒绝、`:2327` 缺失元数据拒绝）。

## 5. 风险与缓解

- **风险**：部分作用域报告放开删除后，用户可能以为报告覆盖全部文件。缓解：部分作用域警告与跳过内容面板如实展示覆盖范围；删除安全不依赖作用域完整性，而依赖逐成员复评 + SHA-512 复核 + 保留副本。
- **风险**：绕过开关被当作常规通道。缓解：不做持久化开关，每次显式二次确认并审计留痕。
- **风险**：跳过记录量极大时写入/读取开销。缓解：分块写入、分页加载、按原因计数只在面板展开时计算。
