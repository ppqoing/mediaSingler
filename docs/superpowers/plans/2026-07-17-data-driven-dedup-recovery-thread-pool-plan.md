# 重复报告、数据驱动、线程池与中断恢复综合修改计划

> 日期：2026-07-17  
> 状态：16 项修改已执行，等待最终人工验收  
> 范围：重复报告、排序、配置、布局、任务进度、线程池、磁盘读取调度、删除、日志、选择状态、安全距离和异常退出恢复  
> 本文保留原实施方案，并记录实际执行与自动化验证结果

## 执行结果（2026-07-17）

- 16 份独立修改方案均已落地，覆盖严格 complete-link 分组、图片/视频独立阈值、长宽比过滤、报告与删除进度、三类独立线程池、每盘自适应读取、持久化选择、平均距离排序和安全选择上限。
- RocksDB 增加单实例互斥与安全关闭租约；启动时清理未发布报告和选择 staging，不删除 RocksDB `LOCK` 文件。
- 人工排查日志改为 UTF-8 易读文本并即时刷新；结构化扫描清单仍保留其原有数据格式。
- GUI 正常关闭时显式保存布局，并周期性保存最近布局；所有进度条文字使用底色反色绘制。
- Debug x64 构建与 Release x64 全量 Rebuild 均通过；两套 `DedupTests` 均为 40/40 通过。损坏 TIFF 相关 FFmpeg 输出为故障输入测试的预期诊断。
- 尚需在真实 MySQL、多块 HDD/SSD、`PhysicalSet` 和大规模媒体库环境执行人工性能与中断恢复验收。

## 独立修改文档索引

1. [图片与视频严格汉明距离分组](2026-07-17-01-strict-dhash-complete-link-grouping-plan.md)
2. [图片与视频独立汉明距离配置](2026-07-17-02-image-video-dhash-thresholds-plan.md)
3. [关闭窗口自动保存 GUI 布局](2026-07-17-03-save-layout-on-close-plan.md)
4. [图片长宽比前置过滤](2026-07-17-04-image-aspect-ratio-prefilter-plan.md)
5. [重复报告生成进度条](2026-07-17-05-duplicate-report-progress-plan.md)
6. [强制退出恢复与 RocksDB 锁安全](2026-07-17-06-forced-exit-recovery-rocksdb-lock-plan.md)
7. [dHash 报告多线程计算](2026-07-17-07-dhash-report-multithreading-plan.md)
8. [三个独立线程池架构](2026-07-17-08-three-independent-thread-pools-plan.md)
9. [按物理盘智能增加文件读取并发](2026-07-17-09-adaptive-per-disk-read-concurrency-plan.md)
10. [永久删除操作进度条](2026-07-17-10-deletion-progress-plan.md)
11. [易读文本执行日志](2026-07-17-11-human-readable-text-logging-plan.md)
12. [重复报告选择状态持久化](2026-07-17-12-persistent-report-selection-plan.md)
13. [项目数据驱动架构渐进改造](2026-07-17-13-data-driven-architecture-plan.md)
14. [显示选中文件数量与总体积](2026-07-17-14-selected-total-size-plan.md)
15. [重复组主列表按平均汉明距离排序](2026-07-17-15-dhash-average-distance-sorting-plan.md)
16. [选择与删除的 dHash 安全距离上限](2026-07-17-16-selection-dhash-safety-threshold-plan.md)

## 1. 已确认的实施口径

1. 相似报告只处理图片和视频；音频继续只参与 SHA-512 精确去重。
2. 图片最大汉明距离沿用整数配置，默认 `4`，有效范围 `0–15`。
3. 视频使用 6 个对应采样帧的汉明距离平均值，判定条件为：

   ```text
   (distance(frame0) + ... + distance(frame5)) / 6 <= video_max_average_hamming_distance
   ```

4. 视频最大平均汉明距离默认 `5`，有效范围 `0–15`。
5. 图片先比较长宽比，再计算 dHash 汉明距离；长宽比容差在配置页设置，默认 `10%`。
6. 文件读取、媒体计算、dHash 报告分别使用三个独立线程池，不共享队列、线程额度或取消状态。
7. 智能文件读取按物理盘分别控制：从 1 个并发开始，每秒采样一次，有积压且磁盘占用低于目标时增加 1，阶段内只增不减。
8. 磁盘读取占用目标在配置页设置，默认 `90%`；无法采样时回退到 HDD/SSD 固定线程数。
9. `PhysicalSet` 使用成员物理盘中的最高占用作为该存储目标的控制值。
10. 扫描中断后保留断点；未发布报告半成品在下次启动时清理；删除事务按 RocksDB 意图记录恢复。
11. RocksDB 正常关闭时先停止任务和三个线程池，再销毁数据库使用者并调用 `RocksStore::Close()`；不直接删除 RocksDB `LOCK` 文件。
12. 强杀后依赖 Windows 释放进程文件锁，启动时先做单实例校验并重试正常打开；确认无其他实例后仍无法打开时记录错误并停止恢复，不绕过 RocksDB 互斥保护。
13. 重复报告选择状态持久化到 RocksDB，并按报告类型和 generation 隔离；重启后继续保留，直到报告被删除或新报告替换。
14. 新增代码继续遵循项目现有 C++ 命名风格，并按全局规则为新增或重写的类型、方法、复杂状态和关键流程补充中文注释。
15. dHash 重复组主列表增加组平均汉明距离正序、倒序；平均值按报告内规范化 `SimilarityEvidence` 计算，相同签名的多路径不重复加权。
16. 图片和视频选择安全上限使用独立可清空输入框；图片比较候选与保留文件的单 dHash 距离，视频比较候选与保留文件的六帧平均距离，只有严格 `<` 有效上限的文件才能选中。空值沿用报告生成时阈值，SHA-512 精确报告不受影响。

## 2. 当前源码基线与差距

### 2.1 已经存在、应复用的能力

- `DuplicateReportGenerator::GenerateSimilar()` 已分为 7 个阶段，并已发布 dHash 候选数、校验数和活动线程数。
- 当前图片相似报告已经使用动态 `n+1` 分桶、真实汉明距离复核和 complete-link 严格分组。
- 当前视频报告保存 6 帧 dHash、2 秒时长差过滤，并排除任一帧为 0 的无效结果和静态画面视频。
- `DuplicateReportStore` 已通过 generation 和 active 指针保证未发布报告不会覆盖上一份完整报告。
- `ScanCheckpointStore` 已保存扫描断点，`DeletionExecutor` 已使用 tombstone 恢复“文件已删、映射未排队”的中断窗口。
- `RenderBackend` 已配置 `VideoScGUI.layout.ini`，设置页已有“重置初始布局”。
- `DiskHashScheduler` 已有每物理存储目标队列、HDD 电梯排序、全局读取闸门和 CPU 自适应计算闸门。
- GUI 已使用 `ImGuiListClipper` 虚拟化重复报告列表，并具有进度条反色文字绘制函数。

### 2.2 本次必须修正的差距

- 视频相似阈值仍固定为平均值 `< 5`，没有独立配置，也没有写入报告规则快照。
- 当前视频候选索引是固定分段规则，不能直接保证任意配置阈值下无漏召回。
- 图片最终比较没有长宽比前置过滤；相同 dHash 签名压缩也没有区分长宽比。
- dHash 校验使用报告内部临时创建的 `std::thread` 数组，没有公共线程池；媒体阶段和磁盘读取同样各自维护线程。
- 严格分组的归属提交必须保持确定性单线程，但纯距离计算与候选组兼容性检查还没有统一交给报告线程池。
- SHA-512 精确报告只显示文本计数，没有与 dHash 报告一致的阶段进度条。
- 删除线程只有最终结果，没有实时阶段、文件数、字节数和当前路径进度。
- `ReportGroupSummary` 没有组平均汉明距离字段，主列表不能在不加载完整组的前提下按平均距离排序。
- “按条件选择全部报告”当前只遍历 `m_reportGroupCache`，实际只能即时改变可见缓存；离屏组没有物化选择状态。
- 选择状态由 GUI 缓存和 `m_reportSelections` 维护，不是持久化数据源，也无法可靠统计选中文件总体积。
- 当前选择逻辑只应用保留策略，没有候选文件相对保留文件的 dHash 安全距离上限，选择和删除前也没有统一复核。
- 日志主要写入 `*.jsonl`，人工阅读困难；读取、计算、报告和删除失败的字段分散。
- 正常关闭只依赖 `ImGui::DestroyContext()` 的隐式保存，缺少关闭前显式布局落盘和强杀前的周期性保存。
- 报告 RAII 清理只在析构函数执行；强杀会留下 work generation、候选桶或任务状态。
- RocksDB 生命周期没有统一的“停止接单—等待操作归零—关闭数据库”闸门，也没有按数据库目录隔离的单实例保护。
- GUI 中按钮回调仍直接修改多个缓存、原子量和消息字符串，数据驱动边界不完整。

## 3. 总体数据驱动设计

### 3.1 单向状态流

本次不进行一次性 GUI 全量重写，先把本计划涉及的任务迁移为统一单向状态流：

```text
用户操作
  -> Command（启动、取消、选择、删除、保存布局）
  -> Service / Coordinator
  -> RocksDB 持久状态 + 线程安全 Snapshot
  -> ApplicationState
  -> ImGui 只读取 ApplicationState 渲染
```

约束：

- GUI 不再以“当前是否可见”决定业务选择结果。
- 后台线程不得直接操作 ImGui，也不得直接拼接多个 GUI 消息字段。
- 服务层只发布不可变快照；GUI 每帧复制或交换快照。
- RocksDB 是报告、选择、恢复意图的持久化事实来源；内存状态只用于加速显示。
- 配置在任务启动时冻结，任务运行中修改只影响下一次任务。

### 3.2 新增状态切片

在 `VideoScGUI` 内增加或整理以下状态模型：

```text
ReportGenerationViewState
ReportSelectionViewState
DeletionViewState
LayoutPersistenceState
RecoveryViewState
```

每个状态至少包含：

- 稳定任务 ID 或 generation ID。
- `Idle / Running / Cancelling / Completed / Failed / Interrupted` 状态。
- 当前阶段、阶段序号和阶段总数。
- 已处理数量、总量、总量是否已知。
- 最后错误和最后更新时间。
- 与该任务相关的线程池快照。

扫描继续使用现有 `ProgressSnapshot`，但磁盘智能读取字段和线程池字段要补齐。

### 3.3 命令与状态提交边界

新增或整理命令：

```text
StartReportGenerationCommand
CancelReportGenerationCommand
ApplyReportSelectionCommand
ToggleReportMemberSelectionCommand
StartDeletionCommand
SaveLayoutCommand
RecoverInterruptedTasksCommand
```

命令完成后一次性提交状态快照，禁止按钮回调同时修改：

- RocksDB 选择记录。
- 报告缓存。
- 详情缓存。
- 总体积计数。
- 删除确认信息。

## 4. 配置模型、兼容与冻结快照

### 4.1 dHash 配置

扩展 `DHashSimilarityConfig`：

```cpp
std::uint32_t image_max_hamming_distance = 4;
std::uint32_t video_max_average_hamming_distance = 5;
std::uint32_t image_aspect_ratio_tolerance_percent = 10;
std::uint32_t validation_worker_threads = 4;
```

校验范围：

| 字段 | 范围 | 默认值 |
| --- | ---: | ---: |
| 图片最大汉明距离 | 0–15 | 4 |
| 视频最大平均汉明距离 | 0–15 | 5 |
| 图片长宽比容差 | 0–100% | 10% |
| dHash 报告线程池大小 | 1–256 | 4 |

### 4.2 智能读取配置

扩展 `StorageConfig`：

```cpp
bool adaptive_read_threads = true;
std::uint32_t disk_read_target_percent = 90;
```

继续复用：

```cpp
max_concurrent_file_reads       // 文件读取线程池硬上限
hdd_read_threads_per_disk       // 关闭智能模式或采样失败时的 HDD 回退值
ssd_read_threads_per_disk       // 关闭智能模式或采样失败时的 SSD 回退值
```

`disk_read_target_percent` 校验范围建议为 `10–100`；采样周期固定 1 秒、增长步长固定 1，不额外增加配置噪声。

### 4.3 配置持久化与迁移

修改：

- `DedupCore/config/AppConfig.h/.cpp`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/orchestration/ScanOptionsCodec.cpp`
- `DedupCore/orchestration/ScanOptions.cpp`

处理方式：

1. 提升 `config.json` schema version。
2. 旧配置缺少新字段时使用上述默认值。
3. 新字段保存到扫描冻结快照；恢复旧扫描时按默认值迁移。
4. 配置页清楚标注：修改只影响下一次扫描或报告。
5. 不修改 MySQL 表结构；新规则只进入配置、RocksDB 报告元数据和任务日志。

### 4.4 dHash 选择安全配置

新增与报告生成阈值相互独立的可空配置：

```cpp
std::optional<std::uint32_t> image_dhash_distance_exclusive_limit;
std::optional<double> video_dhash_average_distance_exclusive_limit;
```

- 字段保存到 `report_selection` 配置节点，空输入保存为 `null`。
- 空值使用当前 dHash 报告 generation 中冻结的图片、视频生成阈值，不读取当前配置页生成阈值替代旧报告规则。
- 图片允许 `0–64` 的整数；视频允许 `0.0–64.0` 的有限小数；判定统一使用严格 `<`。
- SHA-512 精确报告不读取这两个字段。

## 5. 逐条修改方案

## 5.1 第 1 条：重复结果改为严格汉明距离分组

### 目标

dHash 相似报告中的每个主组必须满足：

```text
图片组：任意两个不同视觉签名的 HammingDistance <= 图片阈值
视频组：任意两个不同视觉签名的六帧平均 HammingDistance <= 视频阈值
```

相同视觉签名下的多个 SHA-512 内容和文件路径距离为 0，作为同一签名成员处理。

### 方案

1. 保留当前“候选召回 → 真实距离边 → 稳定顺序 complete-link 归组”主流程。
2. 把图片和视频的最终判定统一收口到 `CompareVisuals()`，显式接收冻结后的图片阈值、视频阈值和长宽比容差。
3. 每个新签名加入候选组前，必须存在它与组内每个既有签名的已验证相似边；缺少任意一条边立即拒绝该候选组。
4. 禁止使用组根、首成员、平均 dHash、中心 dHash 或相似关系传递闭包替代完整链接校验。
5. 多个组都可接纳时继续使用：最大距离更小 → 平均距离更小 → 稳定组根字典序更小。
6. 分组归属提交保持单线程稳定顺序；线程池只能并行计算候选边或只读兼容性结果。
7. 在保存组摘要时记录不同视觉签名数和校验边数；发布前验证每个 `k` 签名组已经完成 `k*(k-1)/2` 个直接关系检查。
8. 图片跨严格组直接相似关系继续只读展示，不进入可释放空间和删除选择；本次不新增视频跨组关系 UI。

### 主要文件

- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `DedupCore/dedup/DHashSimilarity.h/.cpp`
- `DedupTests/main.cpp`

### 验收

- 枚举每个发布组的全部签名对，图片和视频均不出现超过对应阈值的组合。
- 1 线程和多线程生成的组 ID、成员和关系完全一致。
- `A-B` 合格、`B-C` 合格、`A-C` 不合格时，三者不会进入同一主组。

## 5.2 第 2 条：图片和视频使用独立阈值

### 目标

配置页分别提供：

- 图片最大汉明距离。
- 视频 6 帧平均汉明距离最大值。

### 方案

1. 图片继续使用 64 位 dHash 动态 `n+1` 分桶。
2. 视频把 6 个 dHash 按帧顺序视为 384 位签名，候选索引按以下规则动态分段：

   ```text
   maximum_total_distance = video_max_average_hamming_distance * 6
   segment_count = maximum_total_distance + 1
   ```

3. 当视频平均距离 `<= n` 时，总距离必定 `<= 6n`；将 384 位切为 `6n+1` 个非空连续段，可保证至少一段完全相等，因此不会因分桶漏召回。
4. 视频桶键写入索引版本、阈值、段序号、位宽、段值和时长桶；保留现有 2 秒时长差限制。
5. 最终视频比较逐帧计算 6 个距离，使用 `total / 6.0 <= configured_maximum`，并将六帧距离及平均值写入证据。
6. 报告元数据 schema 升级并保存：

   ```text
   image_max_hamming_distance
   video_max_average_hamming_distance
   image_aspect_ratio_tolerance_percent
   image_bucket_index_version
   video_bucket_index_version
   image_grouping_rule
   video_grouping_rule
   ```

7. 缺少新元数据的旧 dHash 报告提示重新生成，不按当前规则解释。

### 主要文件

- `DedupCore/config/AppConfig.h`
- `DedupCore/dedup/DHashSimilarity.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `DedupCore/orchestration/ScanOptionsCodec.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

### 验收

- 图片阈值变化不影响视频结果；视频阈值变化不影响图片结果。
- 对所有允许的视频阈值构造总距离 `0..6n` 的样本，候选索引均能召回。
- 报告详情显示生成时冻结的实际阈值，不读取当前配置替代旧报告规则。

## 5.3 第 3 条：关闭窗口时自动保存布局

### 目标

正常关闭时明确保存主窗口、Dock、现有功能窗口和已拖出的 viewport 布局；强杀前尽可能通过周期性保存降低丢失范围。

### 方案

1. 在 `RenderBackend` 增加幂等 `SaveLayoutNow()`，内部调用 `ImGui::SaveIniSettingsToDisk()`。
2. Win32 收到 `WM_CLOSE` 时只发出关闭请求，不立即销毁 ImGui；主循环按以下顺序退出：

   ```text
   停止接收新命令
   -> 保存布局
   -> 关闭 VideoScApp 后台任务
   -> 再次保存布局
   -> DestroyContext
   -> 销毁窗口
   ```

3. 当 `io.WantSaveIniSettings` 为真或距离上次保存达到固定周期时执行节流保存，覆盖拖动、停靠和 viewport 位置变化。
4. 保存后检查布局文件是否存在并更新时间；失败写入文本执行日志并在设置页显示。
5. “重置初始布局”继续立即保存，并同步刷新 `LayoutPersistenceState`。
6. 强杀无法执行关闭回调，文档和 UI 明确说明只能恢复最近一次周期保存结果。

### 主要文件

- `VideoScGUI/RenderBackend.h/.cpp`
- `VideoScGUI/main.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`

### 验收

- 调整 Dock、拖出功能窗口、移动和缩放后正常关闭，重启恢复最后布局。
- 关闭请求期间后台任务安全停止，布局文件仍在 ImGui Context 销毁前写入。
- 保存失败不会静默丢失，设置页和日志均能看到原因。

## 5.4 第 4 条：图片先比较长宽比，再比较 dHash

### 目标

长宽比差距超过配置容差的图片不进入汉明距离计算，也不进入同一严格组。

### 判定公式

```text
ratio1 = width1 / height1
ratio2 = width2 / height2
relative_difference = abs(ratio1 - ratio2) / max(ratio1, ratio2) * 100
relative_difference <= configured_tolerance_percent
```

实现使用 `long double` 或等价的安全交叉乘法，避免整数除法和乘法溢出。

### 方案

1. 图片 `width == 0` 或 `height == 0` 时标记为无效视觉数据，跳过相似报告并写详细失败日志。
2. 候选桶仍只负责 dHash 无漏召回；工作线程取到候选对后先执行长宽比判断，失败则计入“长宽比拒绝数”，不执行 `popcount`。
3. 为避免“相同 dHash、长宽比完全不同”的内容在签名压缩阶段被错误折叠，图片 `VisualSignatureKey` 增加约分后的精确长宽比：

   ```text
   I/<dhash>/<reduced_width>:<reduced_height>
   ```

4. 精确长宽比不同但容差内的相同 dHash 仍会作为两个候选签名进入真实比较，距离为 0，允许进入同一严格组。
5. 长宽比过滤只作用于图片；视频继续使用时长差和 6 帧平均距离规则。
6. 进度和最终报告日志增加：无效尺寸数、长宽比拒绝数、实际汉明距离计算数。

### 主要文件

- `DedupCore/dedup/DuplicateReportService.cpp`
- `DedupCore/config/AppConfig.h`
- `VideoScGUI/VideoScApp.cpp`
- `DedupTests/main.cpp`

### 验收

- 容差 10% 边界内接受、边界外拒绝、恰好 10% 接受。
- 相同 dHash 但明显不同长宽比不再导致报告生成失败或错误合组。
- 宽高无效的图片不会发生除零，并有可定位日志。

## 5.5 第 5 条：重复报告生成显示进度条

### 目标

SHA-512 精确报告和 dHash 相似报告都使用同一进度组件显示阶段、进度、线程和取消状态。

### 方案

1. 把 `DuplicateReportProgress` 整理为通用阶段字段加报告专用统计，精确报告也填写 `stage_index/stage_count/stage_processed/stage_total`。
2. 精确报告至少分为：统计输入、读取并分组、写入并发布三个阶段；可获得总量时显示确定百分比，MySQL 流式阶段无法可靠预估时显示不确定进度。
3. dHash 报告保留现有 7 阶段，并增加长宽比拒绝、视频阈值、线程池排队和活动线程统计。
4. GUI 统一使用现有 `DrawContrastProgressBar()`，保证进度条文字为底色反色。
5. 进度快照按固定最小间隔节流发布，任务完成、失败、取消和阶段切换强制发布。
6. 取消后冻结最后有效进度，显示“等待线程池任务安全退出”，不回退到 0。

### 主要文件

- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`

### 验收

- 两类报告启动后均立即出现进度条和阶段名称。
- 已处理数单调递增且不超过总量。
- GUI 渲染不访问 RocksDB，不因高频回调卡顿。

## 5.6 第 6 条：处理中途强制退出或杀进程

### 目标

强杀后不发布半成品、不绕过数据库锁、不重复危险删除，并能在下次启动时给出明确恢复结果。

### 任务日志模型

在 RocksDB `Checkpoints` Column Family 增加内部任务记录：

```text
runtime-task/<task-type>/<task-id>
```

字段包含：

```text
record_version
task_type
task_id
state
stage
report_kind / generation_id / scan_id / deletion_batch_id
configuration_snapshot
started_utc_ms
heartbeat_utc_ms
completed_utc_ms
last_error
```

这是内部恢复数据，可以继续使用版本化二进制或 JSON 编码；它不是面向人工的日志，不受“日志改为文本”限制。

### 启动恢复顺序

```text
获取按 RocksDB 目录派生的 Windows named mutex
-> 正常打开 RocksDB（失败时有限重试）
-> 把遗留 Running/Committing 标记为 Interrupted
-> 恢复删除 tombstone
-> 优先发布历史 staged/pending MySQL 操作
-> 清理未发布报告 work generation 和候选桶
-> 清理未发布的选择 staging generation
-> 列出可恢复扫描
-> 启动同步服务
```

### 各任务恢复规则

- 扫描：保留现有 checkpoint 和本地结果；下次由用户选择恢复，重新规划时排除已经完成的能力。
- 报告：不恢复半个报告；保留上一份 active generation，分批删除未发布 generation、work 键和候选索引。
- 报告清理：删除前缀本身保持幂等；强杀后下次继续清理未完成命名空间。
- 选择全部：使用 staging selection generation，只有全部选择和汇总完成后才切换 active selection 指针；强杀后丢弃 staging，保留上一份完整选择。
- 永久删除：继续使用 tombstone；恢复时根据文件存在性和 SHA-512 意图决定只补 MySQL 映射删除，禁止盲目再次删除不同内容文件。
- 布局：加载最近一次周期性保存文件。
- 日志：每条记录写入后立即 flush，最后一条可能缺失时由任务记录和 checkpoint 补充“上次非正常退出”。

### RocksDB 锁安全

1. 新增按规范化 RocksDB 路径哈希生成的 named mutex，同一数据库目录只允许一个应用实例打开。
2. `RocksStore` 增加生命周期闸门：新操作在关闭开始后被拒绝；正在执行的 `Get/Put/WriteBatch/ForEachPrefix/DeletePrefix` 持有操作租约。
3. `Close()` 先获得独占关闭权并等待活动操作租约归零，再销毁 Column Family handle 和数据库实例。
4. `ForEachPrefix` 的迭代器必须在回调结束或异常退出路径全部销毁，不得把迭代器暴露给外部。
5. 正常退出必须先取消或清空三个线程池并 `join`，再停止 MySQL 同步，最后关闭 RocksDB。
6. 不删除 `LOCK` 文件；强杀后 Windows 自动释放文件句柄锁。获得 named mutex 后仍报告锁占用时，只记录错误并停止启动，避免损坏数据库。

### 主要文件

- 新增 `DedupCore/persistence/TaskRecoveryStore.h/.cpp`
- `DedupCore/persistence/RocksStore.h/.cpp`
- `DedupCore/orchestration/ScanCoordinator.cpp`
- `DedupCore/dedup/DuplicateReportService.cpp`
- `DedupCore/deletion/DeletionService.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `VideoScGUI/main.cpp`

### 验收

- 在扫描、报告、选择全部和删除的不同阶段强杀，重启后均得到确定恢复结果。
- 上一份完整报告不因半成品被覆盖。
- 第二个进程无法同时打开同一 RocksDB 目录。
- 正常退出和强杀恢复流程均不手工删除 `LOCK` 文件。

## 5.7 第 7 条：dHash 报告使用多线程

### 目标

dHash 报告中可并行的纯计算通过独立报告线程池执行，同时保持分组结果确定性。

### 方案

并行：

- 图片长宽比判断和汉明距离计算。
- 视频 6 帧距离和平均值计算。
- 候选边序列化前的纯结果构建。
- 对一个当前签名的多个候选组执行只读完整链接兼容性检查。

保持单线程：

- MySQL 流式读取顺序。
- 视觉签名代表选择。
- `group-of` 唯一归属提交。
- 多候选组最终择优。
- 正式组序号、稳定组 ID 和 active generation 发布。

工作结果先写线程安全结果队列或幂等规范化 RocksDB 键，归组线程只消费完整结果，不依赖工作线程完成顺序。

### 主要文件

- `DedupCore/dedup/DuplicateReportService.cpp`
- 新增公共线程池文件
- `DedupTests/main.cpp`

### 验收

- 线程池大小为 1、2、4 和上限值时报告完全一致。
- 任一任务异常会停止接单、取消待执行任务、等待在途任务退出，并且不发布半成品。
- 报告进度显示排队数、完成数、活动线程数和线程池上限。

## 5.8 第 8 条：多线程统一使用线程池技术

### 公共线程池

新增：

- `DedupCore/concurrency/TaskThreadPool.h`
- `DedupCore/concurrency/TaskThreadPool.cpp`

线程池职责：

- 固定最大工作线程数。
- 有界任务队列和生产者背压。
- `Submit()`、`CloseSubmissions()`、`DrainAndJoin()`、`CancelPendingAndJoin()`。
- 任务级异常捕获、首错误保存和失败回调。
- 排队、活动、完成、失败、取消数量快照。
- 析构幂等停止，不允许 detach。

线程池不负责：

- 业务重试。
- RocksDB 事务。
- ImGui 状态。
- 磁盘占用采样。
- CPU 或磁盘自适应策略。

### 三个独立实例

| 线程池 | 生命周期 | 硬上限来源 | 主要任务 |
| --- | --- | --- | --- |
| 文件读取线程池 | 一次扫描的发现完成至 SHA-512 阶段结束 | `storage.max_concurrent_file_reads` | 文件读取与流式 SHA-512 |
| 媒体计算线程池 | 一次扫描的媒体特征阶段 | `compute.worker_threads` | 图片 dHash、视频 6 帧、缩略图和 FFmpeg 媒体分析 |
| dHash 报告线程池 | 一次相似报告生成 | `dhash_similarity.validation_worker_threads` | 候选真实比较和只读组兼容性计算 |

三者分别拥有：

- 独立线程数组。
- 独立有界队列。
- 独立取消源。
- 独立活动计数。
- 独立异常状态。

协调器、磁盘采样器、CPU 采样器、MySQL 同步循环和 GUI 主循环属于长期控制线程，不塞入任务线程池。

### 迁移范围

- `DiskHashScheduler` 不再为每个盘直接创建固定 `std::thread` 数组，改为每盘队列加中央分发器向文件读取线程池提交实际文件任务。
- `ScanCoordinator::ProcessMediaPhase()` 不再临时创建工作线程数组，改为媒体计算线程池。
- `GenerateSimilar()` 删除内部 `CandidatePairQueue + std::thread` 组合，改为 dHash 报告线程池。
- 发现阶段按根目录创建的少量控制线程暂不迁移，避免把长生命周期枚举器占满文件读取池。

### 验收

- 三个池的活动线程和队列互不影响。
- 一个池取消不会取消另一个池。
- 关闭 RocksDB 前三个池均已停止并完成 `join`。
- 线程池队列满时形成背压，不无限增长内存。

## 5.9 第 9 条：智能文件读取线程数

### 目标

每个物理盘在有任务积压且读取占用未到目标前逐步增加并发，直到达到目标或线程池/全局上限。

### 磁盘采样

新增 `DiskUtilizationSampler`：

1. 从 `PhysicalDriveN` 或 `PhysicalSet:n,m` 解析成员物理盘。
2. 优先通过 Windows 物理磁盘性能计数获取采样区间内读取忙碌比例。
3. 第一次采样只建立基线，不扩容。
4. `PhysicalSet` 取成员盘最高读取占用。
5. 权限不足、设备离线、计数回绕或采样失败时写一次降级日志，并回退固定 HDD/SSD 线程数。

### 调度规则

每个盘维护：

```text
allowed_read_concurrency
active_read_count
queued_file_count
read_utilization_percent
sampling_available
```

智能模式：

```text
初始 allowed = 1
每秒：
  if queue_backlogged
     and utilization < target
     and allowed < per-pool/global remaining limit:
       allowed += 1
```

约束：

- 阶段内只增不减。
- 总活动读取数不超过文件读取线程池大小和 `max_concurrent_file_reads`。
- 单盘许可不等于创建新 OS 线程；只是放宽该盘向文件读取线程池提交任务的额度。
- 多盘中央分发器使用轮询公平策略，禁止繁忙 SSD 长期饿死 HDD。
- HDD 继续在该盘待处理队列内使用物理位置电梯选择。
- 没有任务积压时不扩容。
- 取消时唤醒分发器、采样器和等待提交者。

### GUI 进度

每盘显示：

- 当前读取占用百分比。
- 当前允许并发 / 回退或硬上限。
- 活动读取数。
- 排队文件数。
- 当前吞吐量。
- 采样状态或降级原因。

### 主要文件

- 新增 `DedupCore/scheduling/DiskUtilizationSampler.h/.cpp`
- `DedupCore/scheduling/DiskHashScheduler.h/.cpp`
- `DedupCore/orchestration/ProgressSnapshot.h`
- `DedupCore/orchestration/ScanCoordinator.cpp`
- `VideoScGUI/VideoScApp.cpp`

### 验收

- 多块 HDD/SSD 分别从 1 开始增长，互不共用单盘许可。
- 占用达到或超过 90% 后不再增加。
- `PhysicalSet` 任一成员盘达到目标即停止该目标增长。
- 采样失败能完成扫描，并明确显示固定线程回退。

## 5.10 第 10 条：删除操作显示进度条

### 目标

永久删除从预检、复核、删除到映射排队都提供实时进度。

### 删除进度模型

新增 `DeletionProgressSnapshot`：

```text
task_id
stage
stage_index / stage_count
selected_files / selected_bytes
processed_files / processed_bytes
deleted_files / reclaimed_bytes
skipped_files / failed_files
current_group_id / current_path
total_known
state / latest_error
```

建议阶段：

1. `loading_selection`
2. `verifying_retained_files`
3. `verifying_delete_candidates`
4. `deleting_files`
5. `queueing_mapping_deletes`
6. `completed`

### 方案

1. `DeletionExecutor::Execute()` 增加进度回调，至少在每个文件复核完成、删除完成、映射排队完成时发布。
2. 删除总文件数和总体积直接读取持久化选择摘要，不再按可见缓存临时估算。
3. 整个报告删除选择已经提前物化，因此执行删除时不再重新按策略生成另一份选择。
4. GUI 使用反色文字进度条，显示已处理/总文件、已处理/总体积、已释放大小和失败数。
5. 本次只增加进度，不新增“中途取消永久删除”按钮；异常退出由 tombstone 恢复处理。
6. 当前路径只用于显示和日志，不作为删除状态事实来源。

### 主要文件

- `DedupCore/deletion/DeletionService.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`

### 验收

- 删除数百个文件时 GUI 持续更新且不阻塞。
- 删除结束时进度总数与成功、跳过、失败之和一致。
- 强杀后重启能继续处理 tombstone，进度任务显示“上次删除被中断并已恢复”。

## 5.11 第 11 条：日志改为易读文本并记录详细失败原因

### 日志文件

新日志使用 UTF-8 文本：

```text
execution.log
execution-failures.log
scan-errors.log
operations.log
application-errors.log
```

旧 `*.jsonl` 文件保留，不迁移、不删除；升级后只向新文本文件写入。

### 单行文本格式

```text
2026-07-17T16:20:31.123+08:00 [ERROR] task=scan task_id=... stage=sha512 operation=read path="E:\\..." disk="PhysicalDrive2" status=read_timeout native_error=121 offset=... bytes_read=... detail="读取 60 秒无进展"
```

要求：

- 时间使用带时区 ISO 8601。
- 稳定字段名保持英文，说明文本使用中文。
- 路径、引号、换行和控制字符统一转义，保证一条事件占一行。
- 每条记录立即 flush；继续保留滚动、数量和保留天数策略。
- 多线程写入使用进程级互斥，禁止行内容交叉。

### 必须记录的失败

- 读取：打开失败、权限不足、共享冲突、坏块、超时、短读、取消、设备离线、路径消失。
- 计算：SHA-512、图片 dHash、视频 6 帧、缩略图、FFmpeg、长宽比数据无效、线程池任务异常。
- 报告：MySQL 读取、候选索引、汉明校验、严格分组、元数据、发布和半成品清理失败。
- 删除：保留文件复核失败、待删文件 SHA 不一致、`DeleteFileW` 错误、tombstone、映射排队和恢复失败。
- 持久化：RocksDB 打开/锁占用、读写、迭代器、批处理、同步和关闭失败。
- GUI：布局保存、预览生成和后台命令失败。

每条失败至少包含：任务 ID、阶段、操作、路径或 RocksDB 键类别、磁盘、系统错误码、状态码、偏移、已处理字节和详细原因。

内部 `config.json`、扫描 manifest、checkpoint、tombstone 和 RocksDB value 不是人工日志，继续使用适合恢复的版本化格式。

### 主要文件

- `DedupCore/logging/ExecutionLogger.h/.cpp`
- `DedupCore/logging/ApplicationErrorLogger.h/.cpp`
- `DedupCore/logging/ScanErrorLogger.h/.cpp`
- `DedupCore/deletion/DeletionService.cpp`
- 新增 `DedupCore/logging/TextLogFormatter.h/.cpp`
- `VideoScGUI/diagnostics/CrashHandler.cpp`

### 验收

- 使用记事本直接打开即可按时间和字段阅读。
- 人工制造读取、计算和删除失败后均有具体路径、错误码和原因。
- 日志目录不可写时任务在开始危险操作前失败，并通过可用的应急诊断通道提示。

## 5.12 第 12 条：选择状态直接写入数据

### 目标

可见列表、离屏列表、详情窗口、确认窗口和删除执行器读取同一份持久化选择状态。

### RocksDB 选择模型

新增 `ReportSelectionStore`，键按报告隔离：

```text
report-selection/<kind>/<report-generation>/active
report-selection/<kind>/<report-generation>/<selection-generation>/metadata
report-selection/<kind>/<report-generation>/<selection-generation>/group/<group-id>/member/<path-id>
report-selection/<kind>/<report-generation>/<selection-generation>/group-summary/<group-id>
```

元数据包含：

```text
selection_generation
source_report_generation
selected_file_count
selected_total_bytes
selected_group_count
updated_utc_ms
```

### 操作规则

1. 单个成员切换使用 RocksDB `WriteBatch` 同时更新成员选择、组摘要和全局摘要。
2. 同组至少保留一个活动成员；写入前由 `DeletionPlanner` 规则校验。
3. “按条件选择当前可见组”只作用于当前可见组，但选择结果写入持久数据，不依赖缓存寿命。
4. “按条件选择全部报告”流式读取全部组，写到新的 selection staging generation；完成后原子切换 active 指针。
5. 全选任务运行时显示自身进度，避免数百组时阻塞 GUI。
6. 强杀导致 staging 不完整时，下次启动删除 staging，继续使用上一份 active selection。
7. `AcquireReportGroup()` 加载组时从 `ReportSelectionSnapshot` 合并选择状态；渲染函数不再从 `m_reportSelections` 推断。
8. 新报告发布或报告删除时清理对应旧选择命名空间。

### 主要文件

- 新增 `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

### 验收

- 全选后滚动到从未显示过的组，其成员仍正确显示选中底色。
- 关闭并重启应用后选择状态仍存在。
- 切换排序、详情窗口和虚拟列表缓存不会丢失选择。
- 报告 generation 变化后不会把旧选择应用到新报告。

## 5.13 第 13 条：项目按数据驱动理念设计

### 实施边界

本次把受影响流程迁移为数据驱动，不对无关磁盘工具和 FFmpeg DLL API 做大规模重写。

### 方案

1. 报告、选择、删除、恢复和布局均建立明确状态模型。
2. GUI 按钮只生成命令；命令处理器调用 Core 服务。
3. Core 服务通过回调或线程安全快照发布状态，不引用 GUI 对象。
4. RocksDB Store 只负责持久化和范围读取；分组、选择策略、恢复策略分别由专用服务负责。
5. 列表缓存、缩略图缓存和纹理缓存明确标记为派生数据，可随时丢弃并由持久状态重建。
6. 任何统计值只维护一个事实来源：

   - 报告组数来自报告 metadata/count。
   - 选择数和选择总体积来自 selection metadata。
   - 删除进度来自 `DeletionProgressSnapshot`。
   - 线程数来自线程池 snapshot。

7. 状态更新带任务 ID/generation ID，GUI 丢弃过期后台任务快照，避免旧线程覆盖新任务界面。
8. 配置编辑态与运行冻结态分离；页面明确显示当前任务实际使用值。

### 验收

- 清空 GUI 缓存后可以从 RocksDB 和服务快照恢复完整选择与报告状态。
- UI 是否可见不影响业务操作结果。
- 后台任务完成顺序变化不会产生过期状态覆盖。
- Core 单元测试不依赖 ImGui。

## 5.14 第 14 条：显示选中文件总体积

### 目标

重复报告主窗口和永久删除确认窗口实时显示当前报告的选中文件数及总体积。

### 方案

1. `ReportSelectionMetadata` 持久化 `selected_file_count` 和 `selected_total_bytes`。
2. 单文件切换时在同一 `WriteBatch` 中增减计数和字节，禁止 UI 重新扫描全部缓存。
3. 按组或全报告选择时由后台选择任务流式累计，active selection 发布后一次性切换汇总。
4. GUI 显示：

   ```text
   已选择 1,234 个文件，共 87.6 GiB，涉及 318 个重复组
   ```

5. 删除过程中同时显示原始选择总体积和已成功释放体积；跳过或失败文件不计入已释放。
6. 删除完成后从持久化选择中移除已成功删除的成员，并重新计算或原子修正选择摘要。
7. 若摘要损坏，提供从选择成员前缀流式重建摘要的内部修复接口，并记录文本日志。

### 主要文件

- `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `VideoScGUI/VideoScApp.cpp`
- `DedupCore/deletion/DeletionService.cpp`

### 验收

- 单个切换、当前可见组选择、全报告选择和删除后，数量与总体积均准确。
- 选中状态跨重启后汇总一致。
- 不因报告中存在几百个大组而每帧重新遍历全部成员。

## 5.15 第 15 条：重复组主列表按平均汉明距离排序

### 目标

dHash 相似重复组主列表增加 `平均汉明距离 ↑` 和 `平均汉明距离 ↓`，按每组规范化相似证据的平均距离进行稳定排序。

### 方案

1. 报告生成时按 `group.evidence` 计算组平均距离；相同视觉签名下的多条路径不重复加权。
2. 扩展 `ReportGroupSummary`，保存平均距离有效标记和 `double` 原始值，并提升摘要 codec 版本。
3. 排序只读取轻量摘要，不在 GUI 线程加载完整组或重新计算 dHash。
4. 正序、倒序第一键为平均距离，第二键固定为 `group_id` 升序。
5. 相似报告组标题显示两位小数平均距离；SHA-512 精确报告不显示 dHash 排序按钮。
6. 旧 dHash 摘要缺少平均距离时提示重新生成报告，不静默按 `0` 排序。

### 主要文件

- `DedupCore/dedup/DuplicateReportService.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

### 验收

- 图片和视频组均能按真实平均距离正序、倒序稳定排列。
- 同一签名增加多条路径不会改变组平均距离。
- 排序不改变组成员、选择状态、可释放大小或报告 generation。

详细方案见：[第 15 条独立修改文档](2026-07-17-15-dhash-average-distance-sorting-plan.md)。

## 5.16 第 16 条：选择与删除使用 dHash 安全距离上限

### 目标

增加图片、视频两个选择安全距离输入框。保留策略先确定保留文件，只有候选文件相对保留文件的真实距离严格小于对应有效上限时，才能进入待删除状态。

### 方案

1. 图片使用单个 64 位 dHash 汉明距离；视频使用 6 帧对应距离平均值，任意一帧为 `0` 时拒绝选择。
2. 输入为空时沿用报告生成时冻结的对应阈值；输入值与空值状态保存到 `config.json`。
3. 两个“按条件选择”按钮和单成员选择统一走服务层规则，GUI 不自行判断距离。
4. 选择记录保存保留成员、实测距离、严格上限、上限来源和报告 generation。
5. 选择汇总只累计通过安全规则的文件；永久删除前再次复核，失败项按“安全规则跳过”处理并写入易读文本日志。
6. SHA-512 精确报告保持原选择语义，不应用 dHash 上限。

### 主要文件

- `DedupCore/config/AppConfig.h`
- `DedupCore/config/JsonConfigStore.cpp`
- `DedupCore/config/ConfigValidator.cpp`
- `DedupCore/dedup/DHashSimilarity.h/.cpp`
- `DedupCore/dedup/ReportSelectionStore.h/.cpp`
- `DedupCore/deletion/DeletionService.h/.cpp`
- `VideoScGUI/VideoScApp.h/.cpp`
- `DedupTests/main.cpp`

### 验收

- 输入上限为 `n` 时距离等于 `n` 的文件不会选中，只有距离 `< n` 的文件可选中。
- 图片、视频输入分别生效，空值准确使用报告 generation 的冻结阈值。
- 选择入口和删除执行器均不能绕过安全规则，SHA-512 报告结果不变。

详细方案见：[第 16 条独立修改文档](2026-07-17-16-selection-dhash-safety-threshold-plan.md)。

## 6. 报告元数据与兼容策略

相似报告 schema 升级，至少包含：

```text
report_schema_version
media_algorithm_version
image_bucket_index_version
video_bucket_index_version
image_grouping_rule = image-complete-link-disjoint-v2
video_grouping_rule = video-average-complete-link-disjoint-v1
image_aspect_ratio_rule = relative-ratio-v1
image_max_hamming_distance
video_max_average_hamming_distance
image_aspect_ratio_tolerance_percent
validation_worker_threads
generated_utc_ms
```

兼容规则：

- SHA-512 精确报告 codec 不因本次 dHash 规则变更而升级。
- 旧相似报告缺少任一必要规则字段时提示重新生成。
- dHash 组摘要 codec 增加平均汉明距离；旧摘要缺少该字段时禁用平均距离排序并提示重新生成，不在 GUI 线程临时补算。
- 当前配置与报告快照不一致时，详情始终显示报告实际规则，并提示配置变化。
- 新报告发布后清理上一代报告及其 selection namespace；清理过程可中断恢复。

## 7. 预计新增与修改文件

### 7.1 新增文件

| 文件 | 职责 |
| --- | --- |
| `DedupCore/concurrency/TaskThreadPool.h/.cpp` | 有界线程池、取消、异常和快照 |
| `DedupCore/scheduling/DiskUtilizationSampler.h/.cpp` | Windows 物理盘读取占用采样 |
| `DedupCore/persistence/TaskRecoveryStore.h/.cpp` | 任务意图、心跳、中断状态和启动恢复 |
| `DedupCore/dedup/ReportSelectionStore.h/.cpp` | 持久化报告选择、汇总和 staging 发布 |
| `DedupCore/logging/TextLogFormatter.h/.cpp` | 易读 UTF-8 单行文本日志格式化 |

### 7.2 主要修改文件

| 文件 | 修改内容 |
| --- | --- |
| `DedupCore/config/AppConfig.h/.cpp` | 新阈值、长宽比、选择安全上限和智能读取配置 |
| `DedupCore/config/JsonConfigStore.cpp` | schema、迁移和保存 |
| `DedupCore/config/ConfigValidator.cpp` | 新字段范围和交叉约束 |
| `DedupCore/orchestration/ScanOptions.cpp` | 冻结新扫描配置 |
| `DedupCore/orchestration/ScanOptionsCodec.cpp` | checkpoint 配置快照迁移 |
| `DedupCore/dedup/DHashSimilarity.h/.cpp` | 视频动态候选分段和配置平均阈值 |
| `DedupCore/dedup/DuplicateReportService.h/.cpp` | 严格图片/视频分组、长宽比、平均距离摘要、线程池、进度和 metadata |
| `DedupCore/scheduling/DiskHashScheduler.h/.cpp` | 文件读取线程池、中央分发和每盘动态许可 |
| `DedupCore/orchestration/ProgressSnapshot.h` | 每盘占用、许可和线程池快照 |
| `DedupCore/orchestration/ScanCoordinator.h/.cpp` | 三池生命周期、媒体池和恢复记录 |
| `DedupCore/deletion/DeletionService.h/.cpp` | 删除进度、选择安全距离、选择事实来源和恢复日志 |
| `DedupCore/persistence/RocksStore.h/.cpp` | 生命周期操作租约和安全关闭 |
| `DedupCore/logging/*.h/.cpp` | JSONL 改为易读文本日志 |
| `VideoScGUI/RenderBackend.h/.cpp` | 显式和周期布局保存 |
| `VideoScGUI/main.cpp` | 关闭顺序和单实例生命周期 |
| `VideoScGUI/VideoScApp.h/.cpp` | 数据驱动状态、配置、平均距离排序、安全选择、进度和总体积 UI |
| `DedupCore/DedupCore.vcxproj` | 登记新增源码文件 |
| `DedupTests/main.cpp` | 配置、分桶、线程池、恢复、选择和日志测试 |

## 8. 分阶段执行顺序

收到“执行”命令后按以下顺序实施，避免一次改动同时破坏持久化、线程和 GUI：

### 阶段 1：测试基线与配置

1. 记录当前 Debug/Release 构建和测试基线。
2. 增加新配置、schema 迁移、校验和扫描快照。
3. 增加相似报告 metadata v2 和旧报告拒绝测试。

### 阶段 2：公共线程池

1. 实现 `TaskThreadPool` 单元测试。
2. 验证有界背压、Drain、Cancel、异常和析构。
3. 暂不接入 GUI，先确保线程生命周期可靠。

### 阶段 3：dHash 规则与报告线程池

1. 实现图片长宽比规则和签名键调整。
2. 实现视频 384 位动态候选分段和平均阈值。
3. 把候选验证迁移到 dHash 报告线程池。
4. 保留单线程稳定 complete-link 提交，并补齐严格不变量测试。
5. 生成包含平均汉明距离的轻量组摘要。
6. 更新报告 metadata、进度和日志统计。

### 阶段 4：文件读取与媒体计算线程池

1. 把媒体阶段迁移到独立媒体计算线程池。
2. 重构 `DiskHashScheduler` 为每盘队列、中央分发和文件读取线程池。
3. 增加磁盘采样器、每盘动态许可、`PhysicalSet` 和固定回退。
4. 补齐取消、异常和多盘公平性测试。

### 阶段 5：持久选择与总体积

1. 实现 `ReportSelectionStore` 和 staging generation。
2. 把单选、当前可见组和全报告选择改为数据命令。
3. 增加图片、视频安全选择上限，并把实测距离与冻结规则写入选择记录。
4. 永久删除前复核选择安全规则，失败项安全跳过并记录日志。
5. GUI 改为渲染 selection snapshot，并增加平均距离正序、倒序按钮。
6. 显示选中文件数、总体积和组数。

### 阶段 6：删除进度与文本日志

1. 增加删除进度回调和 GUI 进度条。
2. 删除执行器只消费持久化选择，不在执行时重新选择。
3. 统一文本日志格式，并迁移全部人工日志输出。
4. 人工注入读取、计算和删除失败，核对详细原因。

### 阶段 7：中断恢复、RocksDB 和布局

1. 增加任务恢复记录和启动恢复协调器。
2. 增加 named mutex、RocksDB 操作租约和安全关闭顺序。
3. 清理未发布报告/选择 staging，不删除 `LOCK` 文件。
4. 增加显式与周期布局保存。
5. 执行多阶段强杀恢复测试。

### 阶段 8：GUI 数据驱动收口

1. 把报告、选择、删除、恢复和布局统一接入状态切片。
2. 移除这些流程中以可见缓存作为事实来源的旧字段和分支。
3. 校验过期任务快照不会覆盖新任务。

### 阶段 9：构建与验收

1. Debug x64 全解决方案构建。
2. Release x64 全解决方案构建。
3. Debug/Release `DedupTests` 全部通过。
4. 使用真实 MySQL、HDD、SSD 和 `PhysicalSet` 数据执行人工验收。
5. 把实际执行结果、未覆盖环境和性能数据回填到本计划。

## 9. 测试矩阵

### 9.1 配置与兼容

- 旧配置加载后新字段使用默认值。
- 新配置和扫描 checkpoint 往返一致。
- 图片/视频阈值、长宽比、磁盘目标和线程数非法值被拒绝。
- 旧相似报告提示重新生成，精确报告不受影响。

### 9.2 图片严格分组

- 任意组内全部图片签名对距离 `<= image n`。
- 容差 0%、10%、100% 的长宽比边界。
- 相同 dHash、相同比例和不同比例的大量文件。
- 分桶碰撞候选被真实比较拒绝。

### 9.3 视频严格分组

- 6 帧平均值小于、等于和大于阈值。
- 任一帧为 0、缺失 dHash、静态画面和时长差超过 2 秒继续被排除。
- 对 `n=0..15` 验证 384 位动态分段无漏召回。
- 组内全部视频对平均距离 `<= video n`。

### 9.4 线程池

- 三个池大小分别为 1、2、4 和最大值。
- 队列满背压、任务异常、内存不足、取消、Drain 和析构。
- 一个池失败或取消不污染另两个池。
- 所有工作线程在 RocksDB 关闭前完成 `join`。

### 9.5 智能磁盘读取

- 单 HDD、单 SSD、多 HDD、多 SSD、混合盘和 `PhysicalSet`。
- 占用低于目标时逐秒加 1，达到目标后停止。
- 全局池上限小于各盘许可总和时仍公平推进。
- 采样失败、设备离线和权限不足时固定策略回退。
- 取消扫描后采样器、分发器和线程池及时退出。

### 9.6 选择和总体积

- 虚拟列表离屏组全选。
- 排序、切换详情、清缓存和重启后状态一致。
- dHash 组平均距离正序、倒序及相同距离的稳定第二排序键。
- 相同视觉签名的路径数量变化不影响组平均距离。
- 图片、视频有效选择上限的 `<` 边界值、空值回退和非法输入。
- 当前可见组、全部报告、单成员选择和删除前复核均不能绕过安全距离。
- 选择 staging 中强杀后回退上一份 active selection。
- 数量和字节汇总与成员逐项求和一致。
- 新报告 generation 不复用旧选择。

### 9.7 删除和恢复

- 保留文件不存在、SHA 不一致、待删文件改变、权限失败和设备离线。
- 删除前、删除后但映射未排队、映射已排队三个位置强杀。
- 重启后不会删除已经变成其他内容的文件。
- 删除进度最终满足：成功 + 跳过 + 失败 = 已处理。

### 9.8 RocksDB 生命周期

- 两个实例打开同一目录时第二个被拒绝。
- 正常退出时活动操作归零后关闭。
- 强杀后无需删除 `LOCK` 文件即可重启。
- 模拟仍被其他进程占用时只报告错误，不尝试破坏锁。
- 未发布报告 work 和选择 staging 能分批、幂等清理。

### 9.9 日志和布局

- 所有新日志是 UTF-8 易读文本，不再新增 JSONL 人工日志。
- 并发写入不交叉，滚动和保留策略有效。
- 各类失败包含路径、错误码、阶段和原因。
- 正常关闭恢复最后布局；强杀恢复最近周期保存布局。

## 10. 性能与安全约束

1. 严格分组不得退化为全量文件路径两两比较；只在不同视觉签名层计算并复用相似边。
2. 图片长宽比过滤在 `popcount` 前执行，但不能加入会造成边界漏召回的粗糙桶过滤。
3. 视频动态分段按 384 位总距离保证召回，不使用无法随阈值证明完备性的固定分段。
4. 报告、选择和删除均流式访问 RocksDB，不一次性加载全部大组。
5. 线程池队列有界；进度和日志不得持有业务队列锁执行磁盘 IO。
6. 不在持有 RocksDB 操作租约时等待线程池任务结束，避免关闭死锁。
7. 永久删除保持串行安全语义；线程池改造不用于并行 `DeleteFileW`。
8. 强杀恢复优先保证数据安全，不承诺续跑半个报告。
9. 不删除 RocksDB `LOCK` 文件，不调用破坏性数据库重建作为自动恢复手段。
10. 旧日志、旧报告和旧 checkpoint 的清理由明确版本和命名空间控制，不做模糊目录删除。
11. 平均距离排序只读取轻量组摘要，不因排序加载完整组、预览图或媒体文件。
12. dHash 选择记录保存冻结的安全规则；删除执行器必须复核，不信任仅来自 GUI 的选中标记。

## 11. 最终验收标准

1. 图片和视频分别使用配置页中的独立阈值。
2. 图片组内任意两项 dHash 距离不超过图片阈值。
3. 视频组内任意两项的 6 帧平均汉明距离不超过视频阈值。
4. 图片长宽比超过配置容差时不会执行汉明距离比较或进入同组。
5. SHA-512 与 dHash 报告均显示实时阶段进度条。
6. 文件读取、媒体计算和 dHash 报告使用三个独立线程池。
7. 每盘读取并发在占用低于目标时逐步增加，达到目标或硬上限后停止。
8. 删除过程显示文件数、字节数、当前阶段、成功、跳过和失败进度。
9. 人工日志全部使用易读 UTF-8 文本，并记录未完成操作的详细原因。
10. 全报告选择不依赖可见缓存，离屏成员能够正确显示和执行。
11. 选择状态跨重启持久化，并按报告 generation 隔离。
12. 页面实时显示选中文件数量和总体积。
13. 报告、选择、删除、恢复和布局使用单向数据驱动状态流。
14. 正常关闭明确保存布局并安全关闭三个线程池和 RocksDB。
15. 强杀后保留可恢复扫描、清理报告半成品、恢复删除意图并保留上一份完整报告。
16. 同一 RocksDB 目录不能被两个实例同时打开，恢复过程不删除 `LOCK` 文件。
17. dHash 重复组主列表可按组平均汉明距离正序、倒序稳定排列，排序不加载完整组或改变选择状态。
18. 图片和视频候选只有在相对保留文件的距离严格小于对应有效上限时才可选中；空值沿用报告生成阈值，删除前再次复核，SHA-512 报告不受影响。
19. Debug/Release x64 构建通过，自动化测试全部通过。

## 12. 本轮不执行的内容

- 不修改任何业务源码。
- 不迁移 MySQL 表结构。
- 不改变音频只参与 SHA-512 精确去重的规则。
- 不新增永久删除取消按钮。
- 不自动删除旧 JSONL 日志。
- 不自动修复或重建损坏的 RocksDB。
- 不把视频跨严格组关系加入详情 UI。
- 不按组内最大距离、中位数或与保留文件的距离对重复组主列表排序。
- 不让 dHash 选择安全输入反向修改报告生成阈值，也不把该限制应用到 SHA-512 精确报告。
