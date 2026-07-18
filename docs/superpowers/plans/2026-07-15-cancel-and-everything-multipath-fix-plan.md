# 取消扫描状态与 Everything 多路径性能修复计划

> 日期：2026-07-15  
> 状态：已执行完成（Debug/Release 构建及测试均通过）

## 1. 问题现象

1. 点击“取消扫描”后，扫描窗口中的动画进度条仍继续运动，看起来取消没有生效。
2. 配置两个扫描路径后，文件发现明显变慢，表现得像没有使用 Everything，或 Everything 查询参数不正确。
3. 当前扫描窗口只显示全局发现数量，无法确认每个路径实际使用了 Everything、Native，还是发生了回退。

## 2. 根因结论

### 2.1 取消请求没有立即进入 GUI 快照

`ScanCoordinator::Cancel()` 当前只做两件事：

```cpp
cancel_requested_.store(true);
hash_scheduler_->RequestCancel();
```

它没有更新 `ProgressSnapshot`。GUI 仍能读到原来的 `Discovering` 或 `Syncing` 阶段，而这两个阶段的进度条使用 `-ImGui::GetTime()` 持续播放不确定动画，因此点击取消后动画不会立即停止。

扫描协调线程的异常出口还有第二个状态缺口：

1. `catch (const std::exception&)` 能判断 `failurePhase == Cancelled`。
2. 随后只调用 `SaveCheckpoint(Cancelled)`。
3. `SaveCheckpoint()` 只把传入阶段写入 RocksDB 检查点，不会更新内存中的 `progress_.phase`。

因此在发现阶段或媒体阶段通过异常路径结束取消时，任务即使已经停止，GUI 快照也可能永久停留在取消前的活动阶段。

### 2.2 Everything 没有实现真正的多路径批量查询

`ScanCoordinator` 当前为每个扫描根目录创建一个发现线程，每个线程单独调用一次：

```cpp
EverythingFileDiscovery::Enumerate(root, ...)
```

Everything SDK 使用进程级全局查询状态，现有实现用 `query_mutex` 防止查询参数互相覆盖。这说明两个路径不会直接覆盖彼此的 `SetSearch` 参数，但也造成：

- 路径 A 完整查询和处理结束前，路径 B 无法进入 Everything 查询。
- 两个路径不是一次索引批量查询，而是两次串行的完整路径匹配查询。
- 官方文档明确说明 `Everything_SetMatchPath(TRUE)` 会带来显著性能开销；当前每增加一个路径都会重复一次该开销。

因此用户观察是成立的：当前实现虽然调用了 Everything，但多路径执行方式没有发挥 Everything 批量索引查询的优势。

### 2.3 Everything 互斥范围包含了大量非 SDK 慢操作

`query_mutex` 当前从查询开始一直持有到整个根目录枚举结束。锁内不仅包含 Everything SDK 调用，还包含：

- 每个结果的路径对象和模型构建。
- 媒体类型分类。
- HDD 启用物理区间优化时，每个文件调用 `QueryFilePhysicalLocation`。
- `visitor` 回调中的 RocksDB 查询、写入和增量复用判断。

这些操作不是 Everything 全局状态的一部分，却阻塞其他路径的查询。尤其 `QueryFilePhysicalLocation` 和 RocksDB 单文件处理会让第二个路径长时间停在等待状态，看起来像退回了 Native 递归扫描。

### 2.4 Everything 首次启动存在双路径竞态

两个根目录线程会先并发执行 `EnsureReady()`，之后才获取 `query_mutex`。当 Everything 尚未启动时，两个线程可能同时：

- 检查 IPC 窗口。
- 启动 `Everything.exe -startup`。
- 等待数据库加载。

其中一路可能因 IPC/数据库状态变化而失败，随后按现有逻辑回退 Native。这个准备阶段最长可等待启动 30 秒和数据库加载 120 秒，并且目前不检查扫描取消标志。

### 2.5 查询路径格式基本有效，但仍有边界缺口

当前目录查询使用：

```text
"E:\path\"
Everything_SetMatchPath(TRUE)
```

该形式符合 Everything 官方“包含路径并用引号处理空格”的递归搜索方式，不是主要错误来源。但仍有以下问题：

1. 没有在查询中增加 `file:`，Everything 会返回文件夹后再由代码过滤。
2. 单文件扫描根也会被强制追加反斜杠，必然得到零结果并回退 Native。
3. 返回结果没有再次执行严格的根目录包含校验，完全依赖 Everything 的路径子串匹配。
4. 发生回退时只保留第一条全局警告，扫描窗口没有显示每个路径的实际发现器和耗时。

## 3. 推荐修复方案

## 3.1 取消状态分为“已请求”和“最终完成”

在 `ProgressSnapshot` 增加：

```cpp
bool cancellation_requested = false;
```

处理规则：

1. `ScanCoordinator::Cancel()` 仅在任务仍运行时设置原子取消标志和快照中的 `cancellation_requested`。
2. GUI 下一帧检测到该字段后立即停止动画，显示静态提示：

```text
正在取消，等待当前文件或外部调用安全退出……
```

3. 取消按钮同时禁用，避免重复提交取消请求。
4. 协调线程真正退出后，将 `progress_.phase` 明确设置为 `Cancelled`，再保存检查点。
5. 标准异常出口在调用 `SaveCheckpoint(Cancelled)` 前同步更新内存阶段，修复快照与 RocksDB 检查点不一致。
6. 不把“请求取消”和“已经完全退出”混为同一个阶段，不新增会影响检查点枚举值兼容性的 `Cancelling` 枚举。

## 3.2 Everything 改为一次多根目录批量查询

为 `EverythingFileDiscovery` 增加多根目录入口，例如：

```cpp
EnumerateRoots(const std::vector<DiscoveryRoot>& roots, ...)
```

目录查询组合为一个 Everything OR 查询：

```text
file: <"E:\path-a\"|"E:\path-b\">
```

执行规则：

1. 一次查询覆盖本次扫描配置的全部 Everything 根目录，避免每个路径重复完整路径匹配。
2. 返回结果按规范化绝对路径重新匹配到扫描根。
3. 路径互相嵌套时，使用配置列表中靠前的根目录优先级，避免当前多线程到达顺序造成优先级不确定。
4. 单文件根目录使用不带尾部反斜杠的精确路径项。
5. `file:` 在索引侧排除文件夹，减少无效返回和本地过滤。
6. 对 Everything 没有结果或查询失败的根目录单独回退 Native，不影响同批次中已成功的其他根目录。
7. 已请求取消时禁止进入 Native 回退。

## 3.3 缩小 Everything SDK 互斥范围

SDK 查询按固定小页执行，推荐每页 `4096` 条：

1. 获取 `query_mutex`。
2. 设置当前批量搜索串、请求字段、页偏移。
3. 执行 `Everything_QueryW(TRUE)`。
4. 把该页路径、大小、创建时间、修改时间复制到本地页缓冲。
5. 释放 `query_mutex`。
6. 在锁外进行根目录匹配、物理位置查询、媒体分类和 RocksDB visitor 回调。

这样互斥只保护 Everything 的全局查询状态，不再保护磁盘 IOCTL 和 RocksDB 操作。固定小页还能限制临时内存，并在页与页之间及时响应取消。

## 3.4 串行化 Everything 准备流程并支持取消

`EnsureReady()` 增加取消感知，并使用独立准备互斥：

- 同一进程只允许一个线程负责加载 DLL、启动 Everything 和等待数据库。
- 其他线程复用准备结果，不重复启动 `Everything.exe`。
- 启动和数据库轮询每次都检查取消标志。
- 被用户取消时返回“Cancelled”结果，不记录为 Everything 故障，也不触发 Native 回退。

## 3.5 扫描窗口显示每个路径的实际发现状态

在 `ProgressSnapshot` 增加只读的根目录发现快照，扫描窗口增加紧凑表格：

| 扫描路径 | 实际发现器 | 状态 | 已发现 | 耗时 |
| --- | --- | --- | ---: | ---: |
| `E:\path-a` | Everything | 处理索引结果 | 12,430 | 1.2 秒 |
| `E:\path-b` | Everything → Native | Native 回退 | 830 | 8.4 秒 |

状态至少区分：

- 等待 Everything。
- 查询索引。
- 处理索引结果。
- 查询 HDD 物理位置。
- Native 回退。
- 已完成。
- 正在取消/已取消。
- 失败。

回退原因保留在该路径行中，并继续使用现有警告弹窗显示详细信息。这样可以直接确认每个路径是否使用 Everything，以及慢在索引查询、HDD 物理位置还是 Native 回退。

## 4. HDD 物理区间优化说明

当前配置默认启用 `hdd_extent_optimization`。如果扫描根位于 HDD，Everything 返回索引后仍会为每个文件查询物理位置，用于后续电梯排序。这属于真实磁盘/文件系统操作，不能达到纯索引查询的瞬时速度。

本次推荐：

- 保留现有配置值和默认行为，不擅自改变哈希读取策略。
- 把物理位置查询移出 Everything SDK 锁。
- 在每路径状态中明确显示“查询 HDD 物理位置”，避免被误认为没有使用 Everything。
- 若用户更重视文件发现速度，可在设置页关闭现有“HDD 物理区间优化”选项；关闭后不再逐文件查询物理位置，但后续 HDD 哈希读取的寻道顺序可能变差。

## 5. 修改范围

### `DedupCore/orchestration/ProgressSnapshot.h`

- 增加取消请求状态。
- 增加每扫描根目录的发现器、阶段、计数、耗时和回退原因快照。

### `DedupCore/orchestration/ScanCoordinator.h/.cpp`

- 取消请求立即写入快照。
- 所有取消出口统一设置最终 `Cancelled` 阶段。
- Everything 模式改为多根目录批量调用。
- Native 模式继续使用现有逐根目录发现逻辑。
- 维护每个根目录的发现状态与计数。

### `DedupCore/discovery/EverythingFileDiscovery.h/.cpp`

- 增加多根目录批量查询接口。
- 增加查询项构建、严格根路径匹配和结果路由。
- 准备流程串行化并响应取消。
- SDK 查询改为小页复制，慢操作移到互斥之外。
- 取消时不回退 Native。

### `VideoScGUI/VideoScApp.cpp`

- 取消请求后立即停止动画并显示“正在取消”。
- 禁用重复取消按钮。
- 增加每路径发现状态表格。

### `DedupTests/main.cpp`

- 增加取消请求立即进入快照、最终进入 `Cancelled` 的测试。
- 扩展端到端增量扫描测试，验证单路径发现后端、终态和逐路径文件计数。
- 将端到端扫描固定为 Native，避免测试结果依赖本机 Everything 索引状态。
- 保留现有增量扫描、进度和异常边界测试。

## 6. 不修改范围

- 不修改 SHA-512、dHash、FFmpeg 和重复报告算法。
- 不修改 RocksDB/MySQL schema。
- 不修改扫描路径优先级语义。
- 不默认关闭 HDD 物理区间优化，除非用户明确要求优先发现速度。
- 不使用多个 Everything SDK 实例绕开全局状态；仍由一个互斥边界保护 SDK 调用。

## 7. 验收标准

1. 在发现阶段点击取消后，下一帧进度动画停止，按钮禁用并显示“正在取消”。
2. 协调线程退出后阶段显示“已取消”，检查点与内存快照一致。
3. 取消发生在 Everything 启动、数据库加载、索引分页、HDD 位置查询或 Native 回退阶段时都能安全退出。
4. 配置两个 Everything 路径时只执行一次组合索引查询，不再每路径重复全路径匹配。
5. 两个路径均能在扫描窗口看到实际发现器、阶段、发现数量和耗时。
6. 某一路径不在 Everything 索引中时，只该路径回退 Native，其他路径保持 Everything 结果。
7. 含空格、中文、盘根、普通目录和单文件根路径查询正确。
8. 嵌套扫描根不会重复提交文件，路径优先级稳定。
9. `Debug|x64`、`Release|x64` 构建成功，`DedupTests` 全部通过。

## 8. 参考依据

- Everything 官方 SDK 文档说明 `Everything_SetMatchPath(TRUE)` 会搜索完整路径，同时会带来显著性能开销：<https://www.voidtools.com/support/everything/sdk/everything_setmatchpath/>
- Everything 官方搜索文档说明 `|` 可组合 OR 查询、`file:` 可限制只返回文件、带路径的搜索可限制到目录：<https://www.voidtools.com/en-us/support/everything/searching/>
- Everything 官方 SDK 文档说明 `Everything_Query` 使用当前全局搜索状态和请求参数：<https://www.voidtools.com/support/everything/sdk/everything_query/>

## 9. 确认与执行结果

用户已确认：

1. 增加“每路径实际发现器/状态/文件数/耗时/回退原因”表格。
2. 保持 `hdd_extent_optimization` 现有配置和默认开启行为。
3. 直接执行修改。

实际完成：

1. `Cancel()` 会立即发布 `cancellation_requested`，逐路径状态同步进入“正在取消”，GUI 停止不确定动画并禁用重复取消。
2. 协调线程取消异常出口会同步设置内存阶段为 `Cancelled`，不再只写 RocksDB 检查点。
3. Everything 使用单次 `file: <root-a|root-b>` 批量查询，多根结果按规范化路径和配置优先级路由。
4. Everything SDK 锁仅覆盖查询及页数据复制；媒体分类、物理位置查询和 RocksDB 回调在锁外执行。
5. Everything 初始化、启动和数据库等待使用独立互斥并检查取消标志。
6. 单个根 Everything 失败或返回零文件时只回退该根；取消后不进入 Native 回退。
7. 扫描窗口已增加逐路径发现状态表，保留现有详细警告弹窗。
8. `Debug|x64` 与 `Release|x64` 全解决方案构建成功。
9. Debug 与 Release 的 `DedupTests` 均为 `32/32 passed`。
