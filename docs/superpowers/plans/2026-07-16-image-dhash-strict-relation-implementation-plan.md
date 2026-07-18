# 图片 dHash 严格主分组与完整相似关系实施计划

> 日期：2026-07-16  
> 状态：策略已确认，等待执行命令  
> 方案：非重叠严格主分组 + 完整直接相似关系  
> 图片判定口径：`HammingDistance(left, right) <= n`  
> 范围：只调整图片 dHash 相似报告；不改变 SHA-512 精确报告、视频六帧 dHash 判定和删除执行器

## 1. 实施目标

本次同时保证两个不同层面的结果：

1. **严格主分组**

```text
每个图片内容只归属一个主组
同一主组内任意两个不同 dHash 的汉明距离都 <= n
```

2. **完整直接相似关系**

```text
任意两个已进入报告输入且汉明距离 <= n 的有效图片 dHash，
即使最终位于不同严格主组，也必须能够在详情界面中查询到。
```

主分组继续负责：

- 重复结果主列表。
- 可释放空间统计。
- 保留与删除选择。
- 组内成员详情。

直接相似关系负责：

- 补充显示因严格完整链接约束而分处不同主组的图片。
- 解释链式关系和重叠相似关系。
- 排查“候选已命中，但最终不在同组”的情况。

跨组直接相似关系只读展示，不参与可释放空间统计，也不直接生成删除选择。

## 2. 核心数据语义

### 2.1 不按文件路径保存平方级关系

多个 SHA-512 内容可能具有完全相同的 64 位图片 dHash；同一 SHA-512 内容也可能对应多个活动路径。

如果按文件路径两两保存关系，下面的情况会产生大量重复数据：

```text
dHash A 有 200 个活动文件
dHash B 有 300 个活动文件
distance(A, B) <= n

文件级关系数量 = 200 × 300 = 60000
```

因此，关系图以完整 64 位 dHash 视觉签名为节点：

```text
图片视觉签名 = 完整 64 位 image_dhash
```

每一条不同 dHash 间的关系只保存一次：

```text
signature_edge(dhash_a, dhash_b, distance)
```

完全相同的 dHash 不保存两两边，使用同一签名下的成员集合隐式表达距离为 0。

界面展开某条签名关系时，再按需读取两端的活动文件成员。这样可以完整表达文件关系，同时避免持久化文件路径的笛卡尔积。

### 2.2 主分组和关系图的组合含义

全部直接相似关系由两部分组成：

```text
主组内关系：
    由严格组的“任意两项 <= n”不变量隐式保证

跨主组关系：
    由持久化的 signature_edge 显式保存
```

对于：

```text
A-B <= n
B-C <= n
A-C > n
```

允许生成：

```text
主组 G1 = {A, B}
主组 G2 = {C}
跨组关系 = B-C
```

主列表不把 `A、B、C` 错误合并，但详情界面仍能查询 `B-C`。

### 2.3 内容、签名和路径的层级

```text
活动文件路径
    -> SHA-512 内容
        -> 图片 dHash 签名
            -> 唯一严格主组
```

约束：

- 同一个 SHA-512 内容只能对应一个报告快照 dHash。
- 相同 dHash 的多个 SHA-512 内容属于同一个签名节点。
- 同一个签名节点只能归属一个严格主组。
- 一个严格主组可以包含多个不同签名节点。
- 一个签名节点可以与其他严格主组中的多个签名节点建立直接相似关系。

## 3. 候选召回和真实比较修改

### 3.1 保留动态 `n+1` 分桶

继续使用当前分段规则：

```text
segment_count = n + 1
```

保留以下逻辑：

- 完整覆盖 64 位。
- 相同 `n` 使用稳定的分段布局。
- 桶键包含 `n`、分段序号、位宽和值。
- 查询全部 `n+1` 个分段桶。
- 对同一候选签名去重。

分桶只负责候选召回，不作为最终相似判定。

### 3.2 候选回调立即执行真实距离复核

当前候选回调主要收集候选已归属的组，然后由完整链接校验间接排除假候选。

修改后，每个候选签名首先执行：

```text
distance = popcount(current_dhash XOR candidate_dhash)
```

结果分支：

```text
distance > n
    -> 桶碰撞假候选
    -> 不写关系
    -> 不把候选所在组加入候选组集合

distance <= n
    -> 写入规范化临时签名边
    -> 把候选所在组加入候选组集合
    -> 继续执行组内完整链接校验
```

规范化临时键：

```text
work/edge/<min_signature_representative>/<max_signature_representative>
```

值至少包含：

```text
relation_codec_version
media_kind
left_dhash
right_dhash
hamming_distance
```

相同候选命中多个桶时，只保留一条边。

### 3.3 相同 dHash 内容继续压缩

相同完整 dHash 的多个 SHA-512 内容继续折叠到一个视觉签名代表：

```text
signature/<dhash> -> representative_sha512
visual-representative/<sha512> -> representative_sha512
```

需要额外持久化该签名下的内容和活动路径成员，但不生成同签名成员的两两关系。

同签名成员之间的图片汉明距离固定为：

```text
0
```

## 4. 严格主分组修改

### 4.1 保留唯一主组归属

继续按稳定 SHA-512 顺序处理签名代表，并保存：

```text
group-of/<signature_representative> -> strict_group_root
```

每个签名代表只能写入一个 `group-of`。

### 4.2 候选组必须来自真实直接相似签名

只有已通过 `distance <= n` 真实复核的候选签名，其所属主组才可以进入当前内容的候选组集合。

这项调整不会替代完整链接校验，只用于：

- 避免分桶假候选触发无意义的整组扫描。
- 同时生成完整直接相似关系。
- 让“候选关系”和“分组候选来源”使用同一真实判定结果。

### 4.3 加入主组前校验全部签名

对每个候选主组逐一读取全部不同 dHash 签名，并执行：

```text
for every member_signature in candidate_group:
    if HammingDistance(current, member_signature) > n:
        reject candidate_group immediately
```

只有全部通过，当前签名才能加入该组。

禁止使用以下替代规则：

- 只与组根比较。
- 只与第一个成员比较。
- 只与平均或中心 dHash 比较。
- 只因为存在一条候选边就合并整个组。
- 使用相似关系的传递闭包合并组。

### 4.4 多个严格组都可接纳时

保持确定性选择：

1. 当前签名到组内成员的最大距离更小。
2. 最大距离相同时，平均距离更小。
3. 仍相同时，稳定组根字典序更小。

该选择只决定唯一主组归属，不删除当前签名与其他组之间已确认的直接相似关系。

## 5. 跨组关系组织

### 5.1 在严格分组完成后解析边两端主组

遍历所有临时签名边，并分别解析：

```text
left_group = group-of(left_signature)
right_group = group-of(right_signature)
```

分支：

```text
left_group == right_group
    -> 属于严格主组内部关系
    -> 不需要写跨组关系

left_group != right_group
    -> 写跨组签名关系
    -> 标记左右签名都需要保存活动成员
```

同组全部签名对已由完整链接不变量保证，无需重复保存组内平方级边。

### 5.2 为两个端点写对称关系索引

每条跨组签名边在两个主组下各保存一条只读索引：

```text
relation/<left_group_id>/<left_ordinal>
relation/<right_group_id>/<right_ordinal>
```

两个方向引用同一规范化签名关系，便于从任意一端详情窗口直接按组加载。

每个主组保存：

```text
relation-count/<group_id>
```

关系行使用固定序号，因此 GUI 可以按可见范围直接 `Get`，不需要从前缀开头反复跳过。

### 5.3 单签名严格组也必须保留关系成员

链式关系中，跨组关系的一端可能是没有进入主列表的单签名严格组。

例如：

```text
主组 G1 = {A, B}
主组 G2 = {C}
跨组关系 = B-C
```

`G2` 虽然只有一个不同内容签名，仍必须保存与 `C` 对应的活动成员，供 `G1` 的跨组关系详情展开。

因此活动路径连接阶段的保存条件调整为：

```text
主组不同内容数 >= 2
OR
签名参与至少一条跨组关系
```

单签名组不加入主结果列表，不计算可释放空间，只作为跨组关系端点存在。

## 6. 正式报告持久化设计

正式键统一位于：

```text
report/similar/<generation>/
```

### 6.1 报告元数据

新增：

```text
metadata
```

元数据至少包含：

```text
report_schema_version
group_codec_version
similarity_relation_codec_version
media_algorithm_version
image_bucket_index_version
image_grouping_rule = image-complete-link-disjoint-v1
image_relation_rule = image-signature-cross-group-v1
image_max_hamming_distance
generated_utc_ms
```

报告元数据和 active generation 必须在最终发布批次中形成一致状态。

### 6.2 严格主组

保留现有：

```text
group/<ordinal>
summary/<ordinal>
count
```

主组序列化仍只保存主组成员、主组证据和可释放空间，不把跨组成员混入 `DuplicateGroup.members`。

### 6.3 跨组关系

新增：

```text
relation-count/<group_id>
relation/<group_id>/<ordinal>
```

关系摘要至少包含：

```text
current_signature_representative
current_image_dhash
current_group_id
neighbor_signature_representative
neighbor_image_dhash
neighbor_group_id
hamming_distance
neighbor_group_in_main_report
```

### 6.4 签名活动成员

只为跨组关系涉及的签名保存：

```text
signature-member-count/<signature_representative>
signature-member/<signature_representative>/<path_id>
```

成员值复用报告中的 `DuplicateMember` 编码，包含：

- 活动路径 ID。
- 完整路径。
- SHA-512。
- 图片 dHash。
- 文件大小。
- 磁盘标识。
- 图片尺寸。
- 修改时间。
- 缩略图所需源路径。

没有活动路径的关系端点不在 GUI 中发布；同时计入“无活动路径关系端点”日志统计。

### 6.5 原子发布和清理

生成期间所有中间键继续放在：

```text
report/similar/work/<generation>/
```

只有以下数据全部写完后才发布：

- 严格主组。
- 主组摘要。
- 报告元数据。
- 跨组关系索引。
- 关系签名活动成员。
- 关系和成员计数。

取消或失败时：

- 删除当前 generation 的正式半成品。
- 删除工作键。
- 删除临时候选桶。
- 保留上一代 active 报告。

成功发布后：

- active 指向新 generation。
- 删除上一代相似报告。
- 删除本代工作键和临时候选桶。

## 7. 报告存储接口

在 `DuplicateReportService.h/.cpp` 中增加报告专用模型：

```text
SimilarReportMetadata
SimilarImageRelationSummary
```

在 `DuplicateReportStore` 中增加：

```text
SaveSimilarMetadata(...)
LoadSimilarMetadata(...)
SaveImageRelation(...)
ImageRelationCount(...)
LoadImageRelationRange(...)
SaveImageSignatureMember(...)
ImageSignatureMemberCount(...)
LoadImageSignatureMemberRange(...)
```

范围加载按固定序号直接读取：

```text
start_ordinal
maximum_items
```

不把报告关系模型加入扫描和同步使用的通用 `CoreModels`，避免报告专用结构污染扫描数据边界。

## 8. GUI 实施计划

### 8.1 报告头部显示规则元数据

dHash 相似报告主窗口显示：

```text
图片阈值：汉明距离 <= n
严格分组：组内任意两项均满足阈值
跨组关系：已启用
生成时间
generation ID
媒体算法版本
```

删除当前固定文字：

```text
阈值严格 < 5
```

所有阈值文本从报告元数据读取。

### 8.2 详情窗口增加两个区域

详情窗口保留现有“组内成员”区域，并增加：

```text
跨组直接相似
```

跨组关系表格列：

```text
当前 dHash
对方 dHash
汉明距离
对方严格组
对方活动文件数
查看成员
```

全部 dHash 使用 16 进制显示。

### 8.3 关系成员按需展开

用户展开一条跨组关系后，显示对方签名下的活动成员：

```text
预览
完整路径
SHA-512
dHash
磁盘
大小
尺寸和时间
```

加载要求：

- 不一次性加载全部关系。
- 不一次性生成全部缩略图。
- 使用内部范围缓存和 `ImGuiListClipper`。
- 滚动到可见位置时才加载关系摘要。
- 关系展开后才加载该签名成员。
- 成员进入可见区域后才请求图片临时缩略图。
- 关闭详情或切换报告时释放关系缓存和纹理。
- 界面不显示分页控件。

### 8.4 跨组关系保持只读

跨组关系成员不显示 `[保留]/[删除]` 切换。

原因：

- 同一个文件可能与多个严格组有关。
- 在跨组关系中直接删除会造成多组删除计划冲突。
- 可释放空间不能对重叠关系重复累计。

如需删除，用户返回对应严格主组执行现有删除选择。

### 8.5 旧报告和阈值变化

相似报告加载时必须读取元数据。

处理规则：

```text
缺少元数据或关系规则版本不支持
    -> 标记为旧报告
    -> 提示重新生成
    -> 不把它解释为当前严格关系结果

报告 n != 当前配置 n
    -> 显示“报告按 n=旧值生成”
    -> 明确标记配置不一致
    -> 提示重新生成
```

不自动删除旧报告，继续使用现有“删除 dHash 重复报告”按钮显式清理。

## 9. 多线程汉明距离校验

### 9.1 并行边界

多线程只作用于无共享分组状态的纯计算阶段：

```text
读取两个图片签名的 64 位 dHash
    -> popcount(left XOR right)
    -> 判断 distance <= n
    -> 生成规范化相似边结果
```

以下操作保持单线程确定性：

- 视觉签名代表选择。
- 严格主组归属。
- 多个候选组之间的最优组选择。
- 主组序号和稳定组 ID 生成。
- 正式报告发布。

汉明距离计算彼此独立，适合线程池并行；严格归组依赖稳定 SHA-512 顺序和已经生成的 `group-of` 状态，并行修改会让结果受线程调度顺序影响。

### 9.2 独立线程配置

建议在 `DHashSimilarityConfig` 中新增：

```text
dhash_similarity.validation_worker_threads
```

配置页面“dHash 相似报告”区域新增：

```text
dHash 汉明距离校验线程数
```

配置在报告启动时冻结，运行中修改只影响下一次报告。

建议该配置独立于：

```text
compute.worker_threads
compute.adaptive_worker_threads
compute.cpu_target_percent
```

原因：

- 现有自动计算线程明确只作用于 SHA-512 和媒体特征阶段。
- dHash 报告可以在扫描完成后单独运行。
- 报告线程池与扫描计算线程的生命周期不同。

以下细节等待用户确认：

- 是否采用独立固定线程数，不复用扫描的自动分配线程。
- 默认线程数和最大允许线程数。

建议值：

```text
默认值：4
有效范围：1–256
```

配置非法时禁止保存和启动 dHash 报告，不静默截断。

### 9.3 候选任务准备

为支持并行纯计算，将候选召回和严格归组拆开：

1. 按稳定签名顺序召回分桶候选。
2. 把候选对规范化：

```text
left = min(current_signature, candidate_signature)
right = max(current_signature, candidate_signature)
```

3. 写入 RocksDB 临时候选任务：

```text
work/candidate-pair/<left><right>
```

4. 同一候选命中多个分段桶时由规范化键自然去重。
5. 候选枚举完成后顺序统计唯一任务总量。
6. 将该总量作为并行校验阶段进度条分母。

候选任务只保存签名代表，不复制路径和完整成员信息。

### 9.4 有界生产者和工作线程池

并行校验使用：

```text
1 个 RocksDB 顺序生产者
N 个汉明距离校验工作线程
1 个有界任务队列
```

生产者：

- 顺序遍历 `candidate-pair`。
- 把签名对放入有界队列。
- 队列满时等待，禁止无限堆积内存。
- 收到取消或任一工作线程失败时停止生产并关闭队列。

工作线程：

1. 从队列取出候选签名对。
2. 加载两端图片 dHash。
3. 执行完整 64 位汉明距离计算。
4. 距离 `> n` 时计入桶碰撞拒绝数。
5. 距离 `<= n` 时写入规范化签名边和双向邻接临时索引。
6. 原子增加“已完成校验任务数”。

队列容量使用线程数的有限倍数，例如：

```text
max(64, validation_worker_threads * 8)
```

最终倍数通过基准测试确认；不得把全部候选任务加载到内存。

### 9.5 并发写入规则

通过真实校验的边写入同一 RocksDB 批次：

```text
work/edge/<left><right>
work/adjacency/<left>/<right>
work/adjacency/<right>/<left>
```

约束：

- 键全部规范化或带明确方向，不允许工作线程共享可变容器。
- 同一候选对只有一个任务。
- 即使异常重复投递，相同键的幂等覆盖也不得改变结果。
- 工作线程不得直接修改普通进度字段、GUI 状态或严格组映射。
- 统计使用原子计数器，由报告线程节流汇总到进度快照。

### 9.6 线程异常、取消和生命周期

每个工作线程必须捕获：

- `std::bad_alloc`。
- `std::exception`。
- 未知异常。

首个失败：

1. 原子发布失败标志。
2. 保存第一条完整错误信息。
3. 停止领取新任务。
4. 唤醒生产者和其他等待线程。
5. 等待所有已创建线程 `join`。
6. 由报告主线程统一返回失败并清理 generation。

取消：

- 停止候选任务生产。
- 关闭队列。
- 尚未开始的任务不再执行。
- 正在执行的单次 64 位汉明距离计算完成后退出。
- 等待全部线程安全结束。
- 不发布半成品报告。

禁止：

- `detach` 工作线程。
- 工作线程直接调用 ImGui。
- 工作线程直接发布 active generation。
- 因单个线程异常继续发布部分关系结果。

### 9.7 进度快照

`DuplicateReportProgress` 增加：

```text
candidate_pairs_total
validated_candidate_pairs
accepted_similarity_pairs
rejected_bucket_collisions
configured_validation_threads
active_validation_threads
```

专用阶段键：

```text
validating_dhash_distances
```

阶段开始时：

```text
stage_total = candidate_pairs_total
stage_total_known = true
stage_processed = 0
```

每完成一个候选对，无论通过或拒绝：

```text
validated_candidate_pairs += 1
stage_processed = validated_candidate_pairs
```

`active_validation_threads` 只统计正在执行任务的线程，不包含等待队列的线程。

### 9.8 进度条显示

dHash 报告界面增加阶段名称：

```text
并行校验 dHash 汉明距离
```

进度条覆盖文本：

```text
已校验 123456 / 500000（24.69%） · 活动线程 8 / 8
```

进度条下方统计：

```text
候选对 500000
已校验 123456
通过 24500
桶碰撞拒绝 98956
校验线程 8
```

要求：

- 使用现有进度条底色反色字体绘制函数。
- UI 只读取节流后的线程安全快照。
- 进度值只能单调增加且不得超过总量。
- 取消后冻结最后一次进度，显示“正在取消，等待校验线程退出”。
- 候选任务枚举期间使用不确定进度条。
- 唯一候选任务统计完成后切换到确定百分比进度条。

## 10. 进度和日志

调整为七阶段进度：

1. `streaming_mysql_visual_contents`
   - 读取 MySQL 有效视觉内容。
   - 建立本次候选桶。
2. `grouping_identical_visual_signatures`
   - 折叠相同图片 dHash。
   - 建立签名成员映射。
3. `enumerating_dhash_candidates`
   - 分桶召回。
   - 规范化并去重候选签名对。
   - 持久化候选任务并统计总量。
4. `validating_dhash_distances`
   - 使用配置的工作线程并行执行真实距离复核。
   - 写入通过校验的规范化签名边。
   - 发布已校验对数、通过数、拒绝数和活动线程数。
5. `grouping_strict_similarity`
   - 执行严格主分组。
   - 解析边两端主组。
   - 区分组内边和跨组边。
6. `joining_active_paths`
   - 展开主组成员。
   - 保存跨组关系签名的活动成员。
7. `writing_similarity_groups`
   - 写正式主组。
   - 写跨组关系。
   - 写元数据并原子发布。

执行日志增加：

```text
image_max_hamming_distance
validation_worker_threads
有效图片内容数
不同图片 dHash 签名数
桶原始命中数
唯一候选签名对数
真实汉明距离比较数
并行校验阶段耗时
校验线程创建失败数
桶碰撞拒绝数
distance <= n 的签名边数
组内签名边数
跨组签名边数
涉及跨组关系的签名数
无活动路径的关系端点数
发布的严格主组数
发布的跨组关系数
最大单组跨组关系数
报告总耗时
```

报告生成失败时继续写入独立失败日志，至少包含：

- generation ID。
- 阶段。
- RocksDB 键类别。
- 当前签名代表 SHA-512。
- 候选签名代表 SHA-512。
- dHash。
- n。
- 配置线程数。
- 错误信息。

## 11. 预计修改文件

| 文件 | 修改内容 |
| --- | --- |
| `DedupCore/config/AppConfig.h` | 增加 dHash 汉明距离校验线程数配置 |
| `DedupCore/config/JsonConfigStore.cpp` | 保存、读取和迁移线程数配置 |
| `DedupCore/config/ConfigValidator.cpp` | 校验线程数范围 |
| `DedupCore/dedup/DuplicateReportService.h` | 增加报告元数据、跨组关系摘要、并行进度字段和范围加载接口 |
| `DedupCore/dedup/DuplicateReportService.cpp` | 候选任务持久化、有界线程池、真实复核、严格分组、签名边、跨组索引、成员持久化、元数据和发布逻辑 |
| `VideoScGUI/VideoScApp.h` | 增加关系范围缓存、展开状态和异步缩略图状态 |
| `VideoScGUI/VideoScApp.cpp` | 线程数配置控件、并行校验进度、元数据显示、跨组关系虚拟列表、按需成员和预览加载、旧报告提示 |
| `DedupTests/main.cpp` | 增加并行校验、候选无漏召回、严格组、跨组关系、持久化、取消和兼容测试 |

预计不修改：

- `DedupCore/dedup/DHashSimilarity.cpp` 的动态分段数学规则。
- `DedupCore/deletion/DeletionService.*`。
- MySQL 表结构。
- 图片 dHash 生成算法。
- 视频六帧 dHash 相似规则。

如果现有报告编码无法兼容扩展，只提升相似报告元数据和关系的独立 codec version，不改动 SHA-512 精确报告 codec。

## 12. 测试计划

### 12.1 多线程正确性

使用同一固定数据集分别配置：

```text
1 线程
2 线程
4 线程
最大允许线程数
```

验收：

- 候选任务总数一致。
- 通过和拒绝数量一致。
- 规范化签名边集合完全一致。
- 严格主组及稳定组 ID 完全一致。
- 跨组关系集合完全一致。
- 线程数只影响耗时，不影响结果。

### 12.2 并发异常和取消

- 线程池创建到一半失败时，已创建线程全部退出并 `join`。
- RocksDB 读取或写入失败时停止全部工作线程。
- 任一工作线程抛出异常时不发布半成品 generation。
- 大候选队列中取消能够唤醒生产者和工作线程。
- 取消后进度不再增长。
- 上一代 active 报告保持不变。

### 12.3 分桶完备性

对全部允许的 `n`：

- 构造汉明距离为 `0..n` 的大量随机 dHash 对。
- 验证至少命中一个同序号分段桶。
- 验证候选签名只进入一个任务。
- 验证真实距离复核通过。

构造距离 `> n` 但命中至少一个桶的样本：

- 验证进入候选任务。
- 验证被真实距离复核拒绝。
- 验证不写签名边。
- 验证不触发候选组完整扫描。

### 12.4 严格链式关系

构造：

```text
A-B <= n
B-C <= n
A-C > n
```

验收：

- `A、B、C` 不会全部进入同一个主组。
- 每个主组内任意两项距离都 `<= n`。
- `A-B` 和 `B-C` 都能从主组成员或跨组关系中查询。
- 单签名组 `C` 的活动成员可以从 `B-C` 关系展开。

### 12.5 重叠团关系

- 每个签名只进入一个主组。
- 所有跨主组直接相似签名边都被保存。
- 关系方向索引两端都能加载。
- 主列表可释放空间不重复累计。

### 12.6 热门相同 dHash

构造数百个不同 SHA-512 内容具有相同 dHash：

- 只建立一个签名节点。
- 不生成成员两两边。
- 全部活动路径可按需展开。
- 关系记录数量不随路径笛卡尔积增长。

再增加另一个距离 `<= n` 的热门 dHash：

- 两个签名间只保存一个规范化边。
- 两端关系索引各保存一条引用。
- 展开后可查看双方全部活动路径。

### 12.7 报告持久化

- 未发布 generation 不可见。
- 取消后清理正式半成品、工作键、候选任务和候选桶。
- 发布后主组、元数据、关系计数和签名成员均可读取。
- 新 generation 发布后删除上一代。
- “删除 dHash 重复报告”同时清理关系和元数据。

### 12.8 旧报告兼容

- 无 metadata 的旧相似报告显示重新生成提示。
- 不同关系规则版本不得按当前结果解释。
- 报告 `n` 与当前配置不一致时显示实际生成阈值。
- SHA-512 精确报告加载不受影响。

### 12.9 GUI 性能和进度

使用以下数据测试：

- 单个严格组数百个成员。
- 单组数千条跨组签名关系。
- 单个签名数百个活动文件。
- 数百万唯一候选签名对。

验收：

- 打开详情窗口不一次性加载全部关系和缩略图。
- 滚动时只加载可见范围及少量预取范围。
- 图片预览继续使用临时目录，关闭应用后清理。
- 表格列可调宽。
- 长文本单行裁剪，不自动换行。
- 切换组后旧关系缓存和纹理及时释放。
- 并行校验进度持续更新，不阻塞 ImGui 帧循环。
- 已校验数最终严格等于候选任务总数。
- 活动线程数不超过配置值。

## 13. 验收标准

1. 所有进入本次索引的有效图片 dHash 对，只要距离 `<= n`，就不会因分桶而漏召回。
2. 每个严格主组内任意两个不同 dHash 的汉明距离都 `<= n`。
3. 每个图片签名只归属一个严格主组。
4. 因严格完整链接约束而分处不同组的直接相似签名，可以在任一端详情中查询。
5. 相同 dHash 的大量文件不会产生平方级关系记录。
6. 单签名严格组只要参与跨组关系，其活动成员仍可被查看。
7. 跨组关系不计入可释放空间，不直接参与删除选择。
8. 报告显示生成时的 `n` 和规则版本，不再固定显示 `< 5`。
9. 旧报告、配置阈值不一致、无活动路径和无效 dHash 均有明确提示或日志。
10. 取消或失败不会发布半成品，也不会覆盖上一份有效报告。
11. dHash 汉明距离候选校验使用配置数量的有界工作线程并行执行。
12. 线程数为 1 和大于 1 时生成的签名边、严格主组和跨组关系完全一致。
13. 并行校验阶段在进度条显示已完成候选对数、总数、百分比和活动线程数。
14. 取消或线程异常后所有工作线程均安全退出，不残留后台线程。
15. Debug/Release x64 构建成功，相关自动化测试全部通过。

## 14. 执行顺序

收到“执行”命令后按以下顺序实施：

1. 确认 dHash 校验线程配置是否独立，以及默认值和范围。
2. 增加线程数配置、保存、迁移、校验和配置页面控件。
3. 增加报告元数据和跨组关系存储模型。
4. 将候选召回与真实距离校验拆为候选任务阶段和并行校验阶段。
5. 实现有界队列、工作线程异常汇总、取消和安全 `join`。
6. 调整严格分组候选来源，保留单线程完整链接校验。
7. 组织组内/跨组签名关系和单签名端点。
8. 扩展活动路径连接和正式报告发布。
9. 增加报告存储范围加载接口。
10. 修改 GUI 并行校验进度、元数据、关系表格和按需加载。
11. 增加日志和七阶段进度映射。
12. 补齐单线程/多线程一致性、异常、取消和性能测试。
13. 执行 Debug/Release x64 构建及相关测试，记录未通过项和原因。

## 15. 执行结果

本计划已于 2026-07-16 执行，实际采用以下冻结配置：

- 独立固定 `dhash_similarity.validation_worker_threads`。
- 默认值 `4`。
- 有效范围 `1–256`。
- 仅作用于 dHash 报告候选真实距离校验，不复用扫描阶段自动计算线程。

已完成：

- 七阶段报告流程。
- 候选任务持久化和有界工作队列。
- 多线程真实汉明距离校验、异常汇总、取消和安全 `join`。
- 单线程确定性严格完整链接分组。
- 跨严格组图片直接相似关系和单签名端点成员持久化。
- 报告元数据、旧报告拒绝和配置阈值差异提示。
- GUI 线程配置、进度条线程统计、跨组关系与对方图片成员按需加载。
- 配置、快照、严格链式关系、元数据、关系和签名成员持久化自动化测试。

验证结果：

- Debug x64 全解决方案构建成功。
- Release x64 全解决方案构建成功。
- Debug 自动化测试 `37/37` 通过。
- Release 自动化测试 `37/37` 通过。

未在自动化环境连接真实 MySQL 大数据集执行多线程性能基准；该项保留为实际数据验收。
